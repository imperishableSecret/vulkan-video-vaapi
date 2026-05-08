#include "va_private.h"
#include "telemetry.h"
#include "vulkan_runtime.h"

#include <new>

namespace {

constexpr unsigned int surface_attribute_count = 7;

struct LockedSurface {
    explicit LockedSurface(VkvvSurface *surface) : surface(surface) {}
    ~LockedSurface() {
        unlock();
    }

    LockedSurface(const LockedSurface &) = delete;
    LockedSurface &operator=(const LockedSurface &) = delete;

    void unlock() {
        vkvv_surface_unlock(surface);
        surface = NULL;
    }

    VkvvSurface *surface;
};

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
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surface->work_state == VKVV_SURFACE_WORK_RENDERING) {
        (void) timeout_ns;
        return VA_STATUS_ERROR_TIMEDOUT;
    }
    return surface->sync_status;
}

VAStatus complete_vulkan_surface_work(
        VkvvDriver *drv,
        VkvvSurface *surface,
        uint64_t timeout_ns) {
    if (drv == NULL || drv->vulkan == NULL || surface == NULL ||
        !vkvv_surface_has_pending_work(surface)) {
        return sync_surface_work(surface, timeout_ns);
    }

    char reason[512] = {};
    VAStatus status = vkvv_vulkan_complete_surface_work(
        drv->vulkan, surface, timeout_ns, reason, sizeof(reason));
    if (reason[0] != '\0') {
        vkvv_log("%s", reason);
    }
    if (status == VA_STATUS_ERROR_TIMEDOUT && vkvv_surface_has_pending_work(surface)) {
        return sync_surface_work(surface, timeout_ns);
    }
    return status;
}

} // namespace

void vkvv_surface_begin_work(VkvvSurface *surface) {
    if (surface == NULL) {
        return;
    }
    if (surface->destroying) {
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
    if (surface->destroying) {
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
    const unsigned int selected_format = vkvv_select_driver_rt_format(drv, format);
    if (selected_format == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    for (unsigned int i = 0; i < num_surfaces; i++) {
        auto *surface = new (std::nothrow) VkvvSurface();
        if (surface == NULL) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surface->driver_instance_id = drv->driver_instance_id;
        surface->rt_format = selected_format;
        surface->width = width;
        surface->height = height;
        surface->fourcc = vkvv_surface_fourcc_for_format(selected_format);
        surface->role_flags = VKVV_SURFACE_ROLE_DECODE_OUTPUT |
                              (drv->caps.surface_export ? VKVV_SURFACE_ROLE_EXPORTABLE : 0);
        surface->work_state = VKVV_SURFACE_WORK_READY;
        surface->sync_status = VA_STATUS_SUCCESS;
        surfaces[i] = vkvv_object_add(drv, VKVV_OBJECT_SURFACE, surface);
        if (surfaces[i] == VA_INVALID_ID) {
            delete surface;
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surface->id = surfaces[i];
        if (drv->vulkan != NULL) {
            vkvv_vulkan_note_surface_created(drv->vulkan, surface);
        }
        vkvv_log("created surface %u: driver=%llu stream=%llu codec=0x%x %ux%u fourcc=0x%x rt=0x%x",
                 surfaces[i],
                 (unsigned long long) surface->driver_instance_id,
                 (unsigned long long) surface->stream_id,
                 surface->codec_operation,
                 width,
                 height,
                 surface->fourcc,
                 surface->rt_format);
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
        auto *surface = vkvv_surface_get_locked(drv, surface_list[i]);
        if (surface == NULL) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        LockedSurface locked_surface(surface);
        surface->destroying = true;
        if (drv->vulkan != NULL) {
            if (vkvv_surface_has_pending_work(surface)) {
                (void) complete_vulkan_surface_work(drv, surface, VA_TIMEOUT_INFINITE);
            }
            vkvv_vulkan_surface_destroy(drv->vulkan, surface);
        }
        if (vkvv_surface_has_pending_work(surface)) {
            surface->work_state = VKVV_SURFACE_WORK_READY;
            surface->sync_status = VA_STATUS_ERROR_OPERATION_FAILED;
        }
        locked_surface.unlock();
        surface = NULL;
        if (!vkvv_object_remove(drv, surface_list[i], VKVV_OBJECT_SURFACE)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvSyncSurface(VADriverContextP ctx, VASurfaceID render_target) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *surface = vkvv_surface_get_locked(drv, render_target);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    LockedSurface locked_surface(surface);
    return complete_vulkan_surface_work(drv, surface, VA_TIMEOUT_INFINITE);
}

VAStatus vkvvQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *surface = vkvv_surface_get_locked(drv, render_target);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    LockedSurface locked_surface(surface);
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (vkvv_surface_has_pending_work(surface)) {
        (void) complete_vulkan_surface_work(drv, surface, 0);
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
    auto *surface = vkvv_surface_get_locked(drv, render_target);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    LockedSurface locked_surface(surface);
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    *error_info = NULL;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    *num_formats = static_cast<int>(
        vkvv_query_image_formats(drv, format_list, VKVV_MAX_IMAGE_FORMATS));
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

    set_integer_attrib(&attrib_list[0], VASurfaceAttribMinWidth, 0, static_cast<int>(cfg->min_width));
    set_integer_attrib(&attrib_list[1], VASurfaceAttribMinHeight, 0, static_cast<int>(cfg->min_height));
    set_integer_attrib(&attrib_list[2], VASurfaceAttribMaxWidth, 0, static_cast<int>(cfg->max_width));
    set_integer_attrib(&attrib_list[3], VASurfaceAttribMaxHeight, 0, static_cast<int>(cfg->max_height));
    set_integer_attrib(&attrib_list[4], VASurfaceAttribPixelFormat,
                       VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
                       static_cast<int>(cfg->fourcc));
    set_integer_attrib(&attrib_list[5], VASurfaceAttribMemoryType,
                       VA_SURFACE_ATTRIB_GETTABLE,
                       VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                           (cfg->exportable ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 : 0));

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
    VkvvLockGuard driver_state_lock(&drv->state_mutex);
    auto *surface = vkvv_surface_get_locked(drv, surface_id);
    if (surface == NULL) {
        vkvv_log("export requested for unknown surface %u", surface_id);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    LockedSurface locked_surface(surface);
    if (surface->destroying) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    vkvv_trace("va-export-enter",
               "driver=%llu surface=%u active_stream=%llu active_codec=0x%x surface_stream=%llu surface_codec=0x%x decoded=%u pending=%u",
               (unsigned long long) drv->driver_instance_id,
               surface->id,
               (unsigned long long) drv->active_decode_stream_id,
               drv->active_decode_codec_operation,
               (unsigned long long) surface->stream_id,
               surface->codec_operation,
               surface->decoded ? 1U : 0U,
               vkvv_surface_has_pending_work(surface) ? 1U : 0U);
    const bool applied_active_domain = vkvv_driver_apply_active_decode_domain_locked(drv, surface);
    if (applied_active_domain) {
        vkvv_log("tagged export surface %u from active decode domain: driver=%llu stream=%llu codec=0x%x %ux%u fourcc=0x%x rt=0x%x",
                 surface->id,
                 (unsigned long long) surface->driver_instance_id,
                 (unsigned long long) surface->stream_id,
                 surface->codec_operation,
                 surface->width,
                 surface->height,
                 surface->fourcc,
                 surface->rt_format);
    }
    vkvv_trace("va-export-domain",
               "driver=%llu surface=%u applied=%u stream=%llu codec=0x%x decoded=%u",
               (unsigned long long) drv->driver_instance_id,
               surface->id,
               applied_active_domain ? 1U : 0U,
               (unsigned long long) surface->stream_id,
               surface->codec_operation,
               surface->decoded ? 1U : 0U);
    if (vkvv_surface_has_pending_work(surface)) {
        vkvv_trace("va-export-drain",
                   "driver=%llu surface=%u stream=%llu codec=0x%x",
                   (unsigned long long) drv->driver_instance_id,
                   surface->id,
                   (unsigned long long) surface->stream_id,
                   surface->codec_operation);
        VAStatus status = complete_vulkan_surface_work(drv, surface, VA_TIMEOUT_INFINITE);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        if (vkvv_surface_has_pending_work(surface)) {
            return VA_STATUS_ERROR_TIMEDOUT;
        }
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
    VAStatus status = vkvv_vulkan_prepare_surface_export(drv->vulkan, surface, reason, sizeof(reason));
    vkvv_log("%s", reason);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }

    status = vkvv_vulkan_export_surface(
        drv->vulkan, surface, flags,
        static_cast<VADRMPRIMESurfaceDescriptor *>(descriptor),
        reason, sizeof(reason));
    vkvv_log("%s", reason);
    vkvv_trace("va-export-return",
               "driver=%llu surface=%u status=%d stream=%llu codec=0x%x decoded=%u",
               (unsigned long long) drv->driver_instance_id,
               surface->id,
               status,
               (unsigned long long) surface->stream_id,
               surface->codec_operation,
               surface->decoded ? 1U : 0U);
    return status;
}

VAStatus vkvvSyncSurface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout_ns) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *target = vkvv_surface_get_locked(drv, surface);
    if (target == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    LockedSurface locked_surface(target);
    return complete_vulkan_surface_work(drv, target, timeout_ns);
}
