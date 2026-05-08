#include "vulkan/export/internal.h"

#include <algorithm>
#include <cstdio>
#include <unistd.h>

using namespace vkvv;

VAStatus vkvv_vulkan_prepare_surface_export(
        void *runtime_ptr,
        VkvvSurface *surface,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
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
    const ExportFormatInfo *format = export_format_for_surface(surface, nullptr, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    VkExtent2D extent{
        round_up_16(std::max(1u, surface->width)),
        round_up_16(std::max(1u, surface->height)),
    };
    if (!ensure_export_only_surface_resource(surface, format, extent, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
    if (!resource->exportable && !ensure_export_resource(runtime, resource, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    std::snprintf(reason, reason_size,
                  "surface export resource ready: format=%s visible=%ux%u coded=%ux%u vk_format=%d va_fourcc=0x%x exportable=%u shadow=%u decode_mem=%llu export_mem=%llu detached=%zu detached_mem=%llu",
                  format->name,
                  surface->width, surface->height,
                  resource->coded_extent.width, resource->coded_extent.height,
                  resource->format, resource->va_fourcc, resource->exportable,
                  resource->export_resource.image != VK_NULL_HANDLE,
                  static_cast<unsigned long long>(resource->allocation_size),
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  runtime_detached_export_count(runtime),
                  static_cast<unsigned long long>(runtime_detached_export_memory_bytes(runtime)));
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_refresh_surface_export(
        void *runtime_ptr,
        VkvvSurface *surface,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
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

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
    if (resource->export_resource.image == VK_NULL_HANDLE) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    const ExportFormatInfo *format = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (!copy_surface_to_export_resource(runtime, resource, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    std::snprintf(reason, reason_size,
                  "refreshed exported %s shadow image after decode: export_mem=%llu detached=%zu detached_mem=%llu generation=%llu",
                  format->name,
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  runtime_detached_export_count(runtime),
                  static_cast<unsigned long long>(runtime_detached_export_memory_bytes(runtime)),
                  static_cast<unsigned long long>(resource->content_generation));
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_export_surface(
        void *runtime_ptr,
        const VkvvSurface *surface,
        uint32_t flags,
        VADRMPRIMESurfaceDescriptor *descriptor,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
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
    const ExportFormatInfo *format = export_format_for_surface(surface, nullptr, reason, reason_size);
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

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
    format = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    VkDeviceMemory export_memory = resource->memory;
    VkDeviceSize export_allocation_size = resource->allocation_size;
    const VkSubresourceLayout *export_plane_layouts = resource->plane_layouts;
    uint32_t export_plane_count = resource->plane_count;
    uint64_t export_modifier = resource->drm_format_modifier;
    bool export_has_modifier = resource->has_drm_format_modifier;
    bool copied_to_shadow = false;
    ExportResource *exported_shadow = nullptr;

    if (!resource->exportable) {
        if (surface->decoded) {
            const bool shadow_current =
                resource->content_generation != 0 &&
                resource->export_resource.content_generation == resource->content_generation;
            if (!copy_surface_to_export_resource(runtime, resource, reason, reason_size)) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            copied_to_shadow = !shadow_current;
        } else if (!ensure_export_resource(runtime, resource, reason, reason_size)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ExportResource *shadow = &resource->export_resource;
        export_memory = shadow->memory;
        export_allocation_size = shadow->allocation_size;
        export_plane_layouts = shadow->plane_layouts;
        export_plane_count = shadow->plane_count;
        export_modifier = shadow->drm_format_modifier;
        export_has_modifier = shadow->has_drm_format_modifier;
        exported_shadow = shadow;
    }

    if (export_memory == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "Vulkan surface image has no exportable memory layout");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = export_memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    int fd = -1;
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

    const VAStatus descriptor_status = fill_drm_prime_descriptor(
        surface, format, export_allocation_size, export_plane_layouts,
        export_plane_count, export_modifier, export_has_modifier, fd, descriptor,
        reason, reason_size);
    if (descriptor_status != VA_STATUS_SUCCESS) {
        close(fd);
        return descriptor_status;
    }
    if (exported_shadow != nullptr) {
        exported_shadow->exported = true;
    }

    std::snprintf(reason, reason_size,
                  "exported %s dma-buf%s: %ux%u fd=%d size=%u modifier=0x%llx y_pitch=%u uv_pitch=%u decode_mem=%llu export_mem=%llu detached=%zu detached_mem=%llu generation=%llu",
                  format->name, copied_to_shadow ? " via shadow copy" : "",
                  surface->width, surface->height, fd, descriptor->objects[0].size,
                  static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier),
                  descriptor->layers[0].pitch[0], descriptor->layers[1].pitch[0],
                  static_cast<unsigned long long>(resource->allocation_size),
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  runtime_detached_export_count(runtime),
                  static_cast<unsigned long long>(runtime_detached_export_memory_bytes(runtime)),
                  static_cast<unsigned long long>(resource->content_generation));
    return VA_STATUS_SUCCESS;
}

void vkvv_vulkan_note_surface_created(void *runtime_ptr, const VkvvSurface *surface) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    if (runtime == nullptr || surface == nullptr) {
        return;
    }

    char reason[128] = {};
    const ExportFormatInfo *format = export_format_for_surface(surface, nullptr, reason, sizeof(reason));
    const VkExtent2D coded_extent{
        round_up_16(std::max(1u, surface->width)),
        round_up_16(std::max(1u, surface->height)),
    };
    prune_detached_exports_for_surface(
        runtime,
        surface->driver_instance_id,
        surface->id,
        surface->fourcc,
        format != nullptr ? format->vk_format : VK_FORMAT_UNDEFINED,
        coded_extent);
}

void vkvv_vulkan_prune_driver_exports(void *runtime_ptr, uint64_t driver_instance_id) {
    prune_detached_exports_for_driver(static_cast<VulkanRuntime *>(runtime_ptr), driver_instance_id);
}
