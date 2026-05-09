#ifndef VKVV_VULKAN_VP9_API_H
#define VKVV_VULKAN_VP9_API_H

#include "codecs/vp9/vp9.h"

#ifdef __cplusplus
extern "C" {
#endif

void*    vkvv_vulkan_vp9_session_create(void);
void*    vkvv_vulkan_vp9_profile2_session_create(void);
void     vkvv_vulkan_vp9_session_destroy(void* runtime, void* session);
VAStatus vkvv_vulkan_ensure_vp9_session(void* runtime, void* session, unsigned int width, unsigned int height, char* reason, size_t reason_size);
VAStatus vkvv_vulkan_decode_vp9(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvVP9DecodeInput* input,
                                char* reason, size_t reason_size);

#ifdef __cplusplus
}
#endif

#endif
