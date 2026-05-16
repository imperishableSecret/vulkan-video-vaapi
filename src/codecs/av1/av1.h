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
#define VKVV_AV1_MIN_DPB_SLOTS          9
#define VKVV_AV1_MIN_ACTIVE_REFERENCES  7
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
    int8_t   frame_to_show_map_idx;
    uint8_t  skip_mode_frame[2];
    int8_t   ref_frame_idx[VKVV_AV1_ACTIVE_REFERENCE_COUNT];
    uint8_t  order_hints[VKVV_AV1_REFERENCE_COUNT];
    uint8_t  ref_frame_sign_bias[VKVV_AV1_REFERENCE_COUNT];
    uint32_t current_frame_id;
    uint32_t display_frame_id;
    uint32_t order_hint;
    int32_t  expected_frame_id[VKVV_AV1_ACTIVE_REFERENCE_COUNT];
    uint32_t frame_header_offset;
} VkvvAV1FrameHeader;

typedef struct {
    bool     valid;
    bool     still_picture;
    bool     reduced_still_picture_header;
    bool     frame_id_numbers_present_flag;
    bool     use_128x128_superblock;
    bool     enable_filter_intra;
    bool     enable_intra_edge_filter;
    bool     enable_interintra_compound;
    bool     enable_masked_compound;
    bool     enable_warped_motion;
    bool     enable_order_hint;
    bool     enable_dual_filter;
    bool     enable_jnt_comp;
    bool     enable_ref_frame_mvs;
    bool     enable_superres;
    bool     enable_cdef;
    bool     enable_restoration;
    bool     film_grain_params_present;
    bool     timing_info_present_flag;
    bool     equal_picture_interval;
    bool     initial_display_delay_present_flag;
    bool     mono_chrome;
    bool     color_range;
    bool     separate_uv_delta_q;
    bool     color_description_present_flag;
    uint8_t  seq_profile;
    uint8_t  bit_depth;
    uint8_t  frame_width_bits_minus_1;
    uint8_t  frame_height_bits_minus_1;
    uint16_t max_frame_width_minus_1;
    uint16_t max_frame_height_minus_1;
    uint8_t  delta_frame_id_length_minus_2;
    uint8_t  additional_frame_id_length_minus_1;
    int8_t   order_hint_bits_minus_1;
    uint8_t  seq_force_integer_mv;
    uint8_t  seq_force_screen_content_tools;
    uint8_t  subsampling_x;
    uint8_t  subsampling_y;
    uint8_t  chroma_sample_position;
    uint8_t  color_primaries;
    uint8_t  transfer_characteristics;
    uint8_t  matrix_coefficients;
    uint32_t num_units_in_display_tick;
    uint32_t time_scale;
    uint32_t num_ticks_per_picture_minus_1;
} VkvvAV1SequenceHeader;

typedef struct {
    VASliceParameterBufferAV1 param;
    uint32_t                  tile_index;
    uint32_t                  offset;
    uint32_t                  size;
    bool                      has_va_range;
    uint32_t                  va_slice_index;
    uint32_t                  va_offset;
    uint32_t                  va_size;
    uint32_t                  va_data_offset;
    uint32_t                  va_data_size;
    bool                      has_parsed_range;
    uint32_t                  parsed_obu_index;
    uint32_t                  parsed_obu_offset;
    uint32_t                  parsed_obu_size;
    uint32_t                  parsed_payload_offset;
    uint32_t                  parsed_entry_offset;
    uint32_t                  parsed_entry_size;
} VkvvAV1Tile;

typedef struct {
    const VADecPictureParameterBufferAV1* pic;
    const VkvvAV1Tile*                    tiles;
    size_t                                tile_count;
    const uint8_t*                        bitstream;
    size_t                                bitstream_size;
    uint32_t                              decode_window_offset;
    VkvvAV1FrameHeader                    header;
    VkvvAV1SequenceHeader                 sequence;
    uint8_t                               bit_depth;
    unsigned int                          rt_format;
    unsigned int                          fourcc;
    uint32_t                              frame_width;
    uint32_t                              frame_height;
    const VkvvAV1Tile*                    va_tiles;
    size_t                                va_tile_count;
    const VkvvAV1Tile*                    parsed_tiles;
    size_t                                parsed_tile_count;
    const char*                           tile_source;
    const char*                           tile_selection_reason;
    bool                                  parser_used;
    int                                   parser_status;
    uint32_t                              selected_obu_type;
    uint32_t                              tile_group_count;
    bool                                  tile_ranges_equivalent;
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
