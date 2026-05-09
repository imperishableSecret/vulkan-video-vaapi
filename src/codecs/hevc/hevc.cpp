#include "hevc.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

    struct HEVCState {
        bool                                    has_pic             = false;
        bool                                    has_iq              = false;
        bool                                    has_slice_params    = false;
        bool                                    has_slice_data      = false;
        bool                                    has_picture_ext     = false;
        bool                                    has_slice_ext       = false;
        uint32_t                                slice_count         = 0;
        uint32_t                                reference_count     = 0;

        VAPictureParameterBufferHEVC            pic{};
        VAIQMatrixBufferHEVC                    iq{};
        std::vector<VASliceParameterBufferHEVC> slices;
        std::vector<uint8_t>                    bitstream;
        std::vector<uint32_t>                   slice_offsets;
        uint32_t                                first_slice_data_offset      = 0;
        uint32_t                                first_slice_data_size        = 0;
        uint32_t                                first_slice_data_byte_offset = 0;
        uint8_t                                 first_slice_nal0             = 0;
        uint8_t                                 first_slice_nal1             = 0;
        bool                                    has_first_slice_nal          = false;
    };

    bool copy_first_element(const VkvvBuffer* buffer, void* dst, size_t dst_size) {
        if (buffer == nullptr || buffer->data == nullptr || buffer->size < dst_size || buffer->num_elements == 0) {
            return false;
        }
        std::memcpy(dst, buffer->data, dst_size);
        return true;
    }

    bool hevc_picture_is_invalid(const VAPictureHEVC& picture) {
        return (picture.flags & VA_PICTURE_HEVC_INVALID) != 0 || picture.picture_id == VA_INVALID_ID;
    }

    uint32_t count_references(const VAPictureParameterBufferHEVC& pic) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < 15; i++) {
            if (!hevc_picture_is_invalid(pic.ReferenceFrames[i])) {
                count++;
            }
        }
        return count;
    }

} // namespace

void* vkvv_hevc_state_create(void) {
    try {
        return new HEVCState();
    } catch (const std::bad_alloc&) { return nullptr; }
}

void vkvv_hevc_state_destroy(void* state) {
    delete static_cast<HEVCState*>(state);
}

void vkvv_hevc_begin_picture(void* state) {
    auto* hevc = static_cast<HEVCState*>(state);
    if (hevc == nullptr) {
        return;
    }

    hevc->has_pic          = false;
    hevc->has_iq           = false;
    hevc->has_slice_params = false;
    hevc->has_slice_data   = false;
    hevc->has_picture_ext  = false;
    hevc->has_slice_ext    = false;
    hevc->slice_count      = 0;
    hevc->reference_count  = 0;
    hevc->pic              = {};
    hevc->iq               = {};
    hevc->slices.clear();
    hevc->bitstream.clear();
    hevc->slice_offsets.clear();
    hevc->first_slice_data_offset      = 0;
    hevc->first_slice_data_size        = 0;
    hevc->first_slice_data_byte_offset = 0;
    hevc->first_slice_nal0             = 0;
    hevc->first_slice_nal1             = 0;
    hevc->has_first_slice_nal          = false;
}

VAStatus vkvv_hevc_render_buffer(void* state, const VkvvBuffer* buffer) {
    auto* hevc = static_cast<HEVCState*>(state);
    if (hevc == nullptr || buffer == nullptr) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    switch (buffer->type) {
        case VAPictureParameterBufferType:
            if (buffer->size >= sizeof(VAPictureParameterBufferHEVCExtension)) {
                VAPictureParameterBufferHEVCExtension pic_ext{};
                if (!copy_first_element(buffer, &pic_ext, sizeof(pic_ext))) {
                    return VA_STATUS_ERROR_INVALID_BUFFER;
                }
                hevc->pic             = pic_ext.base;
                hevc->has_picture_ext = true;
            } else if (!copy_first_element(buffer, &hevc->pic, sizeof(hevc->pic))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            hevc->reference_count = count_references(hevc->pic);
            hevc->has_pic         = true;
            return VA_STATUS_SUCCESS;

        case VAIQMatrixBufferType:
            if (!copy_first_element(buffer, &hevc->iq, sizeof(hevc->iq))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            hevc->has_iq = true;
            return VA_STATUS_SUCCESS;

        case VASliceParameterBufferType: {
            if (buffer->data == nullptr || buffer->num_elements == 0 || buffer->size < sizeof(VASliceParameterBufferHEVC)) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const auto* base = static_cast<const uint8_t*>(buffer->data);
            for (unsigned int i = 0; i < buffer->num_elements; i++) {
                const uint8_t* item = base + (static_cast<size_t>(i) * buffer->size);
                if (buffer->size >= sizeof(VASliceParameterBufferHEVCExtension)) {
                    const auto* ext = reinterpret_cast<const VASliceParameterBufferHEVCExtension*>(item);
                    hevc->slices.push_back(ext->base);
                    hevc->has_slice_ext = true;
                } else {
                    const auto* slice = reinterpret_cast<const VASliceParameterBufferHEVC*>(item);
                    hevc->slices.push_back(*slice);
                }
            }
            hevc->slice_count += buffer->num_elements;
            hevc->has_slice_params = !hevc->slices.empty();
            return hevc->has_slice_params ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
        }

        case VASliceDataBufferType: {
            if (!hevc->has_slice_params || buffer->data == nullptr) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }

            const auto*  data       = static_cast<const uint8_t*>(buffer->data);
            const size_t total_size = static_cast<size_t>(buffer->size) * buffer->num_elements;

            for (const VASliceParameterBufferHEVC& slice : hevc->slices) {
                const size_t slice_offset = slice.slice_data_offset;
                const size_t slice_size   = slice.slice_data_size;
                if (slice_offset > total_size || slice_size == 0 || slice_size > total_size - slice_offset) {
                    return VA_STATUS_ERROR_INVALID_BUFFER;
                }

                if (hevc->slice_offsets.empty()) {
                    hevc->first_slice_data_offset      = slice.slice_data_offset;
                    hevc->first_slice_data_size        = slice.slice_data_size;
                    hevc->first_slice_data_byte_offset = slice.slice_data_byte_offset;
                    hevc->first_slice_nal0             = data[slice_offset];
                    hevc->first_slice_nal1             = slice_size > 1 ? data[slice_offset + 1] : 0;
                    hevc->has_first_slice_nal          = true;
                }
                hevc->slice_offsets.push_back(static_cast<uint32_t>(hevc->bitstream.size()));
                hevc->bitstream.insert(hevc->bitstream.end(), {0x00, 0x00, 0x01});
                hevc->bitstream.insert(hevc->bitstream.end(), data + slice_offset, data + slice_offset + slice_size);
            }
            hevc->has_slice_data = !hevc->bitstream.empty();
            return hevc->has_slice_data ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
        }

        default: return VA_STATUS_SUCCESS;
    }
}

VAStatus vkvv_hevc_prepare_decode(void* state, unsigned int* width, unsigned int* height, char* reason, size_t reason_size) {
    auto* hevc = static_cast<HEVCState*>(state);
    if (hevc == nullptr) {
        std::snprintf(reason, reason_size, "missing HEVC state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!hevc->has_pic) {
        std::snprintf(reason, reason_size, "missing HEVC picture parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!hevc->has_slice_params) {
        std::snprintf(reason, reason_size, "missing HEVC slice parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!hevc->has_slice_data) {
        std::snprintf(reason, reason_size, "missing HEVC slice data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (hevc->pic.pic_width_in_luma_samples == 0 || hevc->pic.pic_height_in_luma_samples == 0) {
        std::snprintf(reason, reason_size, "HEVC picture has empty dimensions");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (hevc->pic.bit_depth_luma_minus8 != 0 || hevc->pic.bit_depth_chroma_minus8 != 0) {
        std::snprintf(reason, reason_size, "HEVC path currently supports only Main 8-bit decode");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (hevc->pic.pic_fields.bits.chroma_format_idc != 1) {
        std::snprintf(reason, reason_size, "HEVC path currently supports only 4:2:0");
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if ((hevc->pic.CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) != 0) {
        std::snprintf(reason, reason_size, "HEVC field pictures are not supported yet");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (hevc->has_picture_ext || hevc->has_slice_ext) {
        std::snprintf(reason, reason_size, "HEVC range-extension/SCC buffers are not supported yet");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    *width  = hevc->pic.pic_width_in_luma_samples;
    *height = hevc->pic.pic_height_in_luma_samples;
    const VASliceParameterBufferHEVC* first_slice = hevc->slices.empty() ? nullptr : &hevc->slices.front();
    std::snprintf(reason, reason_size,
                  "captured HEVC picture: %ux%u slices=%zu bytes=%zu refs=%u iq=%u st_rps_bits=%u scaling=%u tiles=%u entropy_sync=%u weighted=%u/%u "
                  "lists_mod=%u range_pic_ext=%u range_slice_ext=%u slice_type=%u dep=%u saddr=%u slice_hdr_bytes=%u slice_data=%u+%u nal=%02x:%02x "
                  "refidx=%u/%u wdenom=%u/%d",
                  *width, *height, hevc->slice_offsets.size(), hevc->bitstream.size(), hevc->reference_count, hevc->has_iq ? 1U : 0U, hevc->pic.st_rps_bits,
                  hevc->pic.pic_fields.bits.scaling_list_enabled_flag, hevc->pic.pic_fields.bits.tiles_enabled_flag,
                  hevc->pic.pic_fields.bits.entropy_coding_sync_enabled_flag, hevc->pic.pic_fields.bits.weighted_pred_flag,
                  hevc->pic.pic_fields.bits.weighted_bipred_flag, hevc->pic.slice_parsing_fields.bits.lists_modification_present_flag,
                  hevc->has_picture_ext ? 1U : 0U, hevc->has_slice_ext ? 1U : 0U, first_slice != nullptr ? first_slice->LongSliceFlags.fields.slice_type : 0U,
                  first_slice != nullptr ? first_slice->LongSliceFlags.fields.dependent_slice_segment_flag : 0U, first_slice != nullptr ? first_slice->slice_segment_address : 0U,
                  hevc->first_slice_data_byte_offset, hevc->first_slice_data_offset, hevc->first_slice_data_size, hevc->has_first_slice_nal ? hevc->first_slice_nal0 : 0U,
                  hevc->has_first_slice_nal ? hevc->first_slice_nal1 : 0U, first_slice != nullptr ? first_slice->num_ref_idx_l0_active_minus1 : 0U,
                  first_slice != nullptr ? first_slice->num_ref_idx_l1_active_minus1 : 0U, first_slice != nullptr ? first_slice->luma_log2_weight_denom : 0U,
                  first_slice != nullptr ? first_slice->delta_chroma_log2_weight_denom : 0);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_hevc_get_decode_input(void* state, VkvvHEVCDecodeInput* input) {
    if (input == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    auto* hevc = static_cast<HEVCState*>(state);
    if (hevc == nullptr || !hevc->has_pic || !hevc->has_slice_params || !hevc->has_slice_data) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    *input                = {};
    input->pic            = &hevc->pic;
    input->iq             = hevc->has_iq ? &hevc->iq : nullptr;
    input->slices         = hevc->slices.data();
    input->bitstream      = hevc->bitstream.data();
    input->slice_offsets  = hevc->slice_offsets.data();
    input->bitstream_size = hevc->bitstream.size();
    input->slice_count    = static_cast<uint32_t>(hevc->slice_offsets.size());
    input->reference_count = hevc->reference_count;
    input->has_iq         = hevc->has_iq;
    return VA_STATUS_SUCCESS;
}
