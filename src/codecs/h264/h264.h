#ifndef VKVV_H264_H
#define VKVV_H264_H

#include "driver.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const VAPictureParameterBufferH264 *pic;
    const VAIQMatrixBufferH264 *iq;
    const VASliceParameterBufferH264 *last_slices;
    const uint8_t *bitstream;
    const uint32_t *slice_offsets;
    size_t bitstream_size;
    uint32_t slice_count;
    uint32_t last_slice_count;
    uint32_t pic_parameter_set_id;
    uint32_t idr_pic_id;
    uint32_t parsed_pic_order_cnt_lsb;
    int32_t parsed_delta_pic_order_cnt_bottom;
    uint16_t parsed_frame_num;
    uint8_t first_nal_unit_type;
    uint8_t first_nal_ref_idc;
    uint8_t first_slice_type;
    bool has_iq;
    bool all_slices_intra;
    bool has_slice_header;
    bool has_parsed_pic_order_cnt_lsb;
    bool parsed_field_pic_flag;
    bool parsed_bottom_field_flag;
} VkvvH264DecodeInput;

void *vkvv_h264_state_create(void);
void vkvv_h264_state_destroy(void *state);
void vkvv_h264_begin_picture(void *state);
VAStatus vkvv_h264_render_buffer(void *state, const VkvvBuffer *buffer);
VAStatus vkvv_h264_prepare_decode(void *state, unsigned int *width, unsigned int *height, char *reason, size_t reason_size);
VAStatus vkvv_h264_get_decode_input(void *state, VkvvH264DecodeInput *input);

#ifdef __cplusplus
}
#endif

#endif
