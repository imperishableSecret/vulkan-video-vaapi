#include "../vulkan_runtime_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <drm_fourcc.h>

namespace vkvv {

struct ExportLayerInfo {
    VkImageAspectFlags aspect;
    uint32_t drm_format;
    uint32_t width_divisor;
    uint32_t height_divisor;
};

struct ExportFormatInfo {
    unsigned int va_fourcc;
    VkFormat vk_format;
    const char *name;
    uint32_t layer_count;
    ExportLayerInfo layers[2];
};

constexpr ExportFormatInfo nv12_export_format = {
    VA_FOURCC_NV12,
    VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
    "NV12",
    2,
    {
        {VK_IMAGE_ASPECT_PLANE_0_BIT, DRM_FORMAT_R8, 1, 1},
        {VK_IMAGE_ASPECT_PLANE_1_BIT, DRM_FORMAT_GR88, 2, 2},
    },
};

const ExportFormatInfo *export_format_for_fourcc(unsigned int fourcc) {
    switch (fourcc) {
        case VA_FOURCC_NV12:
            return &nv12_export_format;
        default:
            return nullptr;
    }
}

const ExportFormatInfo *export_format_for_surface(
        const VkvvSurface *surface,
        const SurfaceResource *resource,
        char *reason,
        size_t reason_size) {
    if (surface == nullptr && resource == nullptr) {
        std::snprintf(reason, reason_size, "missing surface export format source");
        return nullptr;
    }
    const unsigned int fourcc = resource != nullptr ? resource->va_fourcc : surface->fourcc;
    const ExportFormatInfo *format = export_format_for_fourcc(fourcc);
    if (format == nullptr) {
        std::snprintf(reason, reason_size, "surface export does not support VA fourcc=0x%x", fourcc);
        return nullptr;
    }
    if (resource != nullptr && resource->format != format->vk_format) {
        std::snprintf(reason, reason_size,
                      "surface export format mismatch: va_fourcc=0x%x resource_format=%d expected_format=%d",
                      fourcc, resource->format, format->vk_format);
        return nullptr;
    }
    return format;
}

VkExtent3D export_layer_extent(VkExtent2D coded_extent, const ExportLayerInfo &layer) {
    return {
        (coded_extent.width + layer.width_divisor - 1) / layer.width_divisor,
        (coded_extent.height + layer.height_divisor - 1) / layer.height_divisor,
        1,
    };
}

VAStatus validate_export_flags(uint32_t flags, char *reason, size_t reason_size) {
    const uint32_t access = flags & VA_EXPORT_SURFACE_READ_WRITE;
    if (access != VA_EXPORT_SURFACE_READ_ONLY) {
        std::snprintf(reason, reason_size, "surface export requires read-only access flags");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) == 0) {
        std::snprintf(reason, reason_size, "surface export currently requires separate layers");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0) {
        std::snprintf(reason, reason_size, "surface export does not support composed layers");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    constexpr uint32_t supported_flags =
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS;
    if ((flags & ~supported_flags) != 0) {
        std::snprintf(reason, reason_size, "surface export has unsupported flags=0x%x", flags & ~supported_flags);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    return VA_STATUS_SUCCESS;
}

bool create_export_resource_with_tiling(
        VulkanRuntime *runtime,
        ExportResource *resource,
        const ExportFormatInfo *format,
        VkExtent2D extent,
        VkImageTiling tiling,
        char *reason,
        size_t reason_size) {
    if (format == nullptr) {
        std::snprintf(reason, reason_size, "missing surface export format");
        return false;
    }

    VkExternalMemoryImageCreateInfo external_image{};
    external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    std::vector<uint64_t> drm_modifiers;
    VkImageDrmFormatModifierListCreateInfoEXT drm_modifier_list{};
    drm_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        if (!enumerate_drm_format_modifiers(
                runtime,
                format->vk_format,
                VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT,
                &drm_modifiers)) {
            std::snprintf(reason, reason_size, "no DRM format modifiers support transfer-dst export images");
            return false;
        }
        drm_modifier_list.drmFormatModifierCount = static_cast<uint32_t>(drm_modifiers.size());
        drm_modifier_list.pDrmFormatModifiers = drm_modifiers.data();
        external_image.pNext = &drm_modifier_list;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_image;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format->vk_format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = tiling;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateImage for export shadow image failed: %d", result);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(runtime->device, resource->image, &requirements);

    uint32_t memory_type_index = 0;
    if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
        !find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, 0, &memory_type_index)) {
        destroy_export_resource(runtime, resource);
        std::snprintf(reason, reason_size, "no memory type for export shadow image");
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedicated_allocate{};
    dedicated_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_allocate.image = resource->image;

    VkExportMemoryAllocateInfo export_allocate{};
    export_allocate.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_allocate.pNext = &dedicated_allocate;
    export_allocate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_allocate;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type_index;

    result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &resource->memory);
    if (result != VK_SUCCESS) {
        destroy_export_resource(runtime, resource);
        std::snprintf(reason, reason_size, "vkAllocateMemory for export shadow image failed: %d", result);
        return false;
    }

    result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_export_resource(runtime, resource);
        std::snprintf(reason, reason_size, "vkBindImageMemory for export shadow image failed: %d", result);
        return false;
    }

    for (uint32_t i = 0; i < format->layer_count; i++) {
        VkImageSubresource plane{};
        plane.aspectMask = format->layers[i].aspect;
        vkGetImageSubresourceLayout(runtime->device, resource->image, &plane, &resource->plane_layouts[i]);
    }

    resource->extent = extent;
    resource->format = format->vk_format;
    resource->va_fourcc = format->va_fourcc;
    resource->allocation_size = requirements.size;
    resource->plane_count = format->layer_count;
    if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
        modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
        result = runtime->get_image_drm_format_modifier_properties(
            runtime->device, resource->image, &modifier_properties);
        if (result != VK_SUCCESS) {
            destroy_export_resource(runtime, resource);
            std::snprintf(reason, reason_size, "vkGetImageDrmFormatModifierPropertiesEXT for export image failed: %d", result);
            return false;
        }
        resource->drm_format_modifier = modifier_properties.drmFormatModifier;
        resource->has_drm_format_modifier = true;
    } else {
        resource->drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        resource->has_drm_format_modifier = true;
    }
    return true;
}

bool ensure_export_resource(VulkanRuntime *runtime, SurfaceResource *source, char *reason, size_t reason_size) {
    const ExportFormatInfo *format = export_format_for_surface(nullptr, source, reason, reason_size);
    if (format == nullptr) {
        return false;
    }

    ExportResource *resource = &source->export_resource;
    if (resource->image != VK_NULL_HANDLE &&
        resource->format == source->format &&
        resource->va_fourcc == source->va_fourcc &&
        resource->extent.width >= source->coded_extent.width &&
        resource->extent.height >= source->coded_extent.height) {
        return true;
    }

    destroy_export_resource(runtime, resource);
    if (runtime->image_drm_format_modifier &&
        create_export_resource_with_tiling(runtime, resource, format, source->coded_extent,
                                           VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                           reason, reason_size)) {
        return true;
    }
    destroy_export_resource(runtime, resource);
    return create_export_resource_with_tiling(runtime, resource, format, source->coded_extent,
                                             VK_IMAGE_TILING_LINEAR,
                                             reason, reason_size);
}

bool export_resource_matches_surface(const SurfaceResource *source) {
    if (source == nullptr) {
        return false;
    }

    const ExportResource &resource = source->export_resource;
    return resource.image != VK_NULL_HANDLE &&
           resource.format == source->format &&
           resource.va_fourcc == source->va_fourcc &&
           resource.extent.width >= source->coded_extent.width &&
           resource.extent.height >= source->coded_extent.height;
}

void add_raw_image_barrier(
        std::vector<VkImageMemoryBarrier2> *barriers,
        VkImage image,
        VkImageLayout old_layout,
        VkImageLayout new_layout,
        VkPipelineStageFlags2 src_stage,
        VkAccessFlags2 src_access,
        VkPipelineStageFlags2 dst_stage,
        VkAccessFlags2 dst_access) {
    if (old_layout == new_layout) {
        return;
    }
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : src_stage;
    barrier.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barriers->push_back(barrier);
}

bool copy_surface_to_export_resource(VulkanRuntime *runtime, SurfaceResource *source, char *reason, size_t reason_size) {
    const ExportFormatInfo *format = export_format_for_surface(nullptr, source, reason, reason_size);
    if (format == nullptr) {
        return false;
    }
    if (source->export_resource.exported && !export_resource_matches_surface(source)) {
        retire_export_resource(source);
    }
    if (!ensure_export_resource(runtime, source, reason, reason_size)) {
        return false;
    }

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        return false;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkResetFences for export copy failed: %d", result);
        return false;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkResetCommandBuffer for export copy failed: %d", result);
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkBeginCommandBuffer for export copy failed: %d", result);
        return false;
    }

    ExportResource *export_resource = &source->export_resource;
    std::vector<VkImageMemoryBarrier2> barriers;
    add_raw_image_barrier(&barriers,
                          source->image,
                          source->layout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
    add_raw_image_barrier(&barriers,
                          export_resource->image,
                          export_resource->layout,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }
    source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    export_resource->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy regions[2]{};
    for (uint32_t i = 0; i < format->layer_count; i++) {
        regions[i].srcSubresource.aspectMask = format->layers[i].aspect;
        regions[i].srcSubresource.layerCount = 1;
        regions[i].dstSubresource.aspectMask = format->layers[i].aspect;
        regions[i].dstSubresource.layerCount = 1;
        regions[i].extent = export_layer_extent(source->coded_extent, format->layers[i]);
    }

    vkCmdCopyImage(runtime->command_buffer,
                   source->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   export_resource->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   format->layer_count,
                   regions);

    barriers.clear();
    add_raw_image_barrier(&barriers,
                          source->image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                          VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
    add_raw_image_barrier(&barriers,
                          export_resource->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          VK_ACCESS_2_MEMORY_READ_BIT);
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }
    source->layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
    export_resource->layout = VK_IMAGE_LAYOUT_GENERAL;

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkEndCommandBuffer for export copy failed: %d", result);
        return false;
    }
    return submit_command_buffer_and_wait(runtime, reason, reason_size, "surface export copy");
}

VAStatus fill_drm_prime_descriptor(
        const VkvvSurface *surface,
        const ExportFormatInfo *format,
        VkDeviceSize allocation_size,
        const VkSubresourceLayout *plane_layouts,
        uint32_t plane_count,
        uint64_t modifier,
        bool has_modifier,
        int fd,
        VADRMPRIMESurfaceDescriptor *descriptor,
        char *reason,
        size_t reason_size) {
    if (plane_count != format->layer_count) {
        std::snprintf(reason, reason_size, "exportable %s image has %u planes, expected %u",
                      format->name, plane_count, format->layer_count);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (allocation_size > std::numeric_limits<uint32_t>::max()) {
        std::snprintf(reason, reason_size, "export allocation is too large for VADRMPRIMESurfaceDescriptor");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    for (uint32_t i = 0; i < format->layer_count; i++) {
        if (plane_layouts[i].offset > std::numeric_limits<uint32_t>::max() ||
            plane_layouts[i].rowPitch > std::numeric_limits<uint32_t>::max()) {
            std::snprintf(reason, reason_size, "export plane layout is too large for VADRMPRIMESurfaceDescriptor");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->fourcc = format->va_fourcc;
    descriptor->width = surface->width;
    descriptor->height = surface->height;
    descriptor->num_objects = 1;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = static_cast<uint32_t>(allocation_size);
    descriptor->objects[0].drm_format_modifier = has_modifier ? modifier : DRM_FORMAT_MOD_INVALID;
    descriptor->num_layers = format->layer_count;
    for (uint32_t i = 0; i < format->layer_count; i++) {
        descriptor->layers[i].drm_format = format->layers[i].drm_format;
        descriptor->layers[i].num_planes = 1;
        descriptor->layers[i].object_index[0] = 0;
        descriptor->layers[i].offset[0] = static_cast<uint32_t>(plane_layouts[i].offset);
        descriptor->layers[i].pitch[0] = static_cast<uint32_t>(plane_layouts[i].rowPitch);
    }

    return VA_STATUS_SUCCESS;
}

} // namespace vkvv

using namespace vkvv;

VAStatus vkvv_vulkan_prepare_surface_export(
        void *runtime_ptr,
        VkvvSurface *surface,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    if (runtime == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan runtime for surface export preparation");
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
    if (!ensure_surface_resource(runtime, surface, extent, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
    if (!resource->exportable && !ensure_export_resource(runtime, resource, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    std::snprintf(reason, reason_size,
                  "surface export resource ready: format=%s visible=%ux%u coded=%ux%u vk_format=%d va_fourcc=0x%x exportable=%u shadow=%u decode_mem=%llu export_mem=%llu retired=%zu",
                  format->name,
                  surface->width, surface->height,
                  resource->coded_extent.width, resource->coded_extent.height,
                  resource->format, resource->va_fourcc, resource->exportable,
                  resource->export_resource.image != VK_NULL_HANDLE,
                  static_cast<unsigned long long>(resource->allocation_size),
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  resource->retired_exports.size());
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
    if (!surface->decoded) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
    if (resource->exportable || resource->export_resource.image == VK_NULL_HANDLE) {
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
                  "refreshed exported %s shadow image after decode: export_mem=%llu retired=%zu",
                  format->name,
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  resource->retired_exports.size());
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
    if (runtime == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan runtime for surface export");
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
            if (!copy_surface_to_export_resource(runtime, resource, reason, reason_size)) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            copied_to_shadow = true;
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
        std::snprintf(reason, reason_size, "vkGetMemoryFdKHR failed: result=%d fd=%d", result, fd);
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
                  "exported %s dma-buf%s: %ux%u fd=%d size=%u modifier=0x%llx y_pitch=%u uv_pitch=%u decode_mem=%llu export_mem=%llu retired=%zu",
                  format->name, copied_to_shadow ? " via shadow copy" : "",
                  surface->width, surface->height, fd, descriptor->objects[0].size,
                  static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier),
                  descriptor->layers[0].pitch[0], descriptor->layers[1].pitch[0],
                  static_cast<unsigned long long>(resource->allocation_size),
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  resource->retired_exports.size());
    return VA_STATUS_SUCCESS;
}
