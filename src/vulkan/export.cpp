#include "vulkan/export/internal.h"
#include "va/surface_import.h"
#include "telemetry.h"

#include <algorithm>
#include <cstdio>
#include <unistd.h>

using namespace vkvv;

bool vkvv_vulkan_surface_has_predecode_export(const VkvvSurface* surface) {
    if (surface == nullptr || surface->vulkan == nullptr) {
        return false;
    }
    const auto* resource = static_cast<const SurfaceResource*>(surface->vulkan);
    return resource->export_resource.image != VK_NULL_HANDLE && resource->export_resource.memory != VK_NULL_HANDLE && resource->export_resource.predecode_exported;
}

VAStatus vkvv_vulkan_prepare_surface_export(void* runtime_ptr, VkvvSurface* surface, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export preparation")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!runtime->surface_export || runtime->get_memory_fd == nullptr) {
        std::snprintf(reason, reason_size, "Vulkan runtime does not support dma-buf surface export");
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (surface == nullptr) {
        std::snprintf(reason, reason_size, "missing surface for export preparation");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    const ExportFormatInfo* format = export_format_for_surface(surface, nullptr, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    char     drain_reason[512] = {};
    VAStatus drain_status      = drain_pending_work_before_sync_command(runtime, drain_reason, sizeof(drain_reason));
    if (drain_status != VA_STATUS_SUCCESS) {
        std::snprintf(reason, reason_size, "%s", drain_reason);
        return drain_status;
    }

    VkExtent2D extent{
        round_up_16(std::max(1u, surface->width)),
        round_up_16(std::max(1u, surface->height)),
    };
    if (!ensure_export_only_surface_resource(surface, format, extent, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    if (!resource->exportable && !ensure_export_resource(runtime, resource, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    vkvv_trace("export-prepare",
               "surface=%u driver=%llu stream=%llu codec=0x%x decoded=%u exportable=%u shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu "
               "retained=%zu retained_mem=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               surface->decoded ? 1U : 0U, resource->exportable ? 1U : 0U, vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation), runtime_retained_export_count(runtime),
               static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));

    if (drain_reason[0] != '\0') {
        std::snprintf(reason, reason_size,
                      "surface export resource ready: driver=%llu surface=%u stream=%llu codec=0x%x format=%s visible=%ux%u coded=%ux%u vk_format=%d va_fourcc=0x%x exportable=%u "
                      "shadow=%u decode_mem=%llu export_mem=%llu retained=%zu retained_mem=%llu drained=\"%s\"",
                      static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                      format->name, surface->width, surface->height, resource->coded_extent.width, resource->coded_extent.height, resource->format, resource->va_fourcc,
                      resource->exportable, resource->export_resource.image != VK_NULL_HANDLE, static_cast<unsigned long long>(resource->allocation_size),
                      static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                      static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), drain_reason);
    } else {
        std::snprintf(reason, reason_size,
                      "surface export resource ready: driver=%llu surface=%u stream=%llu codec=0x%x format=%s visible=%ux%u coded=%ux%u vk_format=%d va_fourcc=0x%x exportable=%u "
                      "shadow=%u decode_mem=%llu export_mem=%llu retained=%zu retained_mem=%llu",
                      static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                      format->name, surface->width, surface->height, resource->coded_extent.width, resource->coded_extent.height, resource->format, resource->va_fourcc,
                      resource->exportable, resource->export_resource.image != VK_NULL_HANDLE, static_cast<unsigned long long>(resource->allocation_size),
                      static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                      static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_refresh_surface_export(void* runtime_ptr, VkvvSurface* surface, bool displayable, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (runtime == nullptr || surface == nullptr || surface->vulkan == nullptr) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export refresh")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!surface->decoded) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    remember_export_seed_resource(runtime, resource);
    if (resource->export_resource.image == VK_NULL_HANDLE && resource->import.external && resource->import.fd.valid) {
        (void)attach_imported_export_resource_by_fd(runtime, resource);
    }
    if (resource->export_resource.image == VK_NULL_HANDLE) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        vkvv_trace("export-refresh-no-backing", "surface=%u driver=%llu stream=%llu codec=0x%x displayable=%u content_gen=%llu retained=%zu retained_mem=%llu", surface->id,
                   static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   displayable ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation), runtime_retained_export_count(runtime),
                   static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
        return VA_STATUS_SUCCESS;
    }

    const ExportFormatInfo* format = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    VAStatus drain_status = drain_pending_work_before_sync_command(runtime, reason, reason_size);
    if (drain_status != VA_STATUS_SUCCESS) {
        return drain_status;
    }
    vkvv_trace("export-refresh-pre",
               "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu", surface->id,
               static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation));
    uint32_t seeded_predecode_exports = 0;
    if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    vkvv_trace("export-refresh-post",
               "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu "
               "seeded_targets=%u retained=%zu retained_mem=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation), seeded_predecode_exports, runtime_retained_export_count(runtime),
               static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
    std::snprintf(reason, reason_size,
                  "refreshed exported %s shadow image after decode: driver=%llu surface=%u stream=%llu codec=0x%x export_mem=%llu retained=%zu retained_mem=%llu "
                  "source_generation=%llu shadow_generation=%llu seeded_predecode=%u",
                  format->name, static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id),
                  resource->codec_operation, static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                  static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
                  static_cast<unsigned long long>(resource->export_resource.content_generation), seeded_predecode_exports);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_export_surface(void* runtime_ptr, const VkvvSurface* surface, uint32_t flags, VADRMPRIMESurfaceDescriptor* descriptor, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!runtime->surface_export || runtime->get_memory_fd == nullptr) {
        std::snprintf(reason, reason_size, "Vulkan runtime does not support dma-buf surface export");
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (surface == nullptr || surface->vulkan == nullptr) {
        std::snprintf(reason, reason_size, "surface has no Vulkan image to export");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    const ExportFormatInfo* format = export_format_for_surface(surface, nullptr, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    const VAStatus flag_status = validate_export_flags(flags, reason, reason_size);
    if (flag_status != VA_STATUS_SUCCESS) {
        return flag_status;
    }
    if (descriptor == nullptr) {
        std::snprintf(reason, reason_size, "surface export descriptor is null");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    format         = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    VAStatus drain_status = drain_pending_work_before_sync_command(runtime, reason, reason_size);
    if (drain_status != VA_STATUS_SUCCESS) {
        return drain_status;
    }
    VkDeviceMemory             export_memory          = resource->memory;
    VkDeviceSize               export_allocation_size = resource->allocation_size;
    const VkSubresourceLayout* export_plane_layouts   = resource->plane_layouts;
    uint32_t                   export_plane_count     = resource->plane_count;
    uint64_t                   export_modifier        = resource->drm_format_modifier;
    bool                       export_has_modifier    = resource->has_drm_format_modifier;
    bool                       copied_to_shadow       = false;
    ExportResource*            exported_shadow        = nullptr;

    if (!resource->exportable) {
        if (surface->decoded) {
            const bool shadow_current           = resource->content_generation != 0 && resource->export_resource.content_generation == resource->content_generation;
            uint32_t   seeded_predecode_exports = 0;
            if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            copied_to_shadow = !shadow_current;
        } else if (!ensure_export_resource(runtime, resource, reason, reason_size)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        } else if (!seed_predecode_export_from_last_good(runtime, resource, reason, reason_size)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ExportResource* shadow = &resource->export_resource;
        export_memory          = shadow->memory;
        export_allocation_size = shadow->allocation_size;
        export_plane_layouts   = shadow->plane_layouts;
        export_plane_count     = shadow->plane_count;
        export_modifier        = shadow->drm_format_modifier;
        export_has_modifier    = shadow->has_drm_format_modifier;
        exported_shadow        = shadow;
    }
    vkvv_trace("export-before-fd",
               "surface=%u driver=%llu stream=%llu codec=0x%x decoded=%u exportable=%u export_mem=0x%llx content_gen=%llu shadow_gen=%llu predecode=%u copied=%u", surface->id,
               static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               surface->decoded ? 1U : 0U, resource->exportable ? 1U : 0U, vkvv_trace_handle(export_memory), static_cast<unsigned long long>(resource->content_generation),
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
               exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, copied_to_shadow ? 1U : 0U);

    if (export_memory == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "Vulkan surface image has no exportable memory layout");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory     = export_memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    int      fd     = -1;
    VkResult result = runtime->get_memory_fd(runtime->device, &fd_info, &fd);
    if (result != VK_SUCCESS || fd < 0) {
        if (fd >= 0) {
            close(fd);
        }
        record_vk_result(runtime, result, "vkGetMemoryFdKHR", "surface export", reason, reason_size);
        if (result == VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkGetMemoryFdKHR for surface export returned invalid fd=%d", fd);
        }
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VAStatus descriptor_status = fill_drm_prime_descriptor(surface, format, export_allocation_size, export_plane_layouts, export_plane_count, export_modifier,
                                                                 export_has_modifier, fd, descriptor, reason, reason_size);
    if (descriptor_status != VA_STATUS_SUCCESS) {
        close(fd);
        return descriptor_status;
    }
    if (exported_shadow != nullptr) {
        exported_shadow->exported = true;
        if (!surface->decoded) {
            exported_shadow->predecode_exported = true;
            register_predecode_export_resource(runtime, exported_shadow);
            vkvv_trace("export-predecode-register", "surface=%u driver=%llu stream=%llu codec=0x%x shadow_mem=0x%llx shadow_gen=%llu seeded=%u placeholder=%u", surface->id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       vkvv_trace_handle(exported_shadow->memory), static_cast<unsigned long long>(exported_shadow->content_generation),
                       exported_shadow->predecode_seeded ? 1U : 0U, exported_shadow->black_placeholder ? 1U : 0U);
        }
    }
    const VkvvFdIdentity fd_stat = vkvv_fd_identity_from_fd(fd);
    if (exported_shadow != nullptr) {
        exported_shadow->fd_stat_valid = fd_stat.valid;
        exported_shadow->fd_dev        = fd_stat.dev;
        exported_shadow->fd_ino        = fd_stat.ino;
    }
    vkvv_trace("export-fd",
               "surface=%u driver=%llu stream=%llu codec=0x%x fd=%d fd_stat=%u fd_dev=%llu fd_ino=%llu export_mem=0x%llx content_gen=%llu shadow_gen=%llu predecode=%u seeded=%u "
               "placeholder=%u seed_surface=%u seed_gen=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, fd,
               fd_stat.valid ? 1U : 0U, static_cast<unsigned long long>(fd_stat.dev), static_cast<unsigned long long>(fd_stat.ino), vkvv_trace_handle(export_memory),
               static_cast<unsigned long long>(resource->content_generation),
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
               exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U,
               exported_shadow != nullptr && exported_shadow->black_placeholder ? 1U : 0U, exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->seed_source_generation : 0));

    std::snprintf(
        reason, reason_size,
        "exported %s dma-buf%s: driver=%llu surface=%u stream=%llu codec=0x%x %ux%u fd=%d size=%u modifier=0x%llx y_pitch=%u uv_pitch=%u decode_mem=%llu export_mem=%llu "
        "retained=%zu retained_mem=%llu source_generation=%llu shadow_generation=%llu predecode=%u seeded=%u placeholder=%s seed_source=%u seed_generation=%llu",
        format->name, copied_to_shadow ? " via shadow copy" : "", static_cast<unsigned long long>(resource->driver_instance_id), surface->id,
        static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, surface->width, surface->height, fd, descriptor->objects[0].size,
        static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier), descriptor->layers[0].pitch[0], descriptor->layers[1].pitch[0],
        static_cast<unsigned long long>(resource->allocation_size), static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
        static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
        static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
        exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U,
        exported_shadow != nullptr && exported_shadow->black_placeholder ? "black" : "none", exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
        static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->seed_source_generation : 0));
    return VA_STATUS_SUCCESS;
}

void vkvv_vulkan_note_surface_created(void* runtime_ptr, const VkvvSurface* surface) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (runtime == nullptr || surface == nullptr) {
        return;
    }

    char                    reason[128] = {};
    const ExportFormatInfo* format      = export_format_for_surface(surface, nullptr, reason, sizeof(reason));
    const VkExtent2D        coded_extent{
        round_up_16(std::max(1u, surface->width)),
        round_up_16(std::max(1u, surface->height)),
    };
    prune_detached_exports_for_surface(runtime, surface->driver_instance_id, surface->id, surface->stream_id, static_cast<VkVideoCodecOperationFlagsKHR>(surface->codec_operation),
                                       surface->fourcc, format != nullptr ? format->vk_format : VK_FORMAT_UNDEFINED, coded_extent);
}

void vkvv_vulkan_prune_driver_exports(void* runtime_ptr, uint64_t driver_instance_id) {
    prune_detached_exports_for_driver(static_cast<VulkanRuntime*>(runtime_ptr), driver_instance_id);
}
