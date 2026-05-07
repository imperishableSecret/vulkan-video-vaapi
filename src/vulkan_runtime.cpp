#include "vulkan_runtime_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
#include <unistd.h>
#include <drm_fourcc.h>

namespace vkvv {

bool extension_present(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    for (const VkExtensionProperties &extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool enumerate_device_extensions(VkPhysicalDevice device, std::vector<VkExtensionProperties> &extensions, char *reason, size_t reason_size) {
    uint32_t count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        std::snprintf(reason, reason_size, "vkEnumerateDeviceExtensionProperties failed: result=%d count=%u", result, count);
        return false;
    }

    extensions.resize(count);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkEnumerateDeviceExtensionProperties failed: %d", result);
        return false;
    }
    extensions.resize(count);
    return true;
}

uint32_t find_decode_queue_family(VulkanRuntime *runtime, VkPhysicalDevice device) {
    uint32_t count = 0;
    runtime->get_queue_family_properties2(device, &count, nullptr);
    if (count == 0) {
        return invalid_queue_family;
    }

    std::vector<VkQueueFamilyProperties2> queue_props(count);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video_props(count);
    for (uint32_t i = 0; i < count; i++) {
        video_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        queue_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        queue_props[i].pNext = &video_props[i];
    }
    runtime->get_queue_family_properties2(device, &count, queue_props.data());

    for (uint32_t i = 0; i < count; i++) {
        const bool has_decode_queue = (queue_props[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0;
        const bool has_h264 = (video_props[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0;
        if (has_decode_queue && has_h264) {
            return i;
        }
    }
    return invalid_queue_family;
}

bool pick_physical_device(VulkanRuntime *runtime, char *reason, size_t reason_size) {
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(runtime->instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        std::snprintf(reason, reason_size, "no Vulkan physical devices: result=%d count=%u", result, count);
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(runtime->instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkEnumeratePhysicalDevices failed: %d", result);
        return false;
    }
    devices.resize(count);

    for (VkPhysicalDevice device : devices) {
        std::vector<VkExtensionProperties> extensions;
        if (!enumerate_device_extensions(device, extensions, reason, reason_size)) {
            continue;
        }
        if (!extension_present(extensions, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) ||
            !extension_present(extensions, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) ||
            !extension_present(extensions, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME)) {
            continue;
        }

        const uint32_t queue_family = find_decode_queue_family(runtime, device);
        if (queue_family == invalid_queue_family) {
            continue;
        }

        const bool has_video_maintenance2 = extension_present(extensions, VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME);
        const bool has_external_memory_fd = extension_present(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        const bool has_external_memory_dma_buf = extension_present(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        const bool has_image_drm_format_modifier = extension_present(extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        VkPhysicalDeviceVideoMaintenance2FeaturesKHR maintenance2_features{};
        maintenance2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &maintenance2_features;
        vkGetPhysicalDeviceFeatures2(device, &features2);

        runtime->physical_device = device;
        runtime->decode_queue_family = queue_family;
        runtime->video_maintenance2 = has_video_maintenance2 && maintenance2_features.videoMaintenance2;
        runtime->external_memory_fd = has_external_memory_fd;
        runtime->external_memory_dma_buf = has_external_memory_dma_buf;
        runtime->image_drm_format_modifier = has_image_drm_format_modifier;
        runtime->surface_export = has_external_memory_fd &&
                                  has_external_memory_dma_buf &&
                                  has_image_drm_format_modifier;
        vkGetPhysicalDeviceMemoryProperties(device, &runtime->memory_properties);
        return true;
    }

    std::snprintf(reason, reason_size, "no physical device exposes H.264 Vulkan Video decode queue");
    return false;
}

bool create_device(VulkanRuntime *runtime, char *reason, size_t reason_size) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = runtime->decode_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    std::vector<const char *> extensions = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    };
    if (runtime->video_maintenance2) {
        extensions.push_back(VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME);
    }
    if (runtime->external_memory_fd) {
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    if (runtime->external_memory_dma_buf) {
        extensions.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }
    if (runtime->image_drm_format_modifier) {
        extensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    }

    VkPhysicalDeviceVideoMaintenance2FeaturesKHR maintenance2{};
    maintenance2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
    maintenance2.videoMaintenance2 = runtime->video_maintenance2 ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceSynchronization2Features sync2{};
    sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2.pNext = runtime->video_maintenance2 ? &maintenance2 : nullptr;
    sync2.synchronization2 = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &sync2;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    device_info.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateDevice(runtime->physical_device, &device_info, nullptr, &runtime->device);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateDevice failed: %d", result);
        return false;
    }
    vkGetDeviceQueue(runtime->device, runtime->decode_queue_family, 0, &runtime->decode_queue);

    runtime->create_video_session = reinterpret_cast<PFN_vkCreateVideoSessionKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkCreateVideoSessionKHR"));
    runtime->destroy_video_session = reinterpret_cast<PFN_vkDestroyVideoSessionKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkDestroyVideoSessionKHR"));
    runtime->get_video_session_memory_requirements = reinterpret_cast<PFN_vkGetVideoSessionMemoryRequirementsKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkGetVideoSessionMemoryRequirementsKHR"));
    runtime->bind_video_session_memory = reinterpret_cast<PFN_vkBindVideoSessionMemoryKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkBindVideoSessionMemoryKHR"));
    runtime->create_video_session_parameters = reinterpret_cast<PFN_vkCreateVideoSessionParametersKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkCreateVideoSessionParametersKHR"));
    runtime->destroy_video_session_parameters = reinterpret_cast<PFN_vkDestroyVideoSessionParametersKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkDestroyVideoSessionParametersKHR"));
    runtime->cmd_begin_video_coding = reinterpret_cast<PFN_vkCmdBeginVideoCodingKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkCmdBeginVideoCodingKHR"));
    runtime->cmd_end_video_coding = reinterpret_cast<PFN_vkCmdEndVideoCodingKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkCmdEndVideoCodingKHR"));
    runtime->cmd_control_video_coding = reinterpret_cast<PFN_vkCmdControlVideoCodingKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkCmdControlVideoCodingKHR"));
    runtime->cmd_decode_video = reinterpret_cast<PFN_vkCmdDecodeVideoKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkCmdDecodeVideoKHR"));
    runtime->get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(runtime->device, "vkGetMemoryFdKHR"));
    runtime->get_image_drm_format_modifier_properties = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
        vkGetDeviceProcAddr(runtime->device, "vkGetImageDrmFormatModifierPropertiesEXT"));

    if (runtime->create_video_session == nullptr ||
        runtime->destroy_video_session == nullptr ||
        runtime->get_video_session_memory_requirements == nullptr ||
        runtime->bind_video_session_memory == nullptr ||
        runtime->create_video_session_parameters == nullptr ||
        runtime->destroy_video_session_parameters == nullptr ||
        runtime->cmd_begin_video_coding == nullptr ||
        runtime->cmd_end_video_coding == nullptr ||
        runtime->cmd_control_video_coding == nullptr ||
        runtime->cmd_decode_video == nullptr) {
        std::snprintf(reason, reason_size, "required Vulkan Video device entrypoints unavailable");
        return false;
    }
    if (runtime->surface_export &&
        (runtime->get_memory_fd == nullptr ||
         runtime->get_image_drm_format_modifier_properties == nullptr)) {
        runtime->surface_export = false;
    }

    return true;
}

uint32_t round_up_16(uint32_t value) {
    return (value + 15u) & ~15u;
}

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

bool find_memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t *type_index) {
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            *type_index = i;
            return true;
        }
    }
    return false;
}

VkImageUsageFlags h264_surface_image_usage() {
    return VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
           VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}

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

bool enumerate_h264_drm_format_modifiers(VulkanRuntime *runtime, std::vector<uint64_t> *modifiers) {
    return enumerate_drm_format_modifiers(
        runtime,
        runtime->h264_picture_format,
        VK_FORMAT_FEATURE_2_VIDEO_DECODE_OUTPUT_BIT_KHR |
            VK_FORMAT_FEATURE_2_VIDEO_DECODE_DPB_BIT_KHR,
        modifiers);
}

void destroy_video_session(VulkanRuntime *runtime, VideoSession *session) {
    if (session == nullptr) {
        return;
    }
    if (runtime != nullptr && session->session != VK_NULL_HANDLE && runtime->destroy_video_session != nullptr) {
        runtime->destroy_video_session(runtime->device, session->session, nullptr);
        session->session = VK_NULL_HANDLE;
    }
    if (runtime != nullptr) {
        for (VkDeviceMemory memory : session->memory) {
            vkFreeMemory(runtime->device, memory, nullptr);
        }
    }
    session->memory.clear();
    session->memory_bytes = 0;
    session->key = {};
    session->initialized = false;
}

void destroy_h264_video_session(VulkanRuntime *runtime, H264VideoSession *session) {
    if (session == nullptr) {
        return;
    }
    destroy_video_session(runtime, &session->video);
    session->bitstream_offset_alignment = 1;
    session->bitstream_size_alignment = 1;
    session->max_level = STD_VIDEO_H264_LEVEL_IDC_5_2;
    session->decode_flags = 0;
    session->max_dpb_slots = 0;
    session->max_active_reference_pictures = 0;
}

bool bind_video_session_memory(VulkanRuntime *runtime, VideoSession *session, char *reason, size_t reason_size) {
    uint32_t count = 0;
    VkResult result = runtime->get_video_session_memory_requirements(runtime->device, session->session, &count, nullptr);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetVideoSessionMemoryRequirementsKHR failed: %d", result);
        return false;
    }
    if (count == 0) {
        return true;
    }

    std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(count);
    for (VkVideoSessionMemoryRequirementsKHR &requirement : requirements) {
        requirement.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    result = runtime->get_video_session_memory_requirements(runtime->device, session->session, &count, requirements.data());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetVideoSessionMemoryRequirementsKHR failed: %d", result);
        return false;
    }
    requirements.resize(count);

    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    binds.reserve(requirements.size());
    session->memory.reserve(requirements.size());

    for (const VkVideoSessionMemoryRequirementsKHR &requirement : requirements) {
        uint32_t memory_type_index = 0;
        if (!find_memory_type(runtime->memory_properties, requirement.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
            !find_memory_type(runtime->memory_properties, requirement.memoryRequirements.memoryTypeBits, 0, &memory_type_index)) {
            std::snprintf(reason, reason_size, "no memory type for H.264 session bind index %u", requirement.memoryBindIndex);
            return false;
        }

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirement.memoryRequirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &memory);
        if (result != VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkAllocateMemory for H.264 session failed: %d", result);
            return false;
        }
        session->memory.push_back(memory);

        VkBindVideoSessionMemoryInfoKHR bind{};
        bind.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        bind.memoryBindIndex = requirement.memoryBindIndex;
        bind.memory = memory;
        bind.memoryOffset = 0;
        bind.memorySize = requirement.memoryRequirements.size;
        binds.push_back(bind);
        session->memory_bytes += requirement.memoryRequirements.size;
    }

    result = runtime->bind_video_session_memory(runtime->device, session->session,
                                               static_cast<uint32_t>(binds.size()), binds.data());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkBindVideoSessionMemoryKHR failed: %d", result);
        return false;
    }

    return true;
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

void destroy_surface_resource_raw(VulkanRuntime *runtime, SurfaceResource *resource) {
    if (resource == nullptr) {
        return;
    }

    destroy_export_resource(runtime, &resource->export_resource);
    for (ExportResource &retired : resource->retired_exports) {
        destroy_export_resource(runtime, &retired);
    }
    resource->retired_exports.clear();
    if (resource->view != VK_NULL_HANDLE) {
        vkDestroyImageView(runtime->device, resource->view, nullptr);
    }
    if (resource->image != VK_NULL_HANDLE) {
        vkDestroyImage(runtime->device, resource->image, nullptr);
    }
    if (resource->memory != VK_NULL_HANDLE) {
        vkFreeMemory(runtime->device, resource->memory, nullptr);
    }
    delete resource;
}

void destroy_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface) {
    if (surface == nullptr || surface->vulkan == nullptr) {
        return;
    }

    destroy_surface_resource_raw(runtime, static_cast<SurfaceResource *>(surface->vulkan));
    surface->vulkan = nullptr;
    surface->decoded = false;
    surface->dpb_slot = -1;
}

bool ensure_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface, VkExtent2D extent, char *reason, size_t reason_size) {
    if (surface == nullptr) {
        std::snprintf(reason, reason_size, "missing target surface");
        return false;
    }

    auto *existing = static_cast<SurfaceResource *>(surface->vulkan);
    if (existing != nullptr &&
        existing->coded_extent.width >= extent.width &&
        existing->coded_extent.height >= extent.height &&
        existing->format == runtime->h264_picture_format &&
        existing->va_rt_format == surface->rt_format &&
        existing->va_fourcc == surface->fourcc) {
        existing->visible_extent = {surface->width, surface->height};
        return true;
    }
    if (existing != nullptr && surface->decoded) {
        std::snprintf(reason, reason_size, "decoded reference surface is too small for H.264 session");
        return false;
    }

    destroy_surface_resource(runtime, surface);

    VideoProfileChain profile_chain;
    VkVideoProfileListInfoKHR profile_list{};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = &profile_chain.profile;

    const bool export_layout_supported =
        runtime->h264_image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
        runtime->h264_image_tiling == VK_IMAGE_TILING_LINEAR;
    const bool request_exportable =
        runtime->surface_export &&
        runtime->h264_picture_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
        export_layout_supported;

    VkExternalMemoryImageCreateInfo external_image{};
    external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    std::vector<uint64_t> drm_modifiers;
    VkImageDrmFormatModifierListCreateInfoEXT drm_modifier_list{};
    drm_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    if (runtime->h264_image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        if (!enumerate_h264_drm_format_modifiers(runtime, &drm_modifiers)) {
            std::snprintf(reason, reason_size, "no DRM format modifiers support H.264 decode surfaces");
            return false;
        }
        drm_modifier_list.drmFormatModifierCount = static_cast<uint32_t>(drm_modifiers.size());
        drm_modifier_list.pDrmFormatModifiers = drm_modifiers.data();
    }

    if (request_exportable) {
        profile_list.pNext = &external_image;
        if (runtime->h264_image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            external_image.pNext = &drm_modifier_list;
        }
    } else if (runtime->h264_image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        profile_list.pNext = &drm_modifier_list;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &profile_list;
    image_info.flags = runtime->h264_image_create_flags;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = runtime->h264_picture_format;
    image_info.extent = {extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = runtime->h264_image_tiling;
    image_info.usage = h264_surface_image_usage();
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    auto *resource = new (std::nothrow) SurfaceResource();
    if (resource == nullptr) {
        std::snprintf(reason, reason_size, "out of memory creating surface resource");
        return false;
    }

    VkResult result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
    if (result != VK_SUCCESS) {
        delete resource;
        std::snprintf(reason, reason_size, "vkCreateImage for H.264 surface failed: %d", result);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(runtime->device, resource->image, &requirements);

    uint32_t memory_type_index = 0;
    if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
        !find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, 0, &memory_type_index)) {
        destroy_surface_resource_raw(runtime, resource);
        std::snprintf(reason, reason_size, "no memory type for H.264 surface image");
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
        destroy_surface_resource_raw(runtime, resource);
        std::snprintf(reason, reason_size, "vkAllocateMemory for H.264 surface failed: %d", result);
        return false;
    }

    result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_surface_resource_raw(runtime, resource);
        std::snprintf(reason, reason_size, "vkBindImageMemory for H.264 surface failed: %d", result);
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = resource->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = runtime->h264_picture_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(runtime->device, &view_info, nullptr, &resource->view);
    if (result != VK_SUCCESS) {
        destroy_surface_resource_raw(runtime, resource);
        std::snprintf(reason, reason_size, "vkCreateImageView for H.264 surface failed: %d", result);
        return false;
    }

    resource->extent = extent;
    resource->coded_extent = extent;
    resource->visible_extent = {surface->width, surface->height};
    resource->format = runtime->h264_picture_format;
    resource->va_rt_format = surface->rt_format;
    resource->va_fourcc = surface->fourcc;
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

        if (runtime->h264_image_tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
            modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
            result = runtime->get_image_drm_format_modifier_properties(
                runtime->device, resource->image, &modifier_properties);
            if (result != VK_SUCCESS) {
                destroy_surface_resource_raw(runtime, resource);
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
    surface->vulkan = resource;
    return true;
}

void destroy_upload_buffer(VulkanRuntime *runtime, UploadBuffer *upload) {
    if (upload->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(runtime->device, upload->buffer, nullptr);
        upload->buffer = VK_NULL_HANDLE;
    }
    if (upload->memory != VK_NULL_HANDLE) {
        vkFreeMemory(runtime->device, upload->memory, nullptr);
        upload->memory = VK_NULL_HANDLE;
    }
    upload->size = 0;
    upload->allocation_size = 0;
}

bool create_upload_buffer(
        VulkanRuntime *runtime,
        const H264VideoSession *session,
        const VkvvH264DecodeInput *input,
        UploadBuffer *upload,
        char *reason,
        size_t reason_size) {
    VideoProfileChain profile_chain;
    VkVideoProfileListInfoKHR profile_list{};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = &profile_chain.profile;

    const VkDeviceSize requested_size = std::max<VkDeviceSize>(1, input->bitstream_size);
    upload->size = align_up(requested_size, session->bitstream_size_alignment);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = &profile_list;
    buffer_info.size = upload->size;
    buffer_info.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &upload->buffer);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateBuffer for H.264 bitstream failed: %d", result);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(runtime->device, upload->buffer, &requirements);
    upload->allocation_size = requirements.size;

    uint32_t memory_type_index = 0;
    bool coherent = true;
    if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &memory_type_index)) {
        coherent = false;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index)) {
            destroy_upload_buffer(runtime, upload);
            std::snprintf(reason, reason_size, "no host-visible memory type for H.264 bitstream");
            return false;
        }
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type_index;
    result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &upload->memory);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, upload);
        std::snprintf(reason, reason_size, "vkAllocateMemory for H.264 bitstream failed: %d", result);
        return false;
    }

    result = vkBindBufferMemory(runtime->device, upload->buffer, upload->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, upload);
        std::snprintf(reason, reason_size, "vkBindBufferMemory for H.264 bitstream failed: %d", result);
        return false;
    }

    void *mapped = nullptr;
    result = vkMapMemory(runtime->device, upload->memory, 0, upload->size, 0, &mapped);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, upload);
        std::snprintf(reason, reason_size, "vkMapMemory for H.264 bitstream failed: %d", result);
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(upload->size));
    std::memcpy(mapped, input->bitstream, input->bitstream_size);
    if (!coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = upload->memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(runtime->device, 1, &range);
    }
    vkUnmapMemory(runtime->device, upload->memory);
    return true;
}

bool ensure_command_resources(VulkanRuntime *runtime, char *reason, size_t reason_size) {
    if (runtime->command_buffer != VK_NULL_HANDLE && runtime->fence != VK_NULL_HANDLE) {
        return true;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = runtime->decode_queue_family;

    VkResult result = vkCreateCommandPool(runtime->device, &pool_info, nullptr, &runtime->command_pool);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateCommandPool for H.264 decode failed: %d", result);
        return false;
    }

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = runtime->command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(runtime->device, &allocate_info, &runtime->command_buffer);
    if (result != VK_SUCCESS) {
        runtime->destroy_command_resources();
        std::snprintf(reason, reason_size, "vkAllocateCommandBuffers for H.264 decode failed: %d", result);
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(runtime->device, &fence_info, nullptr, &runtime->fence);
    if (result != VK_SUCCESS) {
        runtime->destroy_command_resources();
        std::snprintf(reason, reason_size, "vkCreateFence for H.264 decode failed: %d", result);
        return false;
    }

    return true;
}

bool submit_command_buffer_and_wait(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &runtime->command_buffer;

    VkResult result = vkQueueSubmit(runtime->decode_queue, 1, &submit, runtime->fence);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkQueueSubmit for %s failed: %d", operation, result);
        return false;
    }

    result = vkWaitForFences(runtime->device, 1, &runtime->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkWaitForFences for %s failed: %d", operation, result);
        return false;
    }

    return true;
}

bool reset_h264_session(
        VulkanRuntime *runtime,
        H264VideoSession *session,
        VkVideoSessionParametersKHR parameters,
        char *reason,
        size_t reason_size) {
    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        return false;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkResetFences for H.264 session reset failed: %d", result);
        return false;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkResetCommandBuffer for H.264 session reset failed: %d", result);
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkBeginCommandBuffer for H.264 session reset failed: %d", result);
        return false;
    }

    VkVideoBeginCodingInfoKHR video_begin{};
    video_begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession = session->video.session;
    video_begin.videoSessionParameters = parameters;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    VkVideoCodingControlInfoKHR control{};
    control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    runtime->cmd_control_video_coding(runtime->command_buffer, &control);

    VkVideoEndCodingInfoKHR video_end{};
    video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkEndCommandBuffer for H.264 session reset failed: %d", result);
        return false;
    }

    if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "H.264 session reset")) {
        return false;
    }

    session->video.initialized = true;
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

bool create_export_resource_with_tiling(
        VulkanRuntime *runtime,
        ExportResource *resource,
        VkExtent2D extent,
        VkImageTiling tiling,
        char *reason,
        size_t reason_size) {
    if (runtime->h264_picture_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        std::snprintf(reason, reason_size, "export shadow image currently supports only NV12-compatible Vulkan format");
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
                runtime->h264_picture_format,
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
    image_info.format = runtime->h264_picture_format;
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

    VkImageSubresource plane0{};
    plane0.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkImageSubresource plane1{};
    plane1.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    vkGetImageSubresourceLayout(runtime->device, resource->image, &plane0, &resource->plane_layouts[0]);
    vkGetImageSubresourceLayout(runtime->device, resource->image, &plane1, &resource->plane_layouts[1]);

    resource->extent = extent;
    resource->allocation_size = requirements.size;
    resource->plane_count = 2;
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
    ExportResource *resource = &source->export_resource;
    if (resource->image != VK_NULL_HANDLE &&
        resource->extent.width >= source->coded_extent.width &&
        resource->extent.height >= source->coded_extent.height) {
        return true;
    }

    destroy_export_resource(runtime, resource);
    if (runtime->image_drm_format_modifier &&
        create_export_resource_with_tiling(runtime, resource, source->coded_extent,
                                           VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                           reason, reason_size)) {
        return true;
    }
    destroy_export_resource(runtime, resource);
    return create_export_resource_with_tiling(runtime, resource, source->coded_extent,
                                             VK_IMAGE_TILING_LINEAR,
                                             reason, reason_size);
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
    if (source->export_resource.exported) {
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
    regions[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regions[0].srcSubresource.layerCount = 1;
    regions[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regions[0].dstSubresource.layerCount = 1;
    regions[0].extent = {source->coded_extent.width, source->coded_extent.height, 1};

    regions[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regions[1].srcSubresource.layerCount = 1;
    regions[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regions[1].dstSubresource.layerCount = 1;
    regions[1].extent = {(source->coded_extent.width + 1) / 2, (source->coded_extent.height + 1) / 2, 1};

    vkCmdCopyImage(runtime->command_buffer,
                   source->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   export_resource->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   2,
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

using namespace vkvv;

void *vkvv_vulkan_runtime_create(char *reason, size_t reason_size) {
    try {
        auto runtime = new VulkanRuntime();

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "nvidia-vulkan-vaapi";
        app_info.applicationVersion = 1;
        app_info.pEngineName = "nvidia-vulkan-vaapi";
        app_info.engineVersion = 1;
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app_info;

        VkResult result = vkCreateInstance(&instance_info, nullptr, &runtime->instance);
        if (result != VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkCreateInstance failed: %d", result);
            delete runtime;
            return nullptr;
        }

        runtime->get_queue_family_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
            vkGetInstanceProcAddr(runtime->instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
        runtime->get_video_capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
            vkGetInstanceProcAddr(runtime->instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
        runtime->get_video_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(
            vkGetInstanceProcAddr(runtime->instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
        if (runtime->get_queue_family_properties2 == nullptr ||
            runtime->get_video_capabilities == nullptr ||
            runtime->get_video_format_properties == nullptr) {
            std::snprintf(reason, reason_size, "required Vulkan Video instance entrypoints unavailable");
            delete runtime;
            return nullptr;
        }

        if (!pick_physical_device(runtime, reason, reason_size)) {
            delete runtime;
            return nullptr;
        }
        if (!create_device(runtime, reason, reason_size)) {
            delete runtime;
            return nullptr;
        }

        std::snprintf(reason, reason_size, "Vulkan Video runtime ready: queue_family=%u", runtime->decode_queue_family);
        return runtime;
    } catch (const std::bad_alloc &) {
        std::snprintf(reason, reason_size, "out of memory creating Vulkan runtime");
        return nullptr;
    }
}

void vkvv_vulkan_runtime_destroy(void *runtime) {
    delete static_cast<VulkanRuntime *>(runtime);
}

bool vkvv_vulkan_supports_surface_export(void *runtime_ptr) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    return runtime != nullptr && runtime->surface_export;
}

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
    if (surface->fourcc != VA_FOURCC_NV12) {
        std::snprintf(reason, reason_size, "surface export currently supports only NV12");
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
                  "surface export resource ready: visible=%ux%u coded=%ux%u format=%d va_fourcc=0x%x exportable=%u shadow=%u decode_mem=%llu export_mem=%llu retired=%zu",
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

    if (!copy_surface_to_export_resource(runtime, resource, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    std::snprintf(reason, reason_size,
                  "refreshed exported NV12 shadow image after decode: export_mem=%llu retired=%zu",
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
    if (surface->fourcc != VA_FOURCC_NV12) {
        std::snprintf(reason, reason_size, "surface export currently supports only NV12");
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) == 0) {
        std::snprintf(reason, reason_size, "surface export currently requires separate layers");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (descriptor == nullptr) {
        std::snprintf(reason, reason_size, "surface export descriptor is null");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    auto *resource = static_cast<SurfaceResource *>(surface->vulkan);
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

    if (export_memory == VK_NULL_HANDLE || export_plane_count != 2) {
        std::snprintf(reason, reason_size, "Vulkan surface image has no exportable memory layout");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (export_allocation_size > std::numeric_limits<uint32_t>::max()) {
        std::snprintf(reason, reason_size, "export allocation is too large for VADRMPRIMESurfaceDescriptor");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    for (uint32_t i = 0; i < export_plane_count; i++) {
        if (export_plane_layouts[i].offset > std::numeric_limits<uint32_t>::max() ||
            export_plane_layouts[i].rowPitch > std::numeric_limits<uint32_t>::max()) {
            std::snprintf(reason, reason_size, "export plane layout is too large for VADRMPRIMESurfaceDescriptor");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
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

    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->fourcc = VA_FOURCC_NV12;
    descriptor->width = surface->width;
    descriptor->height = surface->height;
    descriptor->num_objects = 1;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = static_cast<uint32_t>(export_allocation_size);
    descriptor->objects[0].drm_format_modifier =
        export_has_modifier ? export_modifier : DRM_FORMAT_MOD_INVALID;

    descriptor->num_layers = 2;
    descriptor->layers[0].drm_format = DRM_FORMAT_R8;
    descriptor->layers[0].num_planes = 1;
    descriptor->layers[0].object_index[0] = 0;
    descriptor->layers[0].offset[0] = static_cast<uint32_t>(export_plane_layouts[0].offset);
    descriptor->layers[0].pitch[0] = static_cast<uint32_t>(export_plane_layouts[0].rowPitch);

    descriptor->layers[1].drm_format = DRM_FORMAT_GR88;
    descriptor->layers[1].num_planes = 1;
    descriptor->layers[1].object_index[0] = 0;
    descriptor->layers[1].offset[0] = static_cast<uint32_t>(export_plane_layouts[1].offset);
    descriptor->layers[1].pitch[0] = static_cast<uint32_t>(export_plane_layouts[1].rowPitch);
    if (exported_shadow != nullptr) {
        exported_shadow->exported = true;
    }

    std::snprintf(reason, reason_size,
                  "exported NV12 dma-buf%s: %ux%u fd=%d size=%u modifier=0x%llx y_pitch=%u uv_pitch=%u decode_mem=%llu export_mem=%llu retired=%zu",
                  copied_to_shadow ? " via shadow copy" : "",
                  surface->width, surface->height, fd, descriptor->objects[0].size,
                  static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier),
                  descriptor->layers[0].pitch[0], descriptor->layers[1].pitch[0],
                  static_cast<unsigned long long>(resource->allocation_size),
                  static_cast<unsigned long long>(export_memory_bytes(resource)),
                  resource->retired_exports.size());
    return VA_STATUS_SUCCESS;
}

void vkvv_vulkan_surface_destroy(void *runtime_ptr, VkvvSurface *surface) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    if (runtime == nullptr) {
        return;
    }
    destroy_surface_resource(runtime, surface);
}
