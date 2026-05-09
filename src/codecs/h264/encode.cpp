#include "encode.h"
#include "va/private.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

    struct H264EncodeState {
        bool                                       has_sequence      = false;
        bool                                       has_picture       = false;
        bool                                       has_slices        = false;
        bool                                       has_iq            = false;
        bool                                       has_frame_rate    = false;
        bool                                       has_rate_control  = false;
        bool                                       has_hrd           = false;
        bool                                       has_quality_level = false;
        bool                                       has_quantization  = false;
        bool                                       has_packed_header = false;
        VAEncSequenceParameterBufferH264           sequence{};
        VAEncPictureParameterBufferH264            picture{};
        VAIQMatrixBufferH264                       iq{};
        VAEncMiscParameterFrameRate                frame_rate{};
        VAEncMiscParameterRateControl              rate_control{};
        VAEncMiscParameterHRD                      hrd{};
        VAEncMiscParameterBufferQualityLevel       quality_level{};
        VAEncMiscParameterQuantization             quantization{};
        VAEncPackedHeaderParameterBuffer           packed_header{};
        std::vector<VAEncSliceParameterBufferH264> slices;
        std::vector<uint8_t>                       packed_header_data;
    };

    bool copy_first_element(const VkvvBuffer* buffer, void* dst, size_t dst_size) {
        if (buffer == nullptr || buffer->data == nullptr || buffer->size < dst_size || buffer->num_elements == 0) {
            return false;
        }
        std::memcpy(dst, buffer->data, dst_size);
        return true;
    }

    bool copy_slice_elements(H264EncodeState* h264, const VkvvBuffer* buffer) {
        if (h264 == nullptr || buffer == nullptr || buffer->data == nullptr || buffer->size < sizeof(VAEncSliceParameterBufferH264) || buffer->num_elements == 0) {
            return false;
        }
        const auto* slices = static_cast<const VAEncSliceParameterBufferH264*>(buffer->data);
        h264->slices.assign(slices, slices + buffer->num_elements);
        h264->has_slices = !h264->slices.empty();
        return h264->has_slices;
    }

    template <typename Payload>
    VAStatus copy_misc_payload(H264EncodeState* h264, const VAEncMiscParameterBuffer* misc, size_t total, Payload* dst, bool* present) {
        if (h264 == nullptr || misc == nullptr || dst == nullptr || present == nullptr) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (total < sizeof(VAEncMiscParameterBuffer) + sizeof(Payload)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        std::memcpy(dst, misc->data, sizeof(Payload));
        *present = true;
        return VA_STATUS_SUCCESS;
    }

    VAStatus copy_misc_parameter(H264EncodeState* h264, const VkvvBuffer* buffer) {
        if (h264 == nullptr || buffer == nullptr || buffer->data == nullptr) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        const size_t total = static_cast<size_t>(buffer->size) * buffer->num_elements;
        if (total < sizeof(VAEncMiscParameterBuffer)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        const auto* misc = static_cast<const VAEncMiscParameterBuffer*>(buffer->data);
        switch (misc->type) {
            case VAEncMiscParameterTypeFrameRate: return copy_misc_payload(h264, misc, total, &h264->frame_rate, &h264->has_frame_rate);
            case VAEncMiscParameterTypeRateControl: return copy_misc_payload(h264, misc, total, &h264->rate_control, &h264->has_rate_control);
            case VAEncMiscParameterTypeHRD: return copy_misc_payload(h264, misc, total, &h264->hrd, &h264->has_hrd);
            case VAEncMiscParameterTypeQualityLevel: return copy_misc_payload(h264, misc, total, &h264->quality_level, &h264->has_quality_level);
            case VAEncMiscParameterTypeQuantization: return copy_misc_payload(h264, misc, total, &h264->quantization, &h264->has_quantization);
            default: return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
    }

    uint8_t normalized_slice_type(const VAEncSliceParameterBufferH264& slice) {
        return slice.slice_type % 5;
    }

    VkvvH264EncodeFrameType frame_type_for_picture(const H264EncodeState* h264) {
        if (h264 == nullptr || !h264->has_picture || !h264->has_slices || h264->slices.empty()) {
            return VKVV_H264_ENCODE_FRAME_P;
        }
        if (h264->picture.pic_fields.bits.idr_pic_flag) {
            return VKVV_H264_ENCODE_FRAME_IDR;
        }

        switch (normalized_slice_type(h264->slices.front())) {
            case 2: return VKVV_H264_ENCODE_FRAME_I;
            case 1: return VKVV_H264_ENCODE_FRAME_B;
            default: return VKVV_H264_ENCODE_FRAME_P;
        }
    }

    bool contains_b_slice(const H264EncodeState* h264) {
        if (h264 == nullptr) {
            return false;
        }
        return std::any_of(h264->slices.begin(), h264->slices.end(), [](const VAEncSliceParameterBufferH264& slice) { return normalized_slice_type(slice) == 1; });
    }

    VAStatus fill_encode_input(H264EncodeState* h264, VkvvDriver* drv, VkvvContext* vctx, VkvvH264EncodeInput* input, char* reason, size_t reason_size) {
        if (h264 == nullptr || input == nullptr) {
            std::snprintf(reason, reason_size, "missing H.264 encode state");
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        if (!h264->has_sequence) {
            std::snprintf(reason, reason_size, "missing H.264 encode sequence parameters");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (!h264->has_picture) {
            std::snprintf(reason, reason_size, "missing H.264 encode picture parameters");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (!h264->has_slices) {
            std::snprintf(reason, reason_size, "missing H.264 encode slice parameters");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (h264->picture.coded_buf == VA_INVALID_ID) {
            std::snprintf(reason, reason_size, "missing H.264 encode coded buffer");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        auto* coded = static_cast<VkvvBuffer*>(vkvv_object_get(drv, h264->picture.coded_buf, VKVV_OBJECT_BUFFER));
        if (coded == nullptr || coded->buffer_class != VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT || coded->coded_payload == nullptr) {
            std::snprintf(reason, reason_size, "invalid H.264 encode coded buffer %u", h264->picture.coded_buf);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (!h264->sequence.seq_fields.bits.frame_mbs_only_flag) {
            std::snprintf(reason, reason_size, "H.264 encode only supports progressive frames");
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (h264->slices.size() != 1) {
            std::snprintf(reason, reason_size, "H.264 encode only supports one slice currently");
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (contains_b_slice(h264)) {
            std::snprintf(reason, reason_size, "H.264 encode B slices are not wired");
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }

        const unsigned int width  = static_cast<unsigned int>(h264->sequence.picture_width_in_mbs) * 16u;
        const unsigned int height = static_cast<unsigned int>(h264->sequence.picture_height_in_mbs) * 16u;
        if (width == 0 || height == 0) {
            std::snprintf(reason, reason_size, "invalid H.264 encode dimensions %ux%u", width, height);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        *input                         = {};
        input->sequence                = &h264->sequence;
        input->picture                 = &h264->picture;
        input->slices                  = h264->slices.data();
        input->iq                      = h264->has_iq ? &h264->iq : nullptr;
        input->frame_rate              = h264->has_frame_rate ? &h264->frame_rate : nullptr;
        input->rate_control            = h264->has_rate_control ? &h264->rate_control : nullptr;
        input->hrd                     = h264->has_hrd ? &h264->hrd : nullptr;
        input->quality_level           = h264->has_quality_level ? &h264->quality_level : nullptr;
        input->quantization            = h264->has_quantization ? &h264->quantization : nullptr;
        input->packed_header           = h264->has_packed_header ? &h264->packed_header : nullptr;
        input->packed_header_data      = h264->packed_header_data.empty() ? nullptr : h264->packed_header_data.data();
        input->packed_header_data_size = h264->packed_header_data.size();
        input->slice_count             = static_cast<uint32_t>(h264->slices.size());
        input->width                   = width;
        input->height                  = height;
        input->input_surface           = vctx != nullptr ? vctx->render_target : VA_INVALID_ID;
        input->reconstructed_surface   = h264->picture.CurrPic.picture_id;
        input->coded_buffer            = h264->picture.coded_buf;
        input->frame_type              = frame_type_for_picture(h264);
        input->has_iq                  = h264->has_iq;
        input->has_frame_rate          = h264->has_frame_rate;
        input->has_rate_control        = h264->has_rate_control;
        input->has_hrd                 = h264->has_hrd;
        input->has_quality_level       = h264->has_quality_level;
        input->has_quantization        = h264->has_quantization;
        input->has_packed_header       = h264->has_packed_header;
        std::snprintf(reason, reason_size, "captured H.264 encode picture: %ux%u slices=%u coded=%u frame=%u fr=%u rc=%u hrd=%u ql=%u quant=%u packed=%u", input->width,
                      input->height, input->slice_count, input->coded_buffer, input->frame_type, input->has_frame_rate ? 1U : 0U, input->has_rate_control ? 1U : 0U,
                      input->has_hrd ? 1U : 0U, input->has_quality_level ? 1U : 0U, input->has_quantization ? 1U : 0U, input->has_packed_header ? 1U : 0U);
        return VA_STATUS_SUCCESS;
    }

} // namespace

void* vkvv_h264_encode_state_create(void) {
    try {
        return new H264EncodeState();
    } catch (const std::bad_alloc&) { return nullptr; }
}

void vkvv_h264_encode_state_destroy(void* state) {
    delete static_cast<H264EncodeState*>(state);
}

void vkvv_h264_encode_begin_picture(void* state) {
    auto* h264 = static_cast<H264EncodeState*>(state);
    if (h264 == nullptr) {
        return;
    }
    h264->has_sequence      = false;
    h264->has_picture       = false;
    h264->has_slices        = false;
    h264->has_iq            = false;
    h264->has_frame_rate    = false;
    h264->has_rate_control  = false;
    h264->has_hrd           = false;
    h264->has_quality_level = false;
    h264->has_quantization  = false;
    h264->has_packed_header = false;
    h264->sequence          = {};
    h264->picture           = {};
    h264->iq                = {};
    h264->frame_rate        = {};
    h264->rate_control      = {};
    h264->hrd               = {};
    h264->quality_level     = {};
    h264->quantization      = {};
    h264->packed_header     = {};
    h264->slices.clear();
    h264->packed_header_data.clear();
}

VAStatus vkvv_h264_encode_render_buffer(void* state, const VkvvBuffer* buffer) {
    auto* h264 = static_cast<H264EncodeState*>(state);
    if (h264 == nullptr || buffer == nullptr) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    switch (buffer->type) {
        case VAEncSequenceParameterBufferType:
            if (!copy_first_element(buffer, &h264->sequence, sizeof(h264->sequence))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            h264->has_sequence = true;
            return VA_STATUS_SUCCESS;

        case VAEncPictureParameterBufferType:
            if (!copy_first_element(buffer, &h264->picture, sizeof(h264->picture))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            h264->has_picture = true;
            return VA_STATUS_SUCCESS;

        case VAEncSliceParameterBufferType: return copy_slice_elements(h264, buffer) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;

        case VAIQMatrixBufferType:
            if (!copy_first_element(buffer, &h264->iq, sizeof(h264->iq))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            h264->has_iq = true;
            return VA_STATUS_SUCCESS;

        case VAEncMiscParameterBufferType: return copy_misc_parameter(h264, buffer);

        case VAEncPackedHeaderParameterBufferType:
        case VAEncPackedHeaderDataBufferType: return VA_STATUS_ERROR_UNIMPLEMENTED;

        default: return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

VAStatus vkvv_h264_encode_prepare(void* state, VkvvDriver* drv, VkvvContext* vctx, unsigned int* width, unsigned int* height, VABufferID* coded_buffer, char* reason,
                                  size_t reason_size) {
    VkvvH264EncodeInput input{};
    VAStatus            status = vkvv_h264_encode_get_input(state, drv, vctx, &input, reason, reason_size);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }
    if (width != nullptr) {
        *width = input.width;
    }
    if (height != nullptr) {
        *height = input.height;
    }
    if (coded_buffer != nullptr) {
        *coded_buffer = input.coded_buffer;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_h264_encode_get_input(void* state, VkvvDriver* drv, VkvvContext* vctx, VkvvH264EncodeInput* input, char* reason, size_t reason_size) {
    return fill_encode_input(static_cast<H264EncodeState*>(state), drv, vctx, input, reason, reason_size);
}
