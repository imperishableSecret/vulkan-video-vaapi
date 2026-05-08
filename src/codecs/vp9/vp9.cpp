#include "vp9.h"

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif
#include <gst/codecparsers/gstvp9parser.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {

enum Vp9SegmentFeature {
    vp9_seg_level_alt_q = 0,
    vp9_seg_level_alt_lf = 1,
    vp9_seg_level_ref_frame = 2,
    vp9_seg_level_skip = 3,
};

struct VP9State {
    ~VP9State() {
        if (parser != nullptr) {
            gst_vp9_parser_free(parser);
        }
    }

    bool has_pic = false;
    bool has_slice = false;
    bool has_slice_data = false;
    bool has_header = false;

    GstVp9Parser *parser = nullptr;
    VADecPictureParameterBufferVP9 pic{};
    VASliceParameterBufferVP9 slice{};
    std::vector<uint8_t> bitstream;
    VkvvVP9FrameHeader header{};
};

bool copy_first_element(const VkvvBuffer *buffer, void *dst, size_t dst_size) {
    if (buffer == nullptr || buffer->data == nullptr || buffer->size < dst_size || buffer->num_elements == 0) {
        return false;
    }
    std::memcpy(dst, buffer->data, dst_size);
    return true;
}

void copy_segmentation_features(
        const GstVp9SegmentationInfo &segmentation,
        VkvvVP9FrameHeader *header) {
    std::memset(header->segment_feature_enabled, 0, sizeof(header->segment_feature_enabled));
    std::memset(header->segment_feature_data, 0, sizeof(header->segment_feature_data));

    for (uint32_t segment = 0; segment < VKVV_VP9_SEGMENT_COUNT; segment++) {
        const GstVp9SegmentationInfoData &src = segmentation.data[segment];
        uint8_t enabled = 0;
        if (src.alternate_quantizer_enabled) {
            enabled |= 1u << vp9_seg_level_alt_q;
            header->segment_feature_data[segment][vp9_seg_level_alt_q] = src.alternate_quantizer;
        }
        if (src.alternate_loop_filter_enabled) {
            enabled |= 1u << vp9_seg_level_alt_lf;
            header->segment_feature_data[segment][vp9_seg_level_alt_lf] = src.alternate_loop_filter;
        }
        if (src.reference_frame_enabled) {
            enabled |= 1u << vp9_seg_level_ref_frame;
            header->segment_feature_data[segment][vp9_seg_level_ref_frame] =
                static_cast<int16_t>(src.reference_frame);
        }
        if (src.reference_skip) {
            enabled |= 1u << vp9_seg_level_skip;
        }
        header->segment_feature_enabled[segment] = enabled;
    }
}

bool copy_gst_header(
        VP9State *vp9,
        const GstVp9FrameHdr &src,
        VkvvVP9FrameHeader *header) {
    if (vp9 == nullptr || vp9->parser == nullptr || header == nullptr) {
        return false;
    }

    *header = {};
    header->valid = true;
    header->show_existing_frame = src.show_existing_frame != 0;
    header->color_range = vp9->parser->color_range == GST_VP9_CR_FULL;
    header->segmentation_update_data = src.segmentation.update_data != 0;
    header->segmentation_abs_or_delta_update = src.segmentation.abs_delta != 0;
    header->loop_filter_delta_enabled = src.loopfilter.mode_ref_delta_enabled != 0;
    header->loop_filter_delta_update = src.loopfilter.mode_ref_delta_update != 0;
    header->use_prev_frame_mvs = !src.error_resilient_mode && !src.frame_parallel_decoding_mode;
    header->profile = static_cast<uint8_t>(src.profile);
    header->bit_depth = static_cast<uint8_t>(
        vp9->parser->bit_depth != 0 ? vp9->parser->bit_depth : vp9->pic.bit_depth);
    header->subsampling_x = static_cast<uint8_t>(vp9->parser->subsampling_x);
    header->subsampling_y = static_cast<uint8_t>(vp9->parser->subsampling_y);
    header->color_space = static_cast<uint8_t>(vp9->parser->color_space);
    header->frame_type = static_cast<uint8_t>(src.frame_type);
    header->refresh_frame_flags = src.frame_type == GST_VP9_KEY_FRAME ?
                                  0xff :
                                  static_cast<uint8_t>(src.refresh_frame_flags);
    header->show_frame = src.show_frame != 0;
    header->interpolation_filter = static_cast<uint8_t>(src.mcomp_filter_type);
    header->base_q_idx = src.quant_indices.y_ac_qi;
    header->delta_q_y_dc = src.quant_indices.y_dc_delta;
    header->delta_q_uv_dc = src.quant_indices.uv_dc_delta;
    header->delta_q_uv_ac = src.quant_indices.uv_ac_delta;
    header->frame_header_length_in_bytes = src.frame_header_length_in_bytes;
    header->first_partition_size = src.first_partition_size;

    std::copy(std::begin(src.loopfilter.ref_deltas), std::end(src.loopfilter.ref_deltas),
              header->loop_filter_ref_deltas);
    std::copy(std::begin(src.loopfilter.mode_deltas), std::end(src.loopfilter.mode_deltas),
              header->loop_filter_mode_deltas);
    copy_segmentation_features(src.segmentation, header);
    return true;
}

bool parse_vp9_frame_header(VP9State *vp9, char *reason, size_t reason_size) {
    if (vp9 == nullptr || vp9->parser == nullptr || !vp9->has_pic ||
        !vp9->has_slice_data || vp9->bitstream.empty()) {
        std::snprintf(reason, reason_size, "missing VP9 parser input");
        return false;
    }

    GstVp9FrameHdr frame_header{};
    const GstVp9ParserResult result = gst_vp9_parser_parse_frame_header(
        vp9->parser, &frame_header, vp9->bitstream.data(), vp9->bitstream.size());
    if (result != GST_VP9_PARSER_OK) {
        std::snprintf(reason, reason_size,
                      "failed to parse VP9 uncompressed frame header: parser_result=%d bytes=%zu first=%02x%02x%02x%02x va_frame=%u show=%u header=%u first_partition=%u",
                      result,
                      vp9->bitstream.size(),
                      vp9->bitstream.size() > 0 ? vp9->bitstream[0] : 0,
                      vp9->bitstream.size() > 1 ? vp9->bitstream[1] : 0,
                      vp9->bitstream.size() > 2 ? vp9->bitstream[2] : 0,
                      vp9->bitstream.size() > 3 ? vp9->bitstream[3] : 0,
                      vp9->pic.pic_fields.bits.frame_type,
                      vp9->pic.pic_fields.bits.show_frame,
                      vp9->pic.frame_header_length_in_bytes,
                      vp9->pic.first_partition_size);
        return false;
    }

    if (!copy_gst_header(vp9, frame_header, &vp9->header)) {
        std::snprintf(reason, reason_size, "failed to copy parsed VP9 frame header");
        return false;
    }
    vp9->has_header = true;
    return true;
}

bool supported_profile_depth(uint8_t profile, uint8_t bit_depth) {
    return (profile == 0 && bit_depth == 8) ||
           (profile == 2 && bit_depth == 10);
}

} // namespace

void *vkvv_vp9_state_create(void) {
    try {
        auto *state = new VP9State();
        state->parser = gst_vp9_parser_new();
        if (state->parser == nullptr) {
            delete state;
            return nullptr;
        }
        return state;
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

void vkvv_vp9_state_destroy(void *state) {
    delete static_cast<VP9State *>(state);
}

void vkvv_vp9_begin_picture(void *state) {
    auto *vp9 = static_cast<VP9State *>(state);
    if (vp9 == nullptr) {
        return;
    }

    vp9->has_pic = false;
    vp9->has_slice = false;
    vp9->has_slice_data = false;
    vp9->has_header = false;
    vp9->pic = {};
    vp9->slice = {};
    vp9->bitstream.clear();
    vp9->header = {};
}

VAStatus vkvv_vp9_render_buffer(void *state, const VkvvBuffer *buffer) {
    auto *vp9 = static_cast<VP9State *>(state);
    if (vp9 == nullptr || buffer == nullptr) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    switch (buffer->type) {
        case VAPictureParameterBufferType:
            if (!copy_first_element(buffer, &vp9->pic, sizeof(vp9->pic))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            vp9->has_pic = true;
            return VA_STATUS_SUCCESS;

        case VASliceParameterBufferType:
            if (!copy_first_element(buffer, &vp9->slice, sizeof(vp9->slice))) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            vp9->has_slice = true;
            return VA_STATUS_SUCCESS;

        case VASliceDataBufferType: {
            if (!vp9->has_slice || buffer->data == nullptr) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const size_t total_size = static_cast<size_t>(buffer->size) * buffer->num_elements;
            const size_t slice_offset = vp9->slice.slice_data_offset;
            const size_t slice_size = vp9->slice.slice_data_size;
            if (slice_offset > total_size || slice_size > total_size - slice_offset || slice_size == 0) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const auto *data = static_cast<const uint8_t *>(buffer->data);
            vp9->bitstream.insert(vp9->bitstream.end(),
                                  data + slice_offset,
                                  data + slice_offset + slice_size);
            vp9->has_slice_data = !vp9->bitstream.empty();
            return vp9->has_slice_data ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
        }

        default:
            return VA_STATUS_SUCCESS;
    }
}

VAStatus vkvv_vp9_prepare_decode(void *state, unsigned int *width, unsigned int *height, char *reason, size_t reason_size) {
    auto *vp9 = static_cast<VP9State *>(state);
    if (vp9 == nullptr) {
        std::snprintf(reason, reason_size, "missing VP9 state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!vp9->has_pic) {
        std::snprintf(reason, reason_size, "missing VP9 picture parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!vp9->has_slice) {
        std::snprintf(reason, reason_size, "missing VP9 slice parameters");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!vp9->has_slice_data) {
        std::snprintf(reason, reason_size, "missing VP9 slice data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!supported_profile_depth(vp9->pic.profile, vp9->pic.bit_depth)) {
        std::snprintf(reason, reason_size,
                      "VP9 path currently supports only Profile0 8-bit and Profile2 10-bit 4:2:0");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (!vp9->pic.pic_fields.bits.subsampling_x || !vp9->pic.pic_fields.bits.subsampling_y) {
        std::snprintf(reason, reason_size, "VP9 path currently supports only 4:2:0");
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (vp9->pic.frame_width == 0 || vp9->pic.frame_height == 0) {
        std::snprintf(reason, reason_size, "VP9 picture has empty dimensions");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!parse_vp9_frame_header(vp9, reason, reason_size)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (vp9->header.show_existing_frame) {
        std::snprintf(reason, reason_size, "VP9 show-existing-frame is not implemented yet");
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (vp9->header.profile != vp9->pic.profile ||
        vp9->header.bit_depth != vp9->pic.bit_depth ||
        vp9->header.subsampling_x != vp9->pic.pic_fields.bits.subsampling_x ||
        vp9->header.subsampling_y != vp9->pic.pic_fields.bits.subsampling_y) {
        std::snprintf(reason, reason_size,
                      "VP9 VA/header mismatch: va profile=%u depth=%u sub=%u/%u header profile=%u depth=%u sub=%u/%u",
                      vp9->pic.profile, vp9->pic.bit_depth,
                      vp9->pic.pic_fields.bits.subsampling_x,
                      vp9->pic.pic_fields.bits.subsampling_y,
                      vp9->header.profile, vp9->header.bit_depth,
                      vp9->header.subsampling_x, vp9->header.subsampling_y);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    const uint32_t compressed_header_offset = vp9->header.frame_header_length_in_bytes;
    const uint32_t tiles_offset = compressed_header_offset + vp9->header.first_partition_size;
    if (compressed_header_offset > vp9->bitstream.size() ||
        tiles_offset > vp9->bitstream.size()) {
        std::snprintf(reason, reason_size,
                      "VP9 parsed header offsets exceed bitstream: compressed=%u tiles=%u bytes=%zu va_header=%u va_first_partition=%u",
                      compressed_header_offset, tiles_offset, vp9->bitstream.size(),
                      vp9->pic.frame_header_length_in_bytes, vp9->pic.first_partition_size);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    *width = vp9->pic.frame_width;
    *height = vp9->pic.frame_height;
    std::snprintf(reason, reason_size,
                  "captured VP9 picture: %ux%u profile=%u depth=%u bytes=%zu refresh=0x%02x frame=%u show=%u q=%u header=%u first_partition=%u va_header=%u va_first_partition=%u",
                  *width, *height, vp9->pic.profile, vp9->pic.bit_depth, vp9->bitstream.size(),
                  vp9->header.refresh_frame_flags,
                  vp9->header.frame_type,
                  vp9->header.show_frame,
                  vp9->header.base_q_idx,
                  vp9->header.frame_header_length_in_bytes,
                  vp9->header.first_partition_size,
                  vp9->pic.frame_header_length_in_bytes,
                  vp9->pic.first_partition_size);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vp9_get_decode_input(void *state, VkvvVP9DecodeInput *input) {
    if (input == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    auto *vp9 = static_cast<VP9State *>(state);
    if (vp9 == nullptr || !vp9->has_pic || !vp9->has_slice || !vp9->has_slice_data || !vp9->has_header) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    *input = {};
    input->pic = &vp9->pic;
    input->slice = &vp9->slice;
    input->bitstream = vp9->bitstream.data();
    input->bitstream_size = vp9->bitstream.size();
    input->header = vp9->header;
    return VA_STATUS_SUCCESS;
}
