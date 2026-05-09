#ifndef VKVV_VULKAN_HEVC_API_H
#define VKVV_VULKAN_HEVC_API_H

#include "codecs/hevc/hevc.h"

#ifdef __cplusplus
extern "C" {
#endif

void*    vkvv_vulkan_hevc_session_create(void);
void*    vkvv_vulkan_hevc_main10_session_create(void);
void*    vkvv_vulkan_hevc_session_create_for_config(const VkvvConfig* config);
void     vkvv_vulkan_hevc_session_destroy(void* runtime, void* session);
VAStatus vkvv_vulkan_ensure_hevc_session(void* runtime, void* session, unsigned int width, unsigned int height, char* reason, size_t reason_size);
VAStatus vkvv_vulkan_decode_hevc(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvHEVCDecodeInput* input,
                                 char* reason, size_t reason_size);

#ifdef __cplusplus
}
#endif

#endif
