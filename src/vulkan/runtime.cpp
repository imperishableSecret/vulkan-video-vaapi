#include "vulkan/runtime_internal.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace vkvv {

    struct RuntimeDecodeCodec {
        const char*                      name;
        VkVideoCodecOperationFlagBitsKHR operation;
        const char*                      extension;
    };

    // Codecs listed here are runtime-wired. VA advertising is still controlled by
    // the driver capability model, so adding hardware probing alone is not enough.
    constexpr RuntimeDecodeCodec wired_decode_codecs[] = {
        {
            "h264",
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        },
        {
            "hevc",
            VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
            VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        },
        {
            "vp9",
            VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
            VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
        },
        {
            "av1",
            VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
            VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
        },
    };

    struct DecodeQueueSelection {
        uint32_t                      family     = invalid_queue_family;
        VkVideoCodecOperationFlagsKHR operations = 0;
    };

    bool extension_present(const std::vector<VkExtensionProperties>& extensions, const char* name) {
        for (const VkExtensionProperties& extension : extensions) {
            if (std::strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    }

    bool enumerate_device_extensions(VkPhysicalDevice device, std::vector<VkExtensionProperties>& extensions, char* reason, size_t reason_size) {
        uint32_t count  = 0;
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

    bool has_video_decode_base_extensions(const std::vector<VkExtensionProperties>& extensions) {
        return extension_present(extensions, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) && extension_present(extensions, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
    }

    VkVideoCodecOperationFlagsKHR device_wired_decode_operations(const std::vector<VkExtensionProperties>& extensions) {
        if (!has_video_decode_base_extensions(extensions)) {
            return 0;
        }

        VkVideoCodecOperationFlagsKHR operations = 0;
        for (const RuntimeDecodeCodec& codec : wired_decode_codecs) {
            if (extension_present(extensions, codec.extension)) {
                operations |= codec.operation;
            }
        }
        return operations;
    }

    uint32_t count_decode_operations(VkVideoCodecOperationFlagsKHR operations) {
        uint32_t count = 0;
        for (const RuntimeDecodeCodec& codec : wired_decode_codecs) {
            if ((operations & codec.operation) != 0) {
                count++;
            }
        }
        return count;
    }

    std::string codec_operation_names(VkVideoCodecOperationFlagsKHR operations) {
        std::string names;
        for (const RuntimeDecodeCodec& codec : wired_decode_codecs) {
            if ((operations & codec.operation) == 0) {
                continue;
            }
            if (!names.empty()) {
                names += ",";
            }
            names += codec.name;
        }
        return names.empty() ? "none" : names;
    }

    void push_unique_extension(std::vector<const char*>* extensions, const char* extension) {
        for (const char* existing : *extensions) {
            if (std::strcmp(existing, extension) == 0) {
                return;
            }
        }
        extensions->push_back(extension);
    }

    DecodeQueueSelection find_decode_queue_family(VulkanRuntime* runtime, VkPhysicalDevice device, VkVideoCodecOperationFlagsKHR device_operations) {
        uint32_t count = 0;
        runtime->get_queue_family_properties2(device, &count, nullptr);
        if (count == 0) {
            return {};
        }

        std::vector<VkQueueFamilyProperties2>        queue_props(count);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video_props(count);
        for (uint32_t i = 0; i < count; i++) {
            video_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            queue_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            queue_props[i].pNext = &video_props[i];
        }
        runtime->get_queue_family_properties2(device, &count, queue_props.data());

        DecodeQueueSelection best{};
        uint32_t             best_score = 0;
        for (uint32_t i = 0; i < count; i++) {
            const bool has_decode_queue = (queue_props[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0;
            if (!has_decode_queue) {
                continue;
            }

            const VkVideoCodecOperationFlagsKHR queue_operations = video_props[i].videoCodecOperations & device_operations;
            const uint32_t                      score            = count_decode_operations(queue_operations);
            if (score > best_score) {
                best.family     = i;
                best.operations = queue_operations;
                best_score      = score;
            }
        }
        return best;
    }

    bool pick_physical_device(VulkanRuntime* runtime, char* reason, size_t reason_size) {
        uint32_t count  = 0;
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

        struct DeviceCandidate {
            VkPhysicalDevice     device = VK_NULL_HANDLE;
            DecodeQueueSelection queue{};
            bool                 video_maintenance2        = false;
            bool                 external_memory_fd        = false;
            bool                 external_memory_dma_buf   = false;
            bool                 image_drm_format_modifier = false;
            bool                 surface_export            = false;
            bool                 video_decode_vp9          = false;
            uint32_t             score                     = 0;
        };

        DeviceCandidate best{};
        for (VkPhysicalDevice device : devices) {
            std::vector<VkExtensionProperties> extensions;
            if (!enumerate_device_extensions(device, extensions, reason, reason_size)) {
                continue;
            }
            const VkVideoCodecOperationFlagsKHR device_operations = device_wired_decode_operations(extensions);
            if (device_operations == 0) {
                continue;
            }

            const DecodeQueueSelection queue = find_decode_queue_family(runtime, device, device_operations);
            if (queue.family == invalid_queue_family || queue.operations == 0) {
                continue;
            }

            const bool                                has_video_maintenance2        = extension_present(extensions, VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME);
            const bool                                has_external_memory_fd        = extension_present(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            const bool                                has_external_memory_dma_buf   = extension_present(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
            const bool                                has_image_drm_format_modifier = extension_present(extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            const bool                                has_vp9_decode                = extension_present(extensions, VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
            VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9_features{};
            vp9_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR;
            VkPhysicalDeviceVideoMaintenance2FeaturesKHR maintenance2_features{};
            maintenance2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext    = has_vp9_decode ? static_cast<void*>(&vp9_features) : static_cast<void*>(&maintenance2_features);
            vp9_features.pNext = &maintenance2_features;
            vkGetPhysicalDeviceFeatures2(device, &features2);

            VkVideoCodecOperationFlagsKHR queue_operations = queue.operations;
            if ((queue_operations & VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) != 0 && !vp9_features.videoDecodeVP9) {
                queue_operations &= ~VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
            }
            if (queue_operations == 0) {
                continue;
            }

            const bool     surface_export = has_external_memory_fd && has_external_memory_dma_buf && has_image_drm_format_modifier;
            const uint32_t score          = count_decode_operations(queue_operations) * 10 + (surface_export ? 1 : 0);
            if (score <= best.score) {
                continue;
            }

            best.device = device;
            best.queue  = {
                .family     = queue.family,
                .operations = queue_operations,
            };
            best.video_maintenance2        = has_video_maintenance2 && maintenance2_features.videoMaintenance2;
            best.external_memory_fd        = has_external_memory_fd;
            best.external_memory_dma_buf   = has_external_memory_dma_buf;
            best.image_drm_format_modifier = has_image_drm_format_modifier;
            best.surface_export            = surface_export;
            best.video_decode_vp9          = has_vp9_decode && vp9_features.videoDecodeVP9;
            best.score                     = score;
        }

        if (best.device == VK_NULL_HANDLE) {
            std::snprintf(reason, reason_size, "no physical device exposes a wired Vulkan Video decode queue");
            return false;
        }

        runtime->physical_device           = best.device;
        runtime->decode_queue_family       = best.queue.family;
        runtime->probed_decode_operations  = best.queue.operations;
        runtime->enabled_decode_operations = best.queue.operations;
        runtime->probed_encode_operations  = 0;
        runtime->enabled_encode_operations = 0;
        runtime->video_maintenance2        = best.video_maintenance2;
        runtime->external_memory_fd        = best.external_memory_fd;
        runtime->external_memory_dma_buf   = best.external_memory_dma_buf;
        runtime->image_drm_format_modifier = best.image_drm_format_modifier;
        runtime->surface_export            = best.surface_export;
        runtime->video_decode_vp9          = best.video_decode_vp9;
        vkGetPhysicalDeviceMemoryProperties(best.device, &runtime->memory_properties);
        return true;
    }

    bool create_device(VulkanRuntime* runtime, char* reason, size_t reason_size) {
        const float             priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = runtime->decode_queue_family;
        queue_info.queueCount       = 1;
        queue_info.pQueuePriorities = &priority;

        std::vector<const char*> extensions;
        push_unique_extension(&extensions, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        push_unique_extension(&extensions, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        for (const RuntimeDecodeCodec& codec : wired_decode_codecs) {
            if ((runtime->enabled_decode_operations & codec.operation) != 0) {
                push_unique_extension(&extensions, codec.extension);
            }
        }
        if (runtime->video_maintenance2) {
            push_unique_extension(&extensions, VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME);
        }
        if (runtime->external_memory_fd) {
            push_unique_extension(&extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        }
        if (runtime->external_memory_dma_buf) {
            push_unique_extension(&extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        }
        if (runtime->image_drm_format_modifier) {
            push_unique_extension(&extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        }

        VkPhysicalDeviceVideoMaintenance2FeaturesKHR maintenance2{};
        maintenance2.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR;
        maintenance2.videoMaintenance2 = runtime->video_maintenance2 ? VK_TRUE : VK_FALSE;

        VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9{};
        vp9.sType          = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR;
        vp9.videoDecodeVP9 = (runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) != 0 && runtime->video_decode_vp9 ? VK_TRUE : VK_FALSE;

        void* feature_chain = nullptr;
        if (runtime->video_maintenance2) {
            maintenance2.pNext = feature_chain;
            feature_chain      = &maintenance2;
        }
        if (vp9.videoDecodeVP9) {
            vp9.pNext     = feature_chain;
            feature_chain = &vp9;
        }

        VkPhysicalDeviceSynchronization2Features sync2{};
        sync2.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        sync2.pNext            = feature_chain;
        sync2.synchronization2 = VK_TRUE;

        VkDeviceCreateInfo device_info{};
        device_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext                   = &sync2;
        device_info.queueCreateInfoCount    = 1;
        device_info.pQueueCreateInfos       = &queue_info;
        device_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        device_info.ppEnabledExtensionNames = extensions.data();

        VkResult result = vkCreateDevice(runtime->physical_device, &device_info, nullptr, &runtime->device);
        if (result != VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkCreateDevice failed: %d", result);
            return false;
        }
        vkGetDeviceQueue(runtime->device, runtime->decode_queue_family, 0, &runtime->decode_queue);

        runtime->create_video_session  = reinterpret_cast<PFN_vkCreateVideoSessionKHR>(vkGetDeviceProcAddr(runtime->device, "vkCreateVideoSessionKHR"));
        runtime->destroy_video_session = reinterpret_cast<PFN_vkDestroyVideoSessionKHR>(vkGetDeviceProcAddr(runtime->device, "vkDestroyVideoSessionKHR"));
        runtime->get_video_session_memory_requirements =
            reinterpret_cast<PFN_vkGetVideoSessionMemoryRequirementsKHR>(vkGetDeviceProcAddr(runtime->device, "vkGetVideoSessionMemoryRequirementsKHR"));
        runtime->bind_video_session_memory = reinterpret_cast<PFN_vkBindVideoSessionMemoryKHR>(vkGetDeviceProcAddr(runtime->device, "vkBindVideoSessionMemoryKHR"));
        runtime->create_video_session_parameters =
            reinterpret_cast<PFN_vkCreateVideoSessionParametersKHR>(vkGetDeviceProcAddr(runtime->device, "vkCreateVideoSessionParametersKHR"));
        runtime->destroy_video_session_parameters =
            reinterpret_cast<PFN_vkDestroyVideoSessionParametersKHR>(vkGetDeviceProcAddr(runtime->device, "vkDestroyVideoSessionParametersKHR"));
        runtime->cmd_begin_video_coding   = reinterpret_cast<PFN_vkCmdBeginVideoCodingKHR>(vkGetDeviceProcAddr(runtime->device, "vkCmdBeginVideoCodingKHR"));
        runtime->cmd_end_video_coding     = reinterpret_cast<PFN_vkCmdEndVideoCodingKHR>(vkGetDeviceProcAddr(runtime->device, "vkCmdEndVideoCodingKHR"));
        runtime->cmd_control_video_coding = reinterpret_cast<PFN_vkCmdControlVideoCodingKHR>(vkGetDeviceProcAddr(runtime->device, "vkCmdControlVideoCodingKHR"));
        runtime->cmd_decode_video         = reinterpret_cast<PFN_vkCmdDecodeVideoKHR>(vkGetDeviceProcAddr(runtime->device, "vkCmdDecodeVideoKHR"));
        runtime->get_memory_fd            = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(runtime->device, "vkGetMemoryFdKHR"));
        runtime->get_image_drm_format_modifier_properties =
            reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(vkGetDeviceProcAddr(runtime->device, "vkGetImageDrmFormatModifierPropertiesEXT"));

        if (runtime->create_video_session == nullptr || runtime->destroy_video_session == nullptr || runtime->get_video_session_memory_requirements == nullptr ||
            runtime->bind_video_session_memory == nullptr || runtime->create_video_session_parameters == nullptr || runtime->destroy_video_session_parameters == nullptr ||
            runtime->cmd_begin_video_coding == nullptr || runtime->cmd_end_video_coding == nullptr || runtime->cmd_control_video_coding == nullptr ||
            runtime->cmd_decode_video == nullptr) {
            std::snprintf(reason, reason_size, "required Vulkan Video device entrypoints unavailable");
            return false;
        }
        if (runtime->surface_export && (runtime->get_memory_fd == nullptr || runtime->get_image_drm_format_modifier_properties == nullptr)) {
            runtime->surface_export = false;
        }

        return true;
    }

} // namespace vkvv

using namespace vkvv;

void* vkvv_vulkan_runtime_create(char* reason, size_t reason_size) {
    try {
        auto              runtime = new VulkanRuntime();

        VkApplicationInfo app_info{};
        app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName   = "nvidia-vulkan-vaapi";
        app_info.applicationVersion = 1;
        app_info.pEngineName        = "nvidia-vulkan-vaapi";
        app_info.engineVersion      = 1;
        app_info.apiVersion         = VK_API_VERSION_1_3;

        VkInstanceCreateInfo instance_info{};
        instance_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app_info;

        VkResult result = vkCreateInstance(&instance_info, nullptr, &runtime->instance);
        if (result != VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkCreateInstance failed: %d", result);
            delete runtime;
            return nullptr;
        }

        runtime->get_queue_family_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(vkGetInstanceProcAddr(runtime->instance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
        runtime->get_video_capabilities =
            reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(vkGetInstanceProcAddr(runtime->instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
        runtime->get_video_format_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(vkGetInstanceProcAddr(runtime->instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
        if (runtime->get_queue_family_properties2 == nullptr || runtime->get_video_capabilities == nullptr || runtime->get_video_format_properties == nullptr) {
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

        const std::string codec_names = codec_operation_names(runtime->enabled_decode_operations);
        std::snprintf(reason, reason_size, "Vulkan Video runtime ready: queue_family=%u codecs=%s", runtime->decode_queue_family, codec_names.c_str());
        return runtime;
    } catch (const std::bad_alloc&) {
        std::snprintf(reason, reason_size, "out of memory creating Vulkan runtime");
        return nullptr;
    }
}

void vkvv_vulkan_runtime_destroy(void* runtime) {
    delete static_cast<VulkanRuntime*>(runtime);
}

bool vkvv_vulkan_supports_surface_export(void* runtime_ptr) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    return runtime != nullptr && runtime->surface_export;
}
