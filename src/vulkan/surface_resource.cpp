#include "../vulkan_runtime_internal.h"

#include <cstdio>
#include <new>
#include <vector>
#include <drm_fourcc.h>

namespace vkvv {

bool enumerate_drm_format_modifiers(
        VulkanRuntime *runtime,
        VkFormat format,
        VkFormatFeatureFlags2 required,
        std::vector<uint64_t> *modifiers) {
    VkDrmFormatModifierPropertiesList2EXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT;

    VkFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(runtime->physical_device, format, &properties);
    if (list.drmFormatModifierCount == 0) {
        return false;
    }

    std::vector<VkDrmFormatModifierProperties2EXT> modifier_properties(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifier_properties.data();
    vkGetPhysicalDeviceFormatProperties2(runtime->physical_device, format, &properties);

    modifiers->clear();
    modifiers->reserve(list.drmFormatModifierCount);
    for (const VkDrmFormatModifierProperties2EXT &property : modifier_properties) {
        if ((property.drmFormatModifierTilingFeatures & required) == required) {
            modifiers->push_back(property.drmFormatModifier);
        }
    }
    return !modifiers->empty();
}

bool enumerate_decode_drm_format_modifiers(VulkanRuntime *runtime, const DecodeImageKey &key, std::vector<uint64_t> *modifiers) {
    return enumerate_drm_format_modifiers(
        runtime,
        key.picture_format,
        VK_FORMAT_FEATURE_2_VIDEO_DECODE_OUTPUT_BIT_KHR |
            VK_FORMAT_FEATURE_2_VIDEO_DECODE_DPB_BIT_KHR,
        modifiers);
}

bool decode_image_key_matches(const DecodeImageKey &existing, const DecodeImageKey &requested) {
    return existing.codec_operation == requested.codec_operation &&
           existing.codec_profile == requested.codec_profile &&
           existing.picture_format == requested.picture_format &&
           existing.reference_picture_format == requested.reference_picture_format &&
           existing.va_rt_format == requested.va_rt_format &&
           existing.va_fourcc == requested.va_fourcc &&
           existing.coded_extent.width >= requested.coded_extent.width &&
           existing.coded_extent.height >= requested.coded_extent.height &&
           existing.usage == requested.usage &&
           existing.create_flags == requested.create_flags &&
           existing.tiling == requested.tiling &&
           existing.chroma_subsampling == requested.chroma_subsampling &&
           existing.luma_bit_depth == requested.luma_bit_depth &&
           existing.chroma_bit_depth == requested.chroma_bit_depth;
}

void destroy_export_resource(VulkanRuntime *runtime, ExportResource *resource) {
    if (resource == nullptr) {
        return;
    }
    if (resource->image != VK_NULL_HANDLE) {
        vkDestroyImage(runtime->device, resource->image, nullptr);
        resource->image = VK_NULL_HANDLE;
    }
    if (resource->memory != VK_NULL_HANDLE) {
        vkFreeMemory(runtime->device, resource->memory, nullptr);
        resource->memory = VK_NULL_HANDLE;
    }
    resource->extent = {};
    resource->format = VK_FORMAT_UNDEFINED;
    resource->va_fourcc = 0;
    resource->allocation_size = 0;
    resource->plane_layouts[0] = {};
    resource->plane_layouts[1] = {};
    resource->plane_count = 0;
    resource->drm_format_modifier = 0;
    resource->has_drm_format_modifier = false;
    resource->exported = false;
    resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

VkDeviceSize retired_export_memory_bytes(const SurfaceResource *resource) {
    VkDeviceSize bytes = 0;
    if (resource == nullptr) {
        return 0;
    }
    for (const ExportResource &retired : resource->retired_exports) {
        bytes += retired.allocation_size;
    }
    return bytes;
}

VkDeviceSize export_memory_bytes(const SurfaceResource *resource) {
    if (resource == nullptr) {
        return 0;
    }
    return resource->export_resource.allocation_size + retired_export_memory_bytes(resource);
}

void retire_export_resource(SurfaceResource *resource) {
    if (resource == nullptr || resource->export_resource.image == VK_NULL_HANDLE) {
        return;
    }
    resource->retired_exports.push_back(resource->export_resource);
    resource->export_resource = {};
}

void destroy_decode_resource_handles(VulkanRuntime *runtime, SurfaceResource *resource) {
    if (resource == nullptr) {
        return;
    }
    if (resource->view != VK_NULL_HANDLE) {
        vkDestroyImageView(runtime->device, resource->view, nullptr);
        resource->view = VK_NULL_HANDLE;
    }
    if (resource->image != VK_NULL_HANDLE) {
        vkDestroyImage(runtime->device, resource->image, nullptr);
        resource->image = VK_NULL_HANDLE;
    }
    if (resource->memory != VK_NULL_HANDLE) {
        vkFreeMemory(runtime->device, resource->memory, nullptr);
        resource->memory = VK_NULL_HANDLE;
    }
    resource->allocation_size = 0;
    resource->plane_layouts[0] = {};
    resource->plane_layouts[1] = {};
    resource->plane_count = 0;
    resource->drm_format_modifier = 0;
    resource->exportable = false;
    resource->has_drm_format_modifier = false;
    resource->decode_key = {};
    resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void destroy_surface_resource_raw(VulkanRuntime *runtime, SurfaceResource *resource) {
    if (resource == nullptr) {
        return;
    }

    destroy_export_resource(runtime, &resource->export_resource);
    for (ExportResource &retired : resource->retired_exports) {
        destroy_export_resource(runtime, &retired);
    }
    resource->retired_exports.clear();
    destroy_decode_resource_handles(runtime, resource);
    delete resource;
}

void destroy_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface) {
    if (surface == nullptr || surface->vulkan == nullptr) {
        return;
    }

    destroy_surface_resource_raw(runtime, static_cast<SurfaceResource *>(surface->vulkan));
    surface->vulkan = nullptr;
    surface->work_state = VKVV_SURFACE_WORK_READY;
    surface->sync_status = VA_STATUS_SUCCESS;
    surface->decoded = false;
}

bool ensure_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface, const DecodeImageKey &key, char *reason, size_t reason_size) {
    if (surface == nullptr) {
        std::snprintf(reason, reason_size, "missing target surface");
        return false;
    }
    if (key.codec_operation == 0 ||
        key.picture_format == VK_FORMAT_UNDEFINED ||
        key.reference_picture_format == VK_FORMAT_UNDEFINED ||
        key.coded_extent.width == 0 ||
        key.coded_extent.height == 0 ||
        key.usage == 0) {
        std::snprintf(reason, reason_size, "invalid decode image key");
        return false;
    }

    auto *existing = static_cast<SurfaceResource *>(surface->vulkan);
    if (existing != nullptr &&
        existing->image != VK_NULL_HANDLE &&
        decode_image_key_matches(existing->decode_key, key)) {
        existing->visible_extent = {surface->width, surface->height};
        return true;
    }
    if (existing != nullptr && existing->image != VK_NULL_HANDLE && surface->decoded) {
        std::snprintf(reason, reason_size,
                      "decoded reference surface decode image key mismatch: codec=0x%x format=%d fourcc=0x%x extent=%ux%u",
                      key.codec_operation, key.picture_format, key.va_fourcc,
                      key.coded_extent.width, key.coded_extent.height);
        return false;
    }

    if (existing != nullptr && existing->image != VK_NULL_HANDLE) {
        destroy_surface_resource(runtime, surface);
        existing = nullptr;
    }

    const VkExtent2D extent = key.coded_extent;
    const VideoProfileSpec profile_spec{
        .operation = static_cast<VkVideoCodecOperationFlagBitsKHR>(key.codec_operation),
        .bit_depth = key.luma_bit_depth,
    };
    VideoProfileChain profile_chain(profile_spec);
    VkVideoProfileListInfoKHR profile_list{};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = &profile_chain.profile;

    const bool export_layout_supported =
        key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
        key.tiling == VK_IMAGE_TILING_LINEAR;
    const bool export_descriptor_supported =
        (key.picture_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
         key.va_fourcc == VA_FOURCC_NV12) ||
        (key.picture_format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 &&
         key.va_fourcc == VA_FOURCC_P010);
    const bool request_exportable =
        runtime->surface_export &&
        export_descriptor_supported &&
        export_layout_supported;

    VkExternalMemoryImageCreateInfo external_image{};
    external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    std::vector<uint64_t> drm_modifiers;
    VkImageDrmFormatModifierListCreateInfoEXT drm_modifier_list{};
    drm_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        if (!enumerate_decode_drm_format_modifiers(runtime, key, &drm_modifiers)) {
            std::snprintf(reason, reason_size, "no DRM format modifiers support decode surfaces");
            return false;
        }
        drm_modifier_list.drmFormatModifierCount = static_cast<uint32_t>(drm_modifiers.size());
        drm_modifier_list.pDrmFormatModifiers = drm_modifiers.data();
    }

    if (request_exportable) {
        profile_list.pNext = &external_image;
        if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            external_image.pNext = &drm_modifier_list;
        }
    } else if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        profile_list.pNext = &drm_modifier_list;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &profile_list;
    image_info.flags = key.create_flags;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = key.picture_format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = key.tiling;
    image_info.usage = key.usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    auto *resource = existing != nullptr ? existing : new (std::nothrow) SurfaceResource();
    if (resource == nullptr) {
        std::snprintf(reason, reason_size, "out of memory creating surface resource");
        return false;
    }
    const bool new_resource = existing == nullptr;

    VkResult result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
    if (result != VK_SUCCESS) {
        if (new_resource) {
            delete resource;
        }
        std::snprintf(reason, reason_size, "vkCreateImage for decode surface failed: %d", result);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(runtime->device, resource->image, &requirements);

    uint32_t memory_type_index = 0;
    if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
        !find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, 0, &memory_type_index)) {
        if (new_resource) {
            destroy_surface_resource_raw(runtime, resource);
        } else {
            destroy_decode_resource_handles(runtime, resource);
        }
        std::snprintf(reason, reason_size, "no memory type for decode surface image");
        return false;
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type_index;

    VkMemoryDedicatedAllocateInfo dedicated_allocate{};
    dedicated_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_allocate.image = resource->image;

    VkExportMemoryAllocateInfo export_allocate{};
    export_allocate.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_allocate.pNext = &dedicated_allocate;
    export_allocate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (request_exportable) {
        allocate_info.pNext = &export_allocate;
    }

    result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &resource->memory);
    if (result != VK_SUCCESS) {
        if (new_resource) {
            destroy_surface_resource_raw(runtime, resource);
        } else {
            destroy_decode_resource_handles(runtime, resource);
        }
        std::snprintf(reason, reason_size, "vkAllocateMemory for decode surface failed: %d", result);
        return false;
    }

    result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
    if (result != VK_SUCCESS) {
        if (new_resource) {
            destroy_surface_resource_raw(runtime, resource);
        } else {
            destroy_decode_resource_handles(runtime, resource);
        }
        std::snprintf(reason, reason_size, "vkBindImageMemory for decode surface failed: %d", result);
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = resource->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = key.picture_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(runtime->device, &view_info, nullptr, &resource->view);
    if (result != VK_SUCCESS) {
        if (new_resource) {
            destroy_surface_resource_raw(runtime, resource);
        } else {
            destroy_decode_resource_handles(runtime, resource);
        }
        std::snprintf(reason, reason_size, "vkCreateImageView for decode surface failed: %d", result);
        return false;
    }

    resource->extent = extent;
    resource->coded_extent = extent;
    resource->visible_extent = {surface->width, surface->height};
    resource->format = key.picture_format;
    resource->va_rt_format = key.va_rt_format;
    resource->va_fourcc = key.va_fourcc;
    resource->decode_key = key;
    resource->allocation_size = requirements.size;
    if (request_exportable) {
        VkImageSubresource plane0{};
        plane0.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        VkImageSubresource plane1{};
        plane1.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        vkGetImageSubresourceLayout(runtime->device, resource->image, &plane0, &resource->plane_layouts[0]);
        vkGetImageSubresourceLayout(runtime->device, resource->image, &plane1, &resource->plane_layouts[1]);
        resource->plane_count = 2;
        resource->exportable = true;

        if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
            modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
            result = runtime->get_image_drm_format_modifier_properties(
                runtime->device, resource->image, &modifier_properties);
            if (result != VK_SUCCESS) {
                if (new_resource) {
                    destroy_surface_resource_raw(runtime, resource);
                } else {
                    destroy_decode_resource_handles(runtime, resource);
                }
                std::snprintf(reason, reason_size, "vkGetImageDrmFormatModifierPropertiesEXT failed: %d", result);
                return false;
            }
            resource->drm_format_modifier = modifier_properties.drmFormatModifier;
            resource->has_drm_format_modifier = true;
        } else {
            resource->drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
            resource->has_drm_format_modifier = true;
        }
    }
    if (new_resource) {
        surface->vulkan = resource;
    }
    return true;
}

void add_image_layout_barrier(std::vector<VkImageMemoryBarrier2> *barriers, SurfaceResource *resource, VkImageLayout new_layout, VkAccessFlags2 dst_access) {
    if (resource->layout == new_layout) {
        return;
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = resource->layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resource->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barriers->push_back(barrier);
    resource->layout = new_layout;
}

VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource *resource, VkExtent2D coded_extent) {
    VkVideoPictureResourceInfoKHR picture{};
    picture.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    picture.codedOffset = {0, 0};
    picture.codedExtent = coded_extent;
    picture.baseArrayLayer = 0;
    picture.imageViewBinding = resource->view;
    return picture;
}

} // namespace vkvv

using namespace vkvv;

void vkvv_vulkan_surface_destroy(void *runtime_ptr, VkvvSurface *surface) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    if (runtime == nullptr) {
        return;
    }
    destroy_surface_resource(runtime, surface);
}
