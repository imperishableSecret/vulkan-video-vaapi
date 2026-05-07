#include "va_private.h"
#include "vulkan_runtime.h"

#include <cstdlib>

namespace {

constexpr int min_surface_dimension = 1;
constexpr int max_decode_surface_dimension = 4096;
constexpr unsigned int surface_attribute_count = 7;

void set_integer_attrib(VASurfaceAttrib *attrib, VASurfaceAttribType type, uint32_t flags, int value) {
    *attrib = {};
    attrib->type = type;
    attrib->flags = flags;
    attrib->value.type = VAGenericValueTypeInteger;
    attrib->value.value.i = value;
}

VASurfaceStatus va_status_for_surface(const VkvvSurface *surface) {
    if (surface != NULL && surface->work_state == VKVV_SURFACE_WORK_RENDERING) {
        return VASurfaceRendering;
    }
    return VASurfaceReady;
}

VAStatus sync_surface_work(const VkvvSurface *surface, uint64_t timeout_ns) {
    if (surface->work_state == VKVV_SURFACE_WORK_RENDERING) {
        (void) timeout_ns;
        return VA_STATUS_ERROR_TIMEDOUT;
    }
    return surface->sync_status;
}

} // namespace

void vkvv_surface_begin_work(VkvvSurface *surface) {
    if (surface == NULL) {
        return;
    }
    surface->work_state = VKVV_SURFACE_WORK_RENDERING;
    surface->sync_status = VA_STATUS_ERROR_TIMEDOUT;
    surface->decoded = false;
}

void vkvv_surface_complete_work(VkvvSurface *surface, VAStatus status) {
    if (surface == NULL) {
        return;
    }
    surface->work_state = VKVV_SURFACE_WORK_READY;
    surface->sync_status = status;
}

bool vkvv_surface_has_pending_work(const VkvvSurface *surface) {
    return surface != NULL && surface->work_state == VKVV_SURFACE_WORK_RENDERING;
}

VAStatus vkvvCreateSurfaces2(
        VADriverContextP ctx,
        unsigned int format,
        unsigned int width,
        unsigned int height,
        VASurfaceID *surfaces,
        unsigned int num_surfaces,
        VASurfaceAttrib *attrib_list,
        unsigned int num_attribs) {
    (void) attrib_list;
    (void) num_attribs;

    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    if ((format & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10)) == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    for (unsigned int i = 0; i < num_surfaces; i++) {
        auto *surface = static_cast<VkvvSurface *>(std::calloc(1, sizeof(VkvvSurface)));
        if (surface == NULL) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surface->rt_format = format;
        surface->width = width;
        surface->height = height;
        surface->fourcc = vkvv_surface_fourcc_for_format(format);
        surface->dpb_slot = -1;
        surface->work_state = VKVV_SURFACE_WORK_READY;
        surface->sync_status = VA_STATUS_SUCCESS;
        surfaces[i] = vkvv_object_add(drv, VKVV_OBJECT_SURFACE, surface);
        if (surfaces[i] == VA_INVALID_ID) {
            std::free(surface);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        vkvv_log("created surface %u: %ux%u fourcc=0x%x rt=0x%x",
                 surfaces[i], width, height, surface->fourcc, surface->rt_format);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus vkvvCreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surfaces) {
    return vkvvCreateSurfaces2(ctx, (unsigned int) format, (unsigned int) width,
                               (unsigned int) height, surfaces,
                               (unsigned int) num_surfaces, NULL, 0);
}

VAStatus vkvvDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    for (int i = 0; i < num_surfaces; i++) {
        auto *surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, surface_list[i], VKVV_OBJECT_SURFACE));
        if (surface == NULL) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (drv->vulkan != NULL) {
            vkvv_vulkan_surface_destroy(drv->vulkan, surface);
        }
        if (!vkvv_object_remove(drv, surface_list[i], VKVV_OBJECT_SURFACE)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvSyncSurface(VADriverContextP ctx, VASurfaceID render_target) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, render_target, VKVV_OBJECT_SURFACE));
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return sync_surface_work(surface, VA_TIMEOUT_INFINITE);
}

VAStatus vkvvQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, render_target, VKVV_OBJECT_SURFACE));
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (status == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    *status = va_status_for_surface(surface);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQuerySurfaceError(VADriverContextP ctx, VASurfaceID render_target, VAStatus error_status, void **error_info) {
    (void) error_status;
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    if (vkvv_object_get(drv, render_target, VKVV_OBJECT_SURFACE) == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    *error_info = NULL;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats) {
    (void) ctx;
    format_list[0] = {};
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12;
    format_list[1] = {};
    format_list[1].fourcc = VA_FOURCC_P010;
    format_list[1].byte_order = VA_LSB_FIRST;
    format_list[1].bits_per_pixel = 24;
    *num_formats = 2;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQuerySurfaceAttributes(
        VADriverContextP ctx,
        VAConfigID config,
        VASurfaceAttrib *attrib_list,
        unsigned int *num_attribs) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *cfg = static_cast<VkvvConfig *>(vkvv_object_get(drv, config, VKVV_OBJECT_CONFIG));
    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (attrib_list == NULL) {
        *num_attribs = surface_attribute_count;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < surface_attribute_count) {
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    set_integer_attrib(&attrib_list[0], VASurfaceAttribMinWidth, 0, min_surface_dimension);
    set_integer_attrib(&attrib_list[1], VASurfaceAttribMinHeight, 0, min_surface_dimension);
    set_integer_attrib(&attrib_list[2], VASurfaceAttribMaxWidth, 0, max_decode_surface_dimension);
    set_integer_attrib(&attrib_list[3], VASurfaceAttribMaxHeight, 0, max_decode_surface_dimension);
    set_integer_attrib(&attrib_list[4], VASurfaceAttribPixelFormat,
                       VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
                       static_cast<int>(vkvv_surface_fourcc_for_format(cfg->rt_format)));
    set_integer_attrib(&attrib_list[5], VASurfaceAttribMemoryType,
                       VA_SURFACE_ATTRIB_GETTABLE,
                       VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                           (drv->caps.surface_export ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 : 0));

    attrib_list[6] = {};
    attrib_list[6].type = VASurfaceAttribExternalBufferDescriptor;
    attrib_list[6].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[6].value.type = VAGenericValueTypePointer;
    attrib_list[6].value.value.p = NULL;
    *num_attribs = surface_attribute_count;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvExportSurfaceHandle(
        VADriverContextP ctx,
        VASurfaceID surface_id,
        uint32_t mem_type,
        uint32_t flags,
        void *descriptor) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    if ((mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    auto *surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, surface_id, VKVV_OBJECT_SURFACE));
    if (surface == NULL) {
        vkvv_log("export requested for unknown surface %u", surface_id);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (drv->vulkan == NULL) {
        char runtime_reason[512] = {};
        drv->vulkan = vkvv_get_or_create_vulkan_runtime(runtime_reason, sizeof(runtime_reason));
        vkvv_log("%s", runtime_reason);
        if (drv->vulkan == NULL) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    char reason[512] = {};
    if (drv->h264_export_session == NULL) {
        drv->h264_export_session = vkvv_vulkan_h264_session_create();
        if (drv->h264_export_session == NULL) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    VAStatus status = vkvv_vulkan_ensure_h264_session(
        drv->vulkan, drv->h264_export_session, surface->width, surface->height, reason, sizeof(reason));
    vkvv_log("%s", reason);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }
    status = vkvv_vulkan_prepare_surface_export(drv->vulkan, surface, reason, sizeof(reason));
    vkvv_log("%s", reason);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }

    status = vkvv_vulkan_export_surface(
        drv->vulkan, surface, flags,
        static_cast<VADRMPRIMESurfaceDescriptor *>(descriptor),
        reason, sizeof(reason));
    vkvv_log("%s", reason);
    return status;
}

VAStatus vkvvSyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *target = static_cast<VkvvSurface *>(vkvv_object_get(drv, surface, VKVV_OBJECT_SURFACE));
    if (target == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return sync_surface_work(target, timeout_ns);
}
