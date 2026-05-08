#ifndef VKVV_VULKAN_H264_API_H
#define VKVV_VULKAN_H264_API_H

#include "codecs/h264/h264.h"

#ifdef __cplusplus
extern "C" {
#endif

void *vkvv_vulkan_h264_session_create(void);
void vkvv_vulkan_h264_session_destroy(void *runtime, void *session);
VAStatus vkvv_vulkan_ensure_h264_session(
        void *runtime,
        void *session,
        unsigned int width,
        unsigned int height,
        char *reason,
        size_t reason_size);
VAStatus vkvv_vulkan_decode_h264(
        void *runtime,
        void *session,
        VkvvDriver *drv,
        VkvvContext *vctx,
        VkvvSurface *target,
        VAProfile profile,
        const VkvvH264DecodeInput *input,
        char *reason,
        size_t reason_size);

#ifdef __cplusplus
}
#endif

#endif
