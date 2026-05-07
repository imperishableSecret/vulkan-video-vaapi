#ifndef VKVV_VULKAN_RUNTIME_H
#define VKVV_VULKAN_RUNTIME_H

#include "h264.h"

#include <va/va_drmcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

void *vkvv_vulkan_runtime_create(char *reason, size_t reason_size);
void vkvv_vulkan_runtime_destroy(void *runtime);
bool vkvv_vulkan_supports_surface_export(void *runtime);
void *vkvv_vulkan_h264_session_create(void);
void vkvv_vulkan_h264_session_destroy(void *runtime, void *session);
VAStatus vkvv_vulkan_ensure_h264_session(void *runtime, void *session, unsigned int width, unsigned int height, char *reason, size_t reason_size);
VAStatus vkvv_vulkan_prepare_surface_export(void *runtime, VkvvSurface *surface, char *reason, size_t reason_size);
VAStatus vkvv_vulkan_refresh_surface_export(void *runtime, VkvvSurface *surface, char *reason, size_t reason_size);
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
VAStatus vkvv_vulkan_export_surface(
        void *runtime,
        const VkvvSurface *surface,
        uint32_t flags,
        VADRMPRIMESurfaceDescriptor *descriptor,
        char *reason,
        size_t reason_size);
void vkvv_vulkan_surface_destroy(void *runtime, VkvvSurface *surface);

#ifdef __cplusplus
}
#endif

#endif
