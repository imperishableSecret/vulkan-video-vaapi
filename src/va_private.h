#ifndef VKVV_VA_PRIVATE_H
#define VKVV_VA_PRIVATE_H

#include "driver.h"

#include <cstddef>
#include <stdint.h>

inline VkvvDriver *vkvv_driver_from_ctx(VADriverContextP ctx) {
    return ctx != NULL ? static_cast<VkvvDriver *>(ctx->pDriverData) : NULL;
}

inline bool vkvv_profile_is_h264(VAProfile profile) {
    return profile == VAProfileH264Main ||
           profile == VAProfileH264High ||
           profile == VAProfileH264ConstrainedBaseline;
}

inline bool vkvv_profile_supported(const VkvvDriver *drv, VAProfile profile) {
    switch (profile) {
        case VAProfileH264Main:
        case VAProfileH264High:
        case VAProfileH264ConstrainedBaseline:
            return drv->caps.h264;
        default:
            return false;
    }
}

inline unsigned int vkvv_default_rt_format_for_profile(VAProfile profile) {
    switch (profile) {
        case VAProfileHEVCMain10:
            return VA_RT_FORMAT_YUV420_10;
        default:
            return VA_RT_FORMAT_YUV420;
    }
}

inline unsigned int vkvv_surface_fourcc_for_format(unsigned int rt_format) {
    if (rt_format & VA_RT_FORMAT_YUV420_10) {
        return VA_FOURCC_P010;
    }
    return VA_FOURCC_NV12;
}

void vkvv_log(const char *fmt, ...);
void *vkvv_get_or_create_vulkan_runtime(char *reason, size_t reason_size);
void vkvv_release_context_payload(VkvvDriver *drv, VkvvContext *vctx);
void vkvv_surface_begin_work(VkvvSurface *surface);
void vkvv_surface_complete_work(VkvvSurface *surface, VAStatus status);
bool vkvv_surface_has_pending_work(const VkvvSurface *surface);

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles);
VAStatus vkvvQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint *entrypoint_list, int *num_entrypoints);
VAStatus vkvvGetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib *attrib_list, int num_attribs);
VAStatus vkvvCreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib *attrib_list, int num_attribs, VAConfigID *config_id);
VAStatus vkvvDestroyConfig(VADriverContextP ctx, VAConfigID config_id);
VAStatus vkvvQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id, VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attrib_list, int *num_attribs);

VAStatus vkvvCreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height, VASurfaceID *surfaces, unsigned int num_surfaces, VASurfaceAttrib *attrib_list, unsigned int num_attribs);
VAStatus vkvvCreateSurfaces(VADriverContextP ctx, int width, int height, int format, int num_surfaces, VASurfaceID *surfaces);
VAStatus vkvvDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces);
VAStatus vkvvSyncSurface(VADriverContextP ctx, VASurfaceID render_target);
VAStatus vkvvQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status);
VAStatus vkvvQuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info);
VAStatus vkvvQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats);
VAStatus vkvvQuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib *attrib_list, unsigned int *num_attribs);
VAStatus vkvvExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void *descriptor);
VAStatus vkvvSyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns);

VAStatus vkvvCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void *data, VABufferID *buf_id);
VAStatus vkvvBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements);
VAStatus vkvvMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf);
VAStatus vkvvUnmapBuffer(VADriverContextP ctx, VABufferID buf_id);
VAStatus vkvvDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id);
VAStatus vkvvBufferInfo(VADriverContextP ctx, VABufferID buf_id, VABufferType *type, unsigned int *size, unsigned int *num_elements);
VAStatus vkvvMapBuffer2(VADriverContextP ctx, VABufferID buf_id, void **pbuf, uint32_t flags);

VAStatus vkvvCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID *render_targets, int num_render_targets, VAContextID *context);
VAStatus vkvvDestroyContext(VADriverContextP ctx, VAContextID context);
VAStatus vkvvBeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target);
VAStatus vkvvRenderPicture(VADriverContextP ctx, VAContextID context, VABufferID *buffers, int num_buffers);
VAStatus vkvvEndPicture(VADriverContextP ctx, VAContextID context);

#endif
