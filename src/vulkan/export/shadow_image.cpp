#include "vulkan/export/internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>
#include <drm_fourcc.h>

namespace vkvv {

void add_raw_image_barrier(
        std::vector<VkImageMemoryBarrier2> *barriers,
        VkImage image,
        VkImageLayout old_layout,
        VkImageLayout new_layout,
        VkPipelineStageFlags2 src_stage,
        VkAccessFlags2 src_access,
        VkPipelineStageFlags2 dst_stage,
        VkAccessFlags2 dst_access);

namespace {

uint32_t export_plane_bytes_per_pixel(uint32_t drm_format) {
    switch (drm_format) {
    case DRM_FORMAT_R8:
        return 1;
    case DRM_FORMAT_GR88:
    case DRM_FORMAT_R16:
        return 2;
    case DRM_FORMAT_GR1616:
        return 4;
    default:
        return 0;
    }
}

void store_u16_le(uint8_t *dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xffu);
    dst[1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

bool fill_export_black_plane(
        uint8_t *dst,
        VkExtent3D extent,
        uint32_t drm_format,
        char *reason,
        size_t reason_size) {
    const size_t samples = static_cast<size_t>(extent.width) * extent.height;
    switch (drm_format) {
    case DRM_FORMAT_R8:
        std::memset(dst, 16, samples);
        return true;
    case DRM_FORMAT_GR88:
        for (size_t i = 0; i < samples; i++) {
            dst[(i * 2) + 0] = 128;
            dst[(i * 2) + 1] = 128;
        }
        return true;
    case DRM_FORMAT_R16:
        for (size_t i = 0; i < samples; i++) {
            store_u16_le(dst + (i * 2), static_cast<uint16_t>(64u << 6u));
        }
        return true;
    case DRM_FORMAT_GR1616:
        for (size_t i = 0; i < samples; i++) {
            store_u16_le(dst + (i * 4) + 0, static_cast<uint16_t>(512u << 6u));
            store_u16_le(dst + (i * 4) + 2, static_cast<uint16_t>(512u << 6u));
        }
        return true;
    default:
        std::snprintf(reason, reason_size, "unsupported export shadow plane format 0x%x", drm_format);
        return false;
    }
}

bool create_staging_transfer_buffer(
        VulkanRuntime *runtime,
        VkDeviceSize size,
        UploadBuffer *staging,
        char *reason,
        size_t reason_size) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &staging->buffer);
    if (!record_vk_result(runtime, result, "vkCreateBuffer", "export shadow init", reason, reason_size)) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(runtime->device, staging->buffer, &requirements);
    staging->size = size;
    staging->capacity = size;
    staging->allocation_size = requirements.size;

    uint32_t memory_type_index = 0;
    staging->coherent = true;
    if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memory_type_index)) {
        staging->coherent = false;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index)) {
            destroy_upload_buffer(runtime, staging);
            std::snprintf(reason, reason_size, "no host-visible memory type for export shadow init");
            return false;
        }
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type_index;
    result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &staging->memory);
    if (!record_vk_result(runtime, result, "vkAllocateMemory", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, staging);
        return false;
    }

    result = vkBindBufferMemory(runtime->device, staging->buffer, staging->memory, 0);
    if (!record_vk_result(runtime, result, "vkBindBufferMemory", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, staging);
        return false;
    }
    return true;
}

bool initialize_export_resource_black(
        VulkanRuntime *runtime,
        ExportResource *resource,
        const ExportFormatInfo *format,
        char *reason,
        size_t reason_size) {
    if (resource == nullptr || resource->image == VK_NULL_HANDLE || format == nullptr) {
        std::snprintf(reason, reason_size, "missing export shadow init resource");
        return false;
    }

    VkBufferImageCopy regions[2]{};
    VkExtent3D plane_extents[2]{};
    uint32_t plane_formats[2]{};
    VkDeviceSize staging_size = 0;
    for (uint32_t i = 0; i < format->layer_count; i++) {
        const uint32_t bytes_per_pixel = export_plane_bytes_per_pixel(format->layers[i].drm_format);
        if (bytes_per_pixel == 0) {
            std::snprintf(reason, reason_size,
                          "unsupported export shadow plane format 0x%x",
                          format->layers[i].drm_format);
            return false;
        }
        plane_extents[i] = export_layer_extent(resource->extent, format->layers[i]);
        plane_formats[i] = format->layers[i].drm_format;
        staging_size = align_up(staging_size, 16);
        regions[i].bufferOffset = staging_size;
        regions[i].bufferRowLength = 0;
        regions[i].bufferImageHeight = 0;
        regions[i].imageSubresource.aspectMask = format->layers[i].aspect;
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageExtent = plane_extents[i];
        staging_size += static_cast<VkDeviceSize>(plane_extents[i].width) *
                        plane_extents[i].height *
                        bytes_per_pixel;
    }

    UploadBuffer staging{};
    if (!create_staging_transfer_buffer(runtime, staging_size, &staging, reason, reason_size)) {
        return false;
    }

    void *mapped = nullptr;
    VkResult result = vkMapMemory(runtime->device, staging.memory, 0, staging.size, 0, &mapped);
    if (!record_vk_result(runtime, result, "vkMapMemory", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(staging.size));
    for (uint32_t i = 0; i < format->layer_count; i++) {
        if (!fill_export_black_plane(static_cast<uint8_t *>(mapped) + regions[i].bufferOffset,
                                     plane_extents[i],
                                     plane_formats[i],
                                     reason,
                                     reason_size)) {
            vkUnmapMemory(runtime->device, staging.memory);
            destroy_upload_buffer(runtime, &staging);
            return false;
        }
    }
    if (!staging.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = staging.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(runtime->device, 1, &range);
    }
    vkUnmapMemory(runtime->device, staging.memory);

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }

    result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (!record_vk_result(runtime, result, "vkResetFences", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }

    std::vector<VkImageMemoryBarrier2> barriers;
    add_raw_image_barrier(&barriers,
                          resource->image,
                          resource->layout,
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
    resource->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    vkCmdCopyBufferToImage(runtime->command_buffer,
                           staging.buffer,
                           resource->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           format->layer_count,
                           regions);

    barriers.clear();
    add_raw_image_barrier(&barriers,
                          resource->image,
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
    resource->layout = VK_IMAGE_LAYOUT_GENERAL;

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "export shadow init", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }
    if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "export shadow init")) {
        destroy_upload_buffer(runtime, &staging);
        return false;
    }

    destroy_upload_buffer(runtime, &staging);
    resource->content_generation = 0;
    return true;
}

} // namespace

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
    if (!record_vk_result(runtime, result, "vkCreateImage", "export shadow image", reason, reason_size)) {
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
    if (!record_vk_result(runtime, result, "vkAllocateMemory", "export shadow image", reason, reason_size)) {
        destroy_export_resource(runtime, resource);
        return false;
    }

    result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
    if (!record_vk_result(runtime, result, "vkBindImageMemory", "export shadow image", reason, reason_size)) {
        destroy_export_resource(runtime, resource);
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
    resource->content_generation = 0;
    if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
        modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
        result = runtime->get_image_drm_format_modifier_properties(
            runtime->device, resource->image, &modifier_properties);
        if (!record_vk_result(runtime, result, "vkGetImageDrmFormatModifierPropertiesEXT", "export shadow image", reason, reason_size)) {
            destroy_export_resource(runtime, resource);
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
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export shadow")) {
        return false;
    }
    const ExportFormatInfo *format = export_format_for_surface(nullptr, source, reason, reason_size);
    if (format == nullptr) {
        return false;
    }

    ExportResource *resource = &source->export_resource;
    if (resource->image != VK_NULL_HANDLE &&
        resource->driver_instance_id == source->driver_instance_id &&
        resource->format == source->format &&
        resource->va_fourcc == source->va_fourcc &&
        resource->extent.width >= source->coded_extent.width &&
        resource->extent.height >= source->coded_extent.height) {
        resource->driver_instance_id = source->driver_instance_id;
        resource->owner_surface_id = source->surface_id;
        return true;
    }

    if (resource->image == VK_NULL_HANDLE && source->surface_id != VA_INVALID_ID) {
        bool reattached = false;
        prune_detached_exports_for_surface(runtime,
                                           source->driver_instance_id,
                                           source->surface_id,
                                           source->va_fourcc,
                                           source->format,
                                           source->coded_extent);
        {
            std::lock_guard<std::mutex> lock(runtime->export_mutex);
            for (auto it = runtime->detached_exports.begin(); it != runtime->detached_exports.end(); ++it) {
                if (it->driver_instance_id == source->driver_instance_id &&
                    it->owner_surface_id == source->surface_id &&
                    it->format == source->format &&
                    it->va_fourcc == source->va_fourcc &&
                    it->extent.width == source->coded_extent.width &&
                    it->extent.height == source->coded_extent.height) {
                    *resource = *it;
                    resource->driver_instance_id = source->driver_instance_id;
                    resource->owner_surface_id = source->surface_id;
                    resource->content_generation = 0;
                    runtime->detached_export_memory_bytes =
                        runtime->detached_export_memory_bytes > resource->allocation_size ?
                            runtime->detached_export_memory_bytes - resource->allocation_size : 0;
                    runtime->detached_exports.erase(it);
                    reattached = true;
                    break;
                }
            }
        }
        if (reattached) {
            if (!initialize_export_resource_black(runtime, resource, format, reason, reason_size)) {
                destroy_export_resource(runtime, resource);
                return false;
            }
            return true;
        }
    }

    detach_export_resource(runtime, source);
    if (runtime->image_drm_format_modifier &&
        create_export_resource_with_tiling(runtime, resource, format, source->coded_extent,
                                           VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                           reason, reason_size)) {
        resource->driver_instance_id = source->driver_instance_id;
        resource->owner_surface_id = source->surface_id;
        if (initialize_export_resource_black(runtime, resource, format, reason, reason_size)) {
            return true;
        }
    }
    destroy_export_resource(runtime, resource);
    if (!create_export_resource_with_tiling(runtime, resource, format, source->coded_extent,
                                           VK_IMAGE_TILING_LINEAR,
                                           reason, reason_size)) {
        return false;
    }
    resource->driver_instance_id = source->driver_instance_id;
    resource->owner_surface_id = source->surface_id;
    if (!initialize_export_resource_black(runtime, resource, format, reason, reason_size)) {
        destroy_export_resource(runtime, resource);
        return false;
    }
    return true;
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
        resource->driver_instance_id = surface->driver_instance_id;
        resource->visible_extent = {surface->width, surface->height};
        return true;
    }

    resource->extent = extent;
    resource->coded_extent = extent;
    resource->visible_extent = {surface->width, surface->height};
    resource->driver_instance_id = surface->driver_instance_id;
    resource->surface_id = surface->id;
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
           resource.driver_instance_id == source->driver_instance_id &&
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
        detach_export_resource(runtime, source);
    }
    if (!ensure_export_resource(runtime, source, reason, reason_size)) {
        return false;
    }
    if (source->content_generation != 0 &&
        source->export_resource.content_generation == source->content_generation) {
        return true;
    }

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        return false;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (!record_vk_result(runtime, result, "vkResetFences", "surface export copy", reason, reason_size)) {
        return false;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "surface export copy", reason, reason_size)) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "surface export copy", reason, reason_size)) {
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
    if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "surface export copy", reason, reason_size)) {
        return false;
    }
    if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "surface export copy")) {
        return false;
    }
    export_resource->content_generation = source->content_generation;
    return true;
}

} // namespace vkvv
