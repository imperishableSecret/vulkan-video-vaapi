#ifndef VKVV_AV1_H
#define VKVV_AV1_H

#include "va/driver.h"

#include <stddef.h>
#include <stdint.h>
#include <va/va_dec_av1.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKVV_AV1_REFERENCE_COUNT        8
#define VKVV_AV1_ACTIVE_REFERENCE_COUNT 7
#define VKVV_AV1_MAX_TILE_COLUMNS       64
#define VKVV_AV1_MAX_TILE_ROWS          64
#define VKVV_AV1_MAX_TILES              4096
#define VKVV_AV1_SEGMENT_COUNT          8
#define VKVV_AV1_SEGMENT_FEATURE_COUNT  8

typedef struct {
    bool     valid;
    bool     show_existing_frame;
    bool     show_frame;
    bool     showable_frame;
    bool     error_resilient_mode;
    bool     disable_cdf_update;
    bool     frame_size_override_flag;
    bool     buffer_removal_time_present_flag;
    bool     allow_intrabc;
    bool     frame_refs_short_signaling;
    bool     allow_high_precision_mv;
    bool     is_motion_mode_switchable;
    bool     use_ref_frame_mvs;
    bool     disable_frame_end_update_cdf;
    bool     allow_warped_motion;
    bool     reduced_tx_set;
    bool     render_and_frame_size_different;
    bool     use_superres;
    bool     is_filter_switchable;
    bool     skip_mode_present;
    bool     reference_select;
    uint8_t  frame_type;
    uint8_t  primary_ref_frame;
    uint8_t  refresh_frame_flags;
    uint8_t  allow_screen_content_tools;
    uint8_t  force_integer_mv;
    uint8_t  interpolation_filter;
    uint8_t  tx_mode;
    uint8_t  tile_size_bytes_minus_1;
    uint8_t  skip_mode_frame[2];
    int8_t   ref_frame_idx[VKVV_AV1_ACTIVE_REFERENCE_COUNT];
    uint8_t  order_hints[VKVV_AV1_REFERENCE_COUNT];
    uint8_t  ref_frame_sign_bias[VKVV_AV1_REFERENCE_COUNT];
    uint32_t current_frame_id;
    uint32_t order_hint;
    int32_t  expected_frame_id[VKVV_AV1_ACTIVE_REFERENCE_COUNT];
    uint32_t frame_header_offset;
} VkvvAV1FrameHeader;

typedef struct {
    VASliceParameterBufferAV1 param;
    uint32_t                  tile_index;
    uint32_t                  offset;
    uint32_t                  size;
} VkvvAV1Tile;

typedef struct {
    const VADecPictureParameterBufferAV1* pic;
    const VkvvAV1Tile*                    tiles;
    size_t                                tile_count;
    const uint8_t*                        bitstream;
    size_t                                bitstream_size;
    uint32_t                              decode_window_offset;
    VkvvAV1FrameHeader                    header;
    uint8_t                               bit_depth;
    unsigned int                          rt_format;
    unsigned int                          fourcc;
    uint32_t                              frame_width;
    uint32_t                              frame_height;
} VkvvAV1DecodeInput;

void*    vkvv_av1_state_create(void);
void     vkvv_av1_state_destroy(void* state);
void     vkvv_av1_begin_picture(void* state);
VAStatus vkvv_av1_render_buffer(void* state, const VkvvBuffer* buffer);
VAStatus vkvv_av1_prepare_decode(void* state, unsigned int* width, unsigned int* height, char* reason, size_t reason_size);
VAStatus vkvv_av1_get_decode_input(void* state, VkvvAV1DecodeInput* input);

#ifdef __cplusplus
}
#endif

#endif
