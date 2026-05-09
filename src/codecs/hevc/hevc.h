#ifndef VKVV_HEVC_H
#define VKVV_HEVC_H

#include "va/driver.h"

#include <stddef.h>
#include <stdint.h>
#include <va/va_dec_hevc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const VAPictureParameterBufferHEVC* pic;
    const VAIQMatrixBufferHEVC*         iq;
    const VASliceParameterBufferHEVC*   slices;
    const uint8_t*                      bitstream;
    const uint32_t*                     slice_offsets;
    size_t                              bitstream_size;
    uint32_t                            slice_count;
    uint32_t                            reference_count;
    bool                                has_iq;
} VkvvHEVCDecodeInput;

void*    vkvv_hevc_state_create(void);
void     vkvv_hevc_state_destroy(void* state);
void     vkvv_hevc_begin_picture(void* state);
VAStatus vkvv_hevc_render_buffer(void* state, const VkvvBuffer* buffer);
VAStatus vkvv_hevc_prepare_decode(void* state, unsigned int* width, unsigned int* height, char* reason, size_t reason_size);
VAStatus vkvv_hevc_get_decode_input(void* state, VkvvHEVCDecodeInput* input);

#ifdef __cplusplus
}
#endif

#endif
