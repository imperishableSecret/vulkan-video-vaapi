#include "vulkan/export/internal.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <new>
#include <vector>
#include <drm_fourcc.h>

namespace vkvv {

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

bool ensure_export_only_surface_resource(
        VkvvSurface *surface,
        const ExportFormatInfo *format,
        VkExtent2D extent,
        char *reason,
        size_t reason_size) {
    if (surface == nullptr || format == nullptr) {
        std::snprintf(reason, reason_size, "missing surface export resource state");
        return false;
    }

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
    if (resource == nullptr) {
        resource = new (std::nothrow) SurfaceResource();
        if (resource == nullptr) {
            std::snprintf(reason, reason_size, "out of memory creating export-only surface resource");
            return false;
        }
        surface->vulkan = resource;
    }

    if (resource->image != VK_NULL_HANDLE) {
        resource->visible_extent = {surface->width, surface->height};
        return true;
    }

    resource->extent = extent;
    resource->coded_extent = extent;
    resource->visible_extent = {surface->width, surface->height};
    resource->format = format->vk_format;
    resource->va_rt_format = surface->rt_format;
    resource->va_fourcc = surface->fourcc;
    resource->decode_key = {};
    resource->allocation_size = 0;
    resource->plane_layouts[0] = {};
    resource->plane_layouts[1] = {};
    resource->plane_count = 0;
    resource->drm_format_modifier = 0;
    resource->exportable = false;
    resource->has_drm_format_modifier = false;
    resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
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

} // namespace vkvv
