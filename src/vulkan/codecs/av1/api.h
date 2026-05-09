#ifndef VKVV_VULKAN_AV1_API_H
#define VKVV_VULKAN_AV1_API_H

#include "codecs/av1/av1.h"

#ifdef __cplusplus
extern "C" {
#endif

void*    vkvv_vulkan_av1_session_create(void);
void*    vkvv_vulkan_av1_p010_session_create(void);
void*    vkvv_vulkan_av1_session_create_for_config(const VkvvConfig* config);
void     vkvv_vulkan_av1_session_destroy(void* runtime, void* session);
VAStatus vkvv_vulkan_ensure_av1_session(void* runtime, void* session, unsigned int width, unsigned int height, char* reason, size_t reason_size);
VAStatus vkvv_vulkan_decode_av1(void* runtime, void* session, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvAV1DecodeInput* input,
                                char* reason, size_t reason_size);

#ifdef __cplusplus
}
#endif

#endif
