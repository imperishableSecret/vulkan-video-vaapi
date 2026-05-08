#ifndef VKVV_VP9_H
#define VKVV_VP9_H

#include "driver.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKVV_VP9_REFERENCE_COUNT 8
#define VKVV_VP9_ACTIVE_REFERENCE_COUNT 3
#define VKVV_VP9_SEGMENT_COUNT 8
#define VKVV_VP9_SEGMENT_FEATURE_COUNT 4
#define VKVV_VP9_LOOP_FILTER_REF_DELTAS 4
#define VKVV_VP9_LOOP_FILTER_MODE_DELTAS 2

typedef struct {
    bool valid;
    bool show_existing_frame;
    bool color_range;
    bool segmentation_update_data;
    bool segmentation_abs_or_delta_update;
    bool loop_filter_delta_enabled;
    bool loop_filter_delta_update;
    bool use_prev_frame_mvs;
    uint8_t profile;
    uint8_t bit_depth;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t color_space;
    uint8_t refresh_frame_flags;
    uint8_t frame_type;
    uint8_t show_frame;
    uint8_t interpolation_filter;
    uint8_t base_q_idx;
    int8_t delta_q_y_dc;
    int8_t delta_q_uv_dc;
    int8_t delta_q_uv_ac;
    uint32_t frame_header_length_in_bytes;
    uint32_t first_partition_size;
    int8_t loop_filter_ref_deltas[VKVV_VP9_LOOP_FILTER_REF_DELTAS];
    int8_t loop_filter_mode_deltas[VKVV_VP9_LOOP_FILTER_MODE_DELTAS];
    uint8_t segment_feature_enabled[VKVV_VP9_SEGMENT_COUNT];
    int16_t segment_feature_data[VKVV_VP9_SEGMENT_COUNT][VKVV_VP9_SEGMENT_FEATURE_COUNT];
} VkvvVP9FrameHeader;

typedef struct {
    const VADecPictureParameterBufferVP9 *pic;
    const VASliceParameterBufferVP9 *slice;
    const uint8_t *bitstream;
    size_t bitstream_size;
    VkvvVP9FrameHeader header;
} VkvvVP9DecodeInput;

void *vkvv_vp9_state_create(void);
void vkvv_vp9_state_destroy(void *state);
void vkvv_vp9_begin_picture(void *state);
VAStatus vkvv_vp9_render_buffer(void *state, const VkvvBuffer *buffer);
VAStatus vkvv_vp9_prepare_decode(void *state, unsigned int *width, unsigned int *height, char *reason, size_t reason_size);
VAStatus vkvv_vp9_get_decode_input(void *state, VkvvVP9DecodeInput *input);

#ifdef __cplusplus
}
#endif

#endif
