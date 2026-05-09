#include "va/private.h"
#include "va/surface_import.h"
#include "telemetry.h"
#include "vulkan/runtime.h"

#include <new>
#include <va/va_drmcommon.h>

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

const char *surface_attrib_name(VASurfaceAttribType type) {
    switch (type) {
    case VASurfaceAttribNone:
        return "None";
    case VASurfaceAttribPixelFormat:
        return "PixelFormat";
    case VASurfaceAttribMinWidth:
        return "MinWidth";
    case VASurfaceAttribMaxWidth:
        return "MaxWidth";
    case VASurfaceAttribMinHeight:
        return "MinHeight";
    case VASurfaceAttribMaxHeight:
        return "MaxHeight";
    case VASurfaceAttribMemoryType:
        return "MemoryType";
    case VASurfaceAttribExternalBufferDescriptor:
        return "ExternalBufferDescriptor";
    case VASurfaceAttribUsageHint:
        return "UsageHint";
    case VASurfaceAttribDRMFormatModifiers:
        return "DRMFormatModifiers";
    case VASurfaceAttribAlignmentSize:
        return "AlignmentSize";
    case VASurfaceAttribCount:
        return "Count";
    }
    return "unknown";
}

const char *generic_value_type_name(VAGenericValueType type) {
    switch (type) {
    case VAGenericValueTypeInteger:
        return "int";
    case VAGenericValueTypeFloat:
        return "float";
    case VAGenericValueTypePointer:
        return "ptr";
    case VAGenericValueTypeFunc:
        return "func";
    }
    return "unknown";
}

const VASurfaceAttrib *find_surface_attrib(
        const VASurfaceAttrib *attrib_list,
        unsigned int num_attribs,
        VASurfaceAttribType type) {
    if (attrib_list == NULL) {
        return NULL;
    }
    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == type) {
            return &attrib_list[i];
        }
    }
    return NULL;
}

void trace_drm_prime_descriptor(
        const VADRMPRIMESurfaceDescriptor *descriptor,
        unsigned int index) {
    if (descriptor == NULL) {
        return;
    }
    const int fd = descriptor->num_objects > 0 ? descriptor->objects[0].fd : -1;
    const VkvvFdIdentity fd_stat = vkvv_fd_identity_from_fd(fd);
    vkvv_trace("surface-import-drm-prime2",
               "index=%u fourcc=0x%x %ux%u objects=%u layers=%u fd0=%d fd0_stat=%u fd0_dev=%llu fd0_ino=%llu size0=%u mod0=0x%llx layer0_format=0x%x layer0_planes=%u layer0_object0=%u layer0_offset0=%u layer0_pitch0=%u",
               index,
               descriptor->fourcc,
               descriptor->width,
               descriptor->height,
               descriptor->num_objects,
               descriptor->num_layers,
               fd,
               fd_stat.valid ? 1U : 0U,
               static_cast<unsigned long long>(fd_stat.dev),
               static_cast<unsigned long long>(fd_stat.ino),
               descriptor->num_objects > 0 ? descriptor->objects[0].size : 0,
               descriptor->num_objects > 0 ? static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier) : 0ULL,
               descriptor->num_layers > 0 ? descriptor->layers[0].drm_format : 0,
               descriptor->num_layers > 0 ? descriptor->layers[0].num_planes : 0,
               descriptor->num_layers > 0 && descriptor->layers[0].num_planes > 0 ? descriptor->layers[0].object_index[0] : 0,
               descriptor->num_layers > 0 && descriptor->layers[0].num_planes > 0 ? descriptor->layers[0].offset[0] : 0,
               descriptor->num_layers > 0 && descriptor->layers[0].num_planes > 0 ? descriptor->layers[0].pitch[0] : 0);
}

void trace_drm_prime3_descriptor(
        const VADRMPRIME3SurfaceDescriptor *descriptor,
        unsigned int index) {
    if (descriptor == NULL) {
        return;
    }
    const int fd = descriptor->num_objects > 0 ? descriptor->objects[0].fd : -1;
    const VkvvFdIdentity fd_stat = vkvv_fd_identity_from_fd(fd);
    vkvv_trace("surface-import-drm-prime3",
               "index=%u fourcc=0x%x %ux%u objects=%u layers=%u fd0=%d fd0_stat=%u fd0_dev=%llu fd0_ino=%llu size0=%u mod0=0x%llx flags=0x%x layer0_format=0x%x layer0_planes=%u layer0_object0=%u layer0_offset0=%u layer0_pitch0=%u",
               index,
               descriptor->fourcc,
               descriptor->width,
               descriptor->height,
               descriptor->num_objects,
               descriptor->num_layers,
               fd,
               fd_stat.valid ? 1U : 0U,
               static_cast<unsigned long long>(fd_stat.dev),
               static_cast<unsigned long long>(fd_stat.ino),
               descriptor->num_objects > 0 ? descriptor->objects[0].size : 0,
               descriptor->num_objects > 0 ? static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier) : 0ULL,
               descriptor->flags,
               descriptor->num_layers > 0 ? descriptor->layers[0].drm_format : 0,
               descriptor->num_layers > 0 ? descriptor->layers[0].num_planes : 0,
               descriptor->num_layers > 0 && descriptor->layers[0].num_planes > 0 ? descriptor->layers[0].object_index[0] : 0,
               descriptor->num_layers > 0 && descriptor->layers[0].num_planes > 0 ? descriptor->layers[0].offset[0] : 0,
               descriptor->num_layers > 0 && descriptor->layers[0].num_planes > 0 ? descriptor->layers[0].pitch[0] : 0);
}

void trace_external_buffers_descriptor(
        const VASurfaceAttribExternalBuffers *descriptor,
        unsigned int index) {
    if (descriptor == NULL) {
        return;
    }
    const uintptr_t first_buffer =
            descriptor->buffers != NULL && descriptor->num_buffers > 0 ? descriptor->buffers[0] : 0;
    const int fd = first_buffer <= static_cast<uintptr_t>(INT32_MAX) ? static_cast<int>(first_buffer) : -1;
    const VkvvFdIdentity fd_stat = vkvv_fd_identity_from_fd(fd);
    vkvv_trace("surface-import-external-buffers",
               "index=%u fourcc=0x%x %ux%u size=%u planes=%u buffers=%u buffer0=%llu fd0_stat=%u fd0_dev=%llu fd0_ino=%llu flags=0x%x pitch0=%u offset0=%u pitch1=%u offset1=%u",
               index,
               descriptor->pixel_format,
               descriptor->width,
               descriptor->height,
               descriptor->data_size,
               descriptor->num_planes,
               descriptor->num_buffers,
               static_cast<unsigned long long>(first_buffer),
               fd_stat.valid ? 1U : 0U,
               static_cast<unsigned long long>(fd_stat.dev),
               static_cast<unsigned long long>(fd_stat.ino),
               descriptor->flags,
               descriptor->num_planes > 0 ? descriptor->pitches[0] : 0,
               descriptor->num_planes > 0 ? descriptor->offsets[0] : 0,
               descriptor->num_planes > 1 ? descriptor->pitches[1] : 0,
               descriptor->num_planes > 1 ? descriptor->offsets[1] : 0);
}

void trace_create_surface_attribs(
        uint64_t driver_instance_id,
        unsigned int format,
        unsigned int width,
        unsigned int height,
        unsigned int num_surfaces,
        const VASurfaceAttrib *attrib_list,
        unsigned int num_attribs) {
    if (!vkvv_trace_enabled()) {
        return;
    }
    const uint32_t memory_type = vkvv_surface_import_memory_type(attrib_list, num_attribs);
    const bool has_external = vkvv_surface_import_has_external_descriptor(attrib_list, num_attribs);
    vkvv_trace("surface-create-request",
               "driver=%llu format=0x%x %ux%u count=%u attribs=%u mem_type=0x%x external=%u",
               static_cast<unsigned long long>(driver_instance_id),
               format,
               width,
               height,
               num_surfaces,
               num_attribs,
               memory_type,
               has_external ? 1U : 0U);

    for (unsigned int i = 0; i < num_attribs; i++) {
        const VASurfaceAttrib &attrib = attrib_list[i];
        const unsigned long long pointer_value =
                attrib.value.type == VAGenericValueTypePointer ?
                        reinterpret_cast<unsigned long long>(attrib.value.value.p) :
                        0ULL;
        vkvv_trace("surface-create-attrib",
                   "driver=%llu index=%u type=%u(%s) flags=0x%x value_type=%u(%s) int=%d ptr=0x%llx",
                   static_cast<unsigned long long>(driver_instance_id),
                   i,
                   static_cast<unsigned int>(attrib.type),
                   surface_attrib_name(attrib.type),
                   attrib.flags,
                   static_cast<unsigned int>(attrib.value.type),
                   generic_value_type_name(attrib.value.type),
                   attrib.value.type == VAGenericValueTypeInteger ? attrib.value.value.i : 0,
                   pointer_value);
    }

    const VASurfaceAttrib *external =
            find_surface_attrib(attrib_list, num_attribs, VASurfaceAttribExternalBufferDescriptor);
    if (external == NULL || external->value.type != VAGenericValueTypePointer ||
        external->value.value.p == NULL) {
        return;
    }

    const unsigned int descriptors_to_log = num_surfaces < 4 ? num_surfaces : 4;
    if ((memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3) != 0) {
        const auto *descriptors =
                static_cast<const VADRMPRIME3SurfaceDescriptor *>(external->value.value.p);
        for (unsigned int i = 0; i < descriptors_to_log; i++) {
            trace_drm_prime3_descriptor(&descriptors[i], i);
        }
        return;
    }
    if ((memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) != 0) {
        const auto *descriptors =
                static_cast<const VADRMPRIMESurfaceDescriptor *>(external->value.value.p);
        for (unsigned int i = 0; i < descriptors_to_log; i++) {
            trace_drm_prime_descriptor(&descriptors[i], i);
        }
        return;
    }
    if ((memory_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) != 0) {
        const auto *descriptors =
                static_cast<const VASurfaceAttribExternalBuffers *>(external->value.value.p);
        for (unsigned int i = 0; i < descriptors_to_log; i++) {
            trace_external_buffers_descriptor(&descriptors[i], i);
        }
    }
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
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    trace_create_surface_attribs(
            drv != NULL ? drv->driver_instance_id : 0,
            format,
            width,
            height,
            num_surfaces,
            attrib_list,
            num_attribs);
    const unsigned int selected_format = vkvv_select_driver_rt_format(drv, format);
    if (selected_format == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    for (unsigned int i = 0; i < num_surfaces; i++) {
        const VkvvExternalSurfaceImport import_info =
                vkvv_surface_import_from_attribs(attrib_list, num_attribs, i);
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
        surface->import = import_info;
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
        vkvv_trace("surface-create",
                   "driver=%llu surface=%u stream=%llu codec=0x%x %ux%u fourcc=0x%x rt=0x%x role=0x%x import_mem=0x%x import_external=%u import_fd_stat=%u import_fd_dev=%llu import_fd_ino=%llu import_fourcc=0x%x import_size=%ux%u",
                   (unsigned long long) surface->driver_instance_id,
                   surfaces[i],
                   (unsigned long long) surface->stream_id,
                   surface->codec_operation,
                   width,
                   height,
                   surface->fourcc,
                   surface->rt_format,
                   surface->role_flags,
                   surface->import.memory_type,
                   surface->import.external ? 1U : 0U,
                   surface->import.fd.valid ? 1U : 0U,
                   static_cast<unsigned long long>(surface->import.fd.dev),
                   static_cast<unsigned long long>(surface->import.fd.ino),
                   surface->import.fourcc,
                   surface->import.width,
                   surface->import.height);
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
        vkvv_trace("surface-destroy",
                   "driver=%llu surface=%u stream=%llu codec=0x%x decoded=%u pending=%u destroying=%u",
                   (unsigned long long) surface->driver_instance_id,
                   surface->id,
                   (unsigned long long) surface->stream_id,
                   surface->codec_operation,
                   surface->decoded ? 1U : 0U,
                   vkvv_surface_has_pending_work(surface) ? 1U : 0U,
                   surface->destroying ? 1U : 0U);
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
