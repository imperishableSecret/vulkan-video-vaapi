#include "h264.h"
#include "codecs/storage.h"
#include "telemetry.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

    struct H264State {
        bool                                    has_pic                           = false;
        bool                                    has_iq                            = false;
        bool                                    has_slice_params                  = false;
        bool                                    has_slice_data                    = false;
        bool                                    has_slice_header                  = false;
        bool                                    has_parsed_pic_order_cnt_lsb      = false;
        bool                                    parsed_field_pic_flag             = false;
        bool                                    parsed_bottom_field_flag          = false;
        bool                                    all_slices_intra                  = true;
        uint32_t                                slice_count                       = 0;
        uint32_t                                pic_parameter_set_id              = 0;
        uint32_t                                idr_pic_id                        = 0;
        uint32_t                                parsed_pic_order_cnt_lsb          = 0;
        int32_t                                 parsed_delta_pic_order_cnt_bottom = 0;
        uint16_t                                parsed_frame_num                  = 0;
        uint8_t                                 first_nal_unit_type               = 0;
        uint8_t                                 first_nal_ref_idc                 = 0;
        uint8_t                                 first_slice_type                  = 0;

        VAPictureParameterBufferH264            pic{};
        VAIQMatrixBufferH264                    iq{};
        std::vector<VASliceParameterBufferH264> slices;
        std::vector<uint8_t>                    bitstream;
        std::vector<uint32_t>                   slice_offsets;
        std::vector<uint8_t>                    rbsp_scratch;
        uint32_t                                slices_underused_frames        = 0;
        uint32_t                                bitstream_underused_frames     = 0;
        uint32_t                                slice_offsets_underused_frames = 0;
        uint32_t                                rbsp_underused_frames          = 0;
    };

    bool copy_first_element(const VkvvBuffer* buffer, void* dst, size_t dst_size) {
        if (buffer == nullptr || buffer->data == nullptr || buffer->size < dst_size || buffer->num_elements == 0) {
            return false;
        }
        std::memcpy(dst, buffer->data, dst_size);
        return true;
    }

    bool slice_is_intra(uint8_t slice_type) {
        const uint8_t normalized = slice_type % 5;
        return normalized == 2 || normalized == 4;
    }

    class BitReader {
      public:
        explicit BitReader(const std::vector<uint8_t>& rbsp) : rbsp_(rbsp) {}

        bool read_bits(uint32_t count, uint32_t* value) {
            if (count > 32 || value == nullptr || bit_offset_ + count > rbsp_.size() * 8) {
                return false;
            }

            uint32_t out = 0;
            for (uint32_t i = 0; i < count; i++) {
                const size_t   byte_offset = bit_offset_ / 8;
                const uint32_t bit_in_byte = 7u - static_cast<uint32_t>(bit_offset_ % 8);
                out                        = (out << 1) | ((rbsp_[byte_offset] >> bit_in_byte) & 1u);
                bit_offset_++;
            }
            *value = out;
            return true;
        }

        bool read_bit(bool* value) {
            uint32_t bit = 0;
            if (!read_bits(1, &bit) || value == nullptr) {
                return false;
            }
            *value = bit != 0;
            return true;
        }

        bool read_ue(uint32_t* value) {
            if (value == nullptr) {
                return false;
            }

            uint32_t leading_zero_bits = 0;
            while (true) {
                uint32_t bit = 0;
                if (!read_bits(1, &bit)) {
                    return false;
                }
                if (bit != 0) {
                    break;
                }
                leading_zero_bits++;
                if (leading_zero_bits > 31) {
                    return false;
                }
            }

            uint32_t suffix = 0;
            if (leading_zero_bits > 0 && !read_bits(leading_zero_bits, &suffix)) {
                return false;
            }
            *value = ((1u << leading_zero_bits) - 1u) + suffix;
            return true;
        }

        bool read_se(int32_t* value) {
            uint32_t code_num = 0;
            if (value == nullptr || !read_ue(&code_num)) {
                return false;
            }
            const int32_t magnitude = static_cast<int32_t>((code_num + 1u) / 2u);
            *value                  = (code_num & 1u) != 0 ? magnitude : -magnitude;
            return true;
        }

      private:
        const std::vector<uint8_t>& rbsp_;
        size_t                      bit_offset_ = 0;
    };

    bool parse_h264_slice_header(const uint8_t* data, size_t size, const VAPictureParameterBufferH264& pic, H264State* h264) {
        if (data == nullptr || h264 == nullptr || size < 2) {
            return false;
        }

        if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            data += 4;
            size -= 4;
        } else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
            data += 3;
            size -= 3;
        }
        if (size < 2) {
            return false;
        }

        const uint8_t nal_header  = data[0];
        h264->first_nal_ref_idc   = (nal_header >> 5) & 0x3;
        h264->first_nal_unit_type = nal_header & 0x1f;

        std::vector<uint8_t>& rbsp = h264->rbsp_scratch;
        rbsp.clear();
        rbsp.reserve(size - 1);
        uint32_t zero_count = 0;
        for (size_t i = 1; i < size; i++) {
            const uint8_t byte = data[i];
            if (zero_count >= 2 && byte == 0x03) {
                zero_count = 0;
                continue;
            }
            rbsp.push_back(byte);
            if (byte == 0) {
                zero_count++;
            } else {
                zero_count = 0;
            }
        }
        if (rbsp.empty()) {
            return false;
        }

        BitReader reader(rbsp);
        uint32_t  first_mb_in_slice    = 0;
        uint32_t  slice_type           = 0;
        uint32_t  pic_parameter_set_id = 0;
        if (!reader.read_ue(&first_mb_in_slice) || !reader.read_ue(&slice_type) || !reader.read_ue(&pic_parameter_set_id)) {
            return false;
        }

        if (pic.seq_fields.bits.residual_colour_transform_flag) {
            uint32_t colour_plane_id = 0;
            if (!reader.read_bits(2, &colour_plane_id)) {
                return false;
            }
        }

        uint32_t       frame_num      = 0;
        const uint32_t frame_num_bits = pic.seq_fields.bits.log2_max_frame_num_minus4 + 4u;
        if (!reader.read_bits(frame_num_bits, &frame_num)) {
            return false;
        }

        bool field_pic_flag    = false;
        bool bottom_field_flag = false;
        if (!pic.seq_fields.bits.frame_mbs_only_flag) {
            if (!reader.read_bit(&field_pic_flag)) {
                return false;
            }
            if (field_pic_flag && !reader.read_bit(&bottom_field_flag)) {
                return false;
            }
        }

        uint32_t idr_pic_id = 0;
        if (h264->first_nal_unit_type == 5 && !reader.read_ue(&idr_pic_id)) {
            return false;
        }

        uint32_t pic_order_cnt_lsb          = 0;
        int32_t  delta_pic_order_cnt_bottom = 0;
        bool     has_pic_order_cnt_lsb      = false;
        if (pic.seq_fields.bits.pic_order_cnt_type == 0) {
            const uint32_t poc_lsb_bits = pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4u;
            if (!reader.read_bits(poc_lsb_bits, &pic_order_cnt_lsb)) {
                return false;
            }
            has_pic_order_cnt_lsb = true;
            if (pic.pic_fields.bits.pic_order_present_flag && !field_pic_flag && !reader.read_se(&delta_pic_order_cnt_bottom)) {
                return false;
            }
        }

        h264->pic_parameter_set_id              = pic_parameter_set_id;
        h264->idr_pic_id                        = idr_pic_id;
        h264->parsed_pic_order_cnt_lsb          = pic_order_cnt_lsb;
        h264->parsed_delta_pic_order_cnt_bottom = delta_pic_order_cnt_bottom;
        h264->parsed_frame_num                  = static_cast<uint16_t>(frame_num);
        h264->first_slice_type                  = static_cast<uint8_t>(slice_type);
        h264->has_parsed_pic_order_cnt_lsb      = has_pic_order_cnt_lsb;
        h264->parsed_field_pic_flag             = field_pic_flag;
        h264->parsed_bottom_field_flag          = bottom_field_flag;
        h264->has_slice_header                  = true;
        return true;
    }

} // namespace

void* vkvv_h264_state_create(void) {
    try {
        return new H264State();
    } catch (const std::bad_alloc&) { return nullptr; }
}

void vkvv_h264_state_destroy(void* state) {
    delete static_cast<H264State*>(state);
}

void vkvv_h264_begin_picture(void* state) {
    auto* h264 = static_cast<H264State*>(state);
    if (h264 == nullptr) {
        return;
    }

    h264->has_pic                           = false;
    h264->has_iq                            = false;
    h264->has_slice_params                  = false;
    h264->has_slice_data                    = false;
    h264->has_slice_header                  = false;
    h264->has_parsed_pic_order_cnt_lsb      = false;
    h264->parsed_field_pic_flag             = false;
    h264->parsed_bottom_field_flag          = false;
    h264->all_slices_intra                  = true;
    h264->slice_count                       = 0;
    h264->pic_parameter_set_id              = 0;
    h264->idr_pic_id                        = 0;
    h264->parsed_pic_order_cnt_lsb          = 0;
    h264->parsed_delta_pic_order_cnt_bottom = 0;
    h264->parsed_frame_num                  = 0;
    h264->first_nal_unit_type               = 0;
    h264->first_nal_ref_idc                 = 0;
    h264->first_slice_type                  = 0;
    h264->pic                               = {};
    h264->iq                                = {};
    vkvv::clear_with_capacity_hysteresis(h264->slices, h264->slices_underused_frames);
    vkvv::clear_with_capacity_hysteresis(h264->bitstream, h264->bitstream_underused_frames);
    vkvv::clear_with_capacity_hysteresis(h264->slice_offsets, h264->slice_offsets_underused_frames);
    vkvv::clear_with_capacity_hysteresis(h264->rbsp_scratch, h264->rbsp_underused_frames);
}

VAStatus vkvv_h264_render_buffer(void* state, const VkvvBuffer* buffer) {
    auto* h264 = static_cast<H264State*>(state);
    if (h264 == nullptr || buffer == nullptr) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    switch (buffer->type) {
        case VAPictureParameterBufferType:
            if (!copy_first_element(buffer, &h264->pic, sizeof(h264->pic))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            h264->has_pic = true;
            return VA_STATUS_SUCCESS;

        case VAIQMatrixBufferType:
            if (!copy_first_element(buffer, &h264->iq, sizeof(h264->iq))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            h264->has_iq = true;
            return VA_STATUS_SUCCESS;

        case VASliceParameterBufferType: {
            if (buffer->data == nullptr || buffer->size < sizeof(VASliceParameterBufferH264)) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const auto* src = static_cast<const VASliceParameterBufferH264*>(buffer->data);
            h264->slices.assign(src, src + buffer->num_elements);
            h264->has_slice_params = !h264->slices.empty();
            for (unsigned int i = 0; i < buffer->num_elements; i++) {
                h264->all_slices_intra = h264->all_slices_intra && slice_is_intra(src[i].slice_type);
            }
            h264->slice_count += buffer->num_elements;
            return h264->has_slice_params ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
        }

        case VASliceDataBufferType: {
            if (!h264->has_slice_params || buffer->data == nullptr) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }

            const auto*  data       = static_cast<const uint8_t*>(buffer->data);
            const size_t total_size = static_cast<size_t>(buffer->size) * buffer->num_elements;

            for (const VASliceParameterBufferH264& slice : h264->slices) {
                const size_t slice_offset = slice.slice_data_offset;
                const size_t slice_size   = slice.slice_data_size;
                if (slice_offset > total_size || slice_size > total_size - slice_offset) {
                    return VA_STATUS_ERROR_INVALID_BUFFER;
                }

                h264->slice_offsets.push_back(static_cast<uint32_t>(h264->bitstream.size()));
                if (!h264->has_slice_header) {
                    parse_h264_slice_header(data + slice_offset, slice_size, h264->pic, h264);
                }
                h264->bitstream.insert(h264->bitstream.end(), {0x00, 0x00, 0x01});
                h264->bitstream.insert(h264->bitstream.end(), data + slice_offset, data + slice_offset + slice_size);
            }
            h264->has_slice_data = !h264->bitstream.empty();
            return h264->has_slice_data ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
        }

        default: return VA_STATUS_SUCCESS;
    }
}

VAStatus vkvv_h264_prepare_decode(void* state, unsigned int* width, unsigned int* height, char* reason, size_t reason_size) {
    auto* h264 = static_cast<H264State*>(state);
    if (h264 == nullptr) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_CONTEXT, "missing H.264 state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!h264->has_pic) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "missing H.264 picture parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!h264->has_slice_params) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "missing H.264 slice parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!h264->has_slice_data) {
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_INVALID_BUFFER, "missing H.264 slice data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    *width  = static_cast<unsigned int>(h264->pic.picture_width_in_mbs_minus1 + 1) * 16;
    *height = static_cast<unsigned int>(h264->pic.picture_height_in_mbs_minus1 + 1) * 16;
    VKVV_SUCCESS_REASON(reason, reason_size, "captured H.264 picture: %ux%u slices=%zu bitstream=%zu bytes iq=%d slice_header=%d nal=%u pps=%u idr=%u", *width, *height,
                        h264->slice_offsets.size(), h264->bitstream.size(), h264->has_iq, h264->has_slice_header, h264->first_nal_unit_type, h264->pic_parameter_set_id,
                        h264->idr_pic_id);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_h264_get_decode_input(void* state, VkvvH264DecodeInput* input) {
    if (input == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    auto* h264 = static_cast<H264State*>(state);
    if (h264 == nullptr || !h264->has_pic || !h264->has_slice_params || !h264->has_slice_data) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    *input                                   = {};
    input->pic                               = &h264->pic;
    input->iq                                = h264->has_iq ? &h264->iq : nullptr;
    input->last_slices                       = h264->slices.data();
    input->last_slice_count                  = static_cast<uint32_t>(h264->slices.size());
    input->bitstream                         = h264->bitstream.data();
    input->bitstream_size                    = h264->bitstream.size();
    input->slice_offsets                     = h264->slice_offsets.data();
    input->slice_count                       = static_cast<uint32_t>(h264->slice_offsets.size());
    input->pic_parameter_set_id              = h264->pic_parameter_set_id;
    input->idr_pic_id                        = h264->idr_pic_id;
    input->parsed_pic_order_cnt_lsb          = h264->parsed_pic_order_cnt_lsb;
    input->parsed_delta_pic_order_cnt_bottom = h264->parsed_delta_pic_order_cnt_bottom;
    input->parsed_frame_num                  = h264->parsed_frame_num;
    input->first_nal_unit_type               = h264->first_nal_unit_type;
    input->first_nal_ref_idc                 = h264->first_nal_ref_idc;
    input->first_slice_type                  = h264->first_slice_type;
    input->has_iq                            = h264->has_iq;
    input->all_slices_intra                  = h264->all_slices_intra;
    input->has_slice_header                  = h264->has_slice_header;
    input->has_parsed_pic_order_cnt_lsb      = h264->has_parsed_pic_order_cnt_lsb;
    input->parsed_field_pic_flag             = h264->parsed_field_pic_flag;
    input->parsed_bottom_field_flag          = h264->parsed_bottom_field_flag;
    return VA_STATUS_SUCCESS;
}
