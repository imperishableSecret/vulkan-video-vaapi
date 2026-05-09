#ifndef VKVV_H264_ENCODE_H
#define VKVV_H264_ENCODE_H

#include "va/driver.h"

#include <stddef.h>
#include <stdint.h>
#include <va/va_enc_h264.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VKVV_H264_ENCODE_FRAME_IDR = 0,
    VKVV_H264_ENCODE_FRAME_I,
    VKVV_H264_ENCODE_FRAME_P,
    VKVV_H264_ENCODE_FRAME_B,
} VkvvH264EncodeFrameType;

typedef struct {
    const VAEncSequenceParameterBufferH264* sequence;
    const VAEncPictureParameterBufferH264*  picture;
    const VAEncSliceParameterBufferH264*    slices;
    const VAIQMatrixBufferH264*             iq;
    const VAEncMiscParameterRateControl*    rate_control;
    const VAEncPackedHeaderParameterBuffer* packed_header;
    const uint8_t*                          packed_header_data;
    size_t                                  packed_header_data_size;
    uint32_t                                slice_count;
    unsigned int                            width;
    unsigned int                            height;
    VASurfaceID                             input_surface;
    VASurfaceID                             reconstructed_surface;
    VABufferID                              coded_buffer;
    VkvvH264EncodeFrameType                 frame_type;
    bool                                    has_iq;
    bool                                    has_rate_control;
    bool                                    has_packed_header;
} VkvvH264EncodeInput;

void*    vkvv_h264_encode_state_create(void);
void     vkvv_h264_encode_state_destroy(void* state);
void     vkvv_h264_encode_begin_picture(void* state);
VAStatus vkvv_h264_encode_render_buffer(void* state, const VkvvBuffer* buffer);
VAStatus vkvv_h264_encode_prepare(void* state, VkvvDriver* drv, VkvvContext* vctx, unsigned int* width, unsigned int* height, VABufferID* coded_buffer, char* reason,
                                  size_t reason_size);
VAStatus vkvv_h264_encode_get_input(void* state, VkvvDriver* drv, VkvvContext* vctx, VkvvH264EncodeInput* input, char* reason, size_t reason_size);

#ifdef __cplusplus
}
#endif

#endif
