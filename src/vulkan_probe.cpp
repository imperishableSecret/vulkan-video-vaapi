#include "driver.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <vulkan/vulkan.h>

namespace {

struct VideoProbeProfile {
    VkVideoCodecOperationFlagBitsKHR operation;
    const char *extension;
    const void *codec_profile;
    VkStructureType codec_capabilities_type;
    VkVideoChromaSubsamplingFlagsKHR chroma_subsampling;
    VkVideoComponentBitDepthFlagsKHR bit_depth;
};

class VulkanInstance {
  public:
    explicit VulkanInstance(VkInstance instance) : instance_(instance) {}

    VulkanInstance(const VulkanInstance &) = delete;
    VulkanInstance &operator=(const VulkanInstance &) = delete;

    ~VulkanInstance() {
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    VkInstance get() const {
        return instance_;
    }

  private:
    VkInstance instance_;
};

struct VideoCapabilitiesChain {
    VkVideoDecodeH264CapabilitiesKHR h264{};
    VkVideoDecodeH265CapabilitiesKHR h265{};
    VkVideoDecodeVP9CapabilitiesKHR vp9{};
    VkVideoDecodeAV1CapabilitiesKHR av1{};
    VkVideoDecodeCapabilitiesKHR decode{};
    VkVideoCapabilitiesKHR video{};

    explicit VideoCapabilitiesChain(VkStructureType codec_capabilities_type) {
        h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
        h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
        vp9.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR;
        av1.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;

        void *codec_caps = nullptr;
        switch (codec_capabilities_type) {
            case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR:
                codec_caps = &h264;
                break;
            case VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR:
                codec_caps = &h265;
                break;
            case VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR:
                codec_caps = &vp9;
                break;
            case VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR:
                codec_caps = &av1;
                break;
            default:
                break;
        }

        decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
        decode.pNext = codec_caps;

        video.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        video.pNext = &decode;
    }
};

bool env_enabled(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0;
}

bool extension_present(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    for (const VkExtensionProperties &extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

void assume_caps(VkvvVideoCaps *caps, const char *reason) {
    const VkvvVideoProfileLimits fallback_limits = {
        1,
        1,
        4096,
        4096,
        17,
        16,
    };
    caps->h264 = true;
    caps->h265 = true;
    caps->h265_10 = true;
    caps->vp9 = true;
    caps->av1 = true;
    caps->surface_export = true;
    caps->h264_limits = fallback_limits;
    caps->h265_limits = fallback_limits;
    caps->h265_10_limits = fallback_limits;
    caps->vp9_limits = fallback_limits;
    caps->av1_limits = fallback_limits;
    std::snprintf(caps->summary, sizeof(caps->summary),
                  "assuming H264/H265/H265Main10/VP9/AV1 support: %s", reason);
}

bool device_has_required_extensions(VkPhysicalDevice device, const char *codec_extension) {
    uint32_t extension_count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS || extension_count == 0) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extension_count);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }
    extensions.resize(extension_count);

    return extension_present(extensions, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) &&
           extension_present(extensions, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) &&
           extension_present(extensions, codec_extension);
}

bool device_has_surface_export_extensions(VkPhysicalDevice device) {
    uint32_t extension_count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS || extension_count == 0) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extension_count);
    result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
    if (result != VK_SUCCESS) {
        return false;
    }
    extensions.resize(extension_count);

    return extension_present(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
           extension_present(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
           extension_present(extensions, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

bool device_has_decode_queue(
        PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_queue_family_properties2,
        VkPhysicalDevice device,
        VkVideoCodecOperationFlagBitsKHR operation) {
    uint32_t queue_family_count = 0;
    get_queue_family_properties2(device, &queue_family_count, nullptr);
    if (queue_family_count == 0) {
        return false;
    }

    std::vector<VkQueueFamilyProperties2> queue_props(queue_family_count);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video_props(queue_family_count);
    for (uint32_t i = 0; i < queue_family_count; i++) {
        video_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        queue_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        queue_props[i].pNext = &video_props[i];
    }

    get_queue_family_properties2(device, &queue_family_count, queue_props.data());

    for (uint32_t i = 0; i < queue_family_count; i++) {
        const bool queue_advertises_decode =
            (queue_props[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0;
        const bool queue_supports_codec =
            (video_props[i].videoCodecOperations & operation) != 0;
        if (queue_advertises_decode && queue_supports_codec) {
            return true;
        }
    }

    return false;
}

bool device_supports_video_profile(
        PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_queue_family_properties2,
        PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_video_capabilities,
        VkPhysicalDevice device,
        const VideoProbeProfile &probe,
        VkvvVideoProfileLimits *limits,
        char *reason,
        size_t reason_size) {
    if (!device_has_required_extensions(device, probe.extension)) {
        std::snprintf(reason, reason_size, "missing-ext");
        return false;
    }

    if (!device_has_decode_queue(get_queue_family_properties2, device, probe.operation)) {
        std::snprintf(reason, reason_size, "no-decode-queue");
        return false;
    }

    VideoCapabilitiesChain capabilities(probe.codec_capabilities_type);
    if (capabilities.decode.pNext == nullptr) {
        std::snprintf(reason, reason_size, "bad-cap-chain");
        return false;
    }

    VkVideoProfileInfoKHR profile{};
    profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile.pNext = probe.codec_profile;
    profile.videoCodecOperation = probe.operation;
    profile.chromaSubsampling = probe.chroma_subsampling;
    profile.lumaBitDepth = probe.bit_depth;
    profile.chromaBitDepth = probe.bit_depth;

    VkResult result = get_video_capabilities(device, &profile, &capabilities.video);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "caps-result=%d", result);
        return false;
    }

    if (capabilities.video.maxDpbSlots == 0 || capabilities.video.maxActiveReferencePictures == 0) {
        std::snprintf(reason, reason_size, "dpb=%u refs=%u",
                      capabilities.video.maxDpbSlots,
                      capabilities.video.maxActiveReferencePictures);
        return false;
    }

    if (limits != nullptr) {
        limits->min_width = capabilities.video.minCodedExtent.width;
        limits->min_height = capabilities.video.minCodedExtent.height;
        limits->max_width = capabilities.video.maxCodedExtent.width;
        limits->max_height = capabilities.video.maxCodedExtent.height;
        limits->max_dpb_slots = capabilities.video.maxDpbSlots;
        limits->max_active_references = capabilities.video.maxActiveReferencePictures;
    }

    std::snprintf(reason, reason_size, "ok");
    return true;
}

bool probe_one_profile(
        bool already_supported,
        PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_queue_family_properties2,
        PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_video_capabilities,
        VkPhysicalDevice device,
        const VideoProbeProfile &probe,
        VkvvVideoProfileLimits *limits,
        char *reason,
        size_t reason_size) {
    if (already_supported) {
        return true;
    }

    return device_supports_video_profile(
        get_queue_family_properties2, get_video_capabilities, device, probe,
        limits, reason, reason_size);
}

bool probe_impl(VkvvVideoCaps *caps) {
    if (env_enabled("VKVV_ASSUME_CAPS")) {
        assume_caps(caps, "VKVV_ASSUME_CAPS is set");
        return true;
    }

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

    VkInstance raw_instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, nullptr, &raw_instance);
    if (result != VK_SUCCESS) {
        std::snprintf(caps->summary, sizeof(caps->summary), "vkCreateInstance failed: %d", result);
        return false;
    }
    VulkanInstance instance(raw_instance);

    auto get_queue_family_properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
            vkGetInstanceProcAddr(instance.get(), "vkGetPhysicalDeviceQueueFamilyProperties2"));
    auto get_video_capabilities =
        reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
            vkGetInstanceProcAddr(instance.get(), "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    if (get_queue_family_properties2 == nullptr || get_video_capabilities == nullptr) {
        std::snprintf(caps->summary, sizeof(caps->summary), "required Vulkan Video query entrypoints unavailable");
        return false;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance.get(), &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        std::snprintf(caps->summary, sizeof(caps->summary),
                      "no Vulkan physical devices: result=%d count=%u", result, device_count);
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance.get(), &device_count, devices.data());
    if (result != VK_SUCCESS) {
        std::snprintf(caps->summary, sizeof(caps->summary), "vkEnumeratePhysicalDevices failed: %d", result);
        return false;
    }
    devices.resize(device_count);

    VkVideoDecodeH264ProfileInfoKHR h264_profile{};
    h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

    VkVideoDecodeH265ProfileInfoKHR h265_profile{};
    h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
    h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;

    VkVideoDecodeH265ProfileInfoKHR h265_10_profile{};
    h265_10_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
    h265_10_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;

    VkVideoDecodeVP9ProfileInfoKHR vp9_profile{};
    vp9_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
    vp9_profile.stdProfile = STD_VIDEO_VP9_PROFILE_0;

    VkVideoDecodeAV1ProfileInfoKHR av1_profile{};
    av1_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
    av1_profile.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
    av1_profile.filmGrainSupport = VK_FALSE;

    const std::array<VideoProbeProfile, 5> probes = {{
        {
            VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
            &h264_profile,
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR,
            VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        },
        {
            VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
            VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
            &h265_profile,
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR,
            VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        },
        {
            VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
            VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
            &h265_10_profile,
            VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR,
            VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
        },
        {
            VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
            VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
            &vp9_profile,
            VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR,
            VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        },
        {
            VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
            VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
            &av1_profile,
            VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR,
            VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        },
    }};

    char h264_reason[64] = "not-probed";
    char h265_reason[64] = "not-probed";
    char h265_10_reason[64] = "not-probed";
    char vp9_reason[64] = "not-probed";
    char av1_reason[64] = "not-probed";

    for (VkPhysicalDevice device : devices) {
        caps->h264 = probe_one_profile(
            caps->h264, get_queue_family_properties2, get_video_capabilities,
            device, probes[0], &caps->h264_limits, h264_reason, sizeof(h264_reason));
        caps->h265 = probe_one_profile(
            caps->h265, get_queue_family_properties2, get_video_capabilities,
            device, probes[1], &caps->h265_limits, h265_reason, sizeof(h265_reason));
        caps->h265_10 = probe_one_profile(
            caps->h265_10, get_queue_family_properties2, get_video_capabilities,
            device, probes[2], &caps->h265_10_limits, h265_10_reason, sizeof(h265_10_reason));
        caps->vp9 = probe_one_profile(
            caps->vp9, get_queue_family_properties2, get_video_capabilities,
            device, probes[3], &caps->vp9_limits, vp9_reason, sizeof(vp9_reason));
        caps->av1 = probe_one_profile(
            caps->av1, get_queue_family_properties2, get_video_capabilities,
            device, probes[4], &caps->av1_limits, av1_reason, sizeof(av1_reason));
        if (!caps->surface_export &&
            device_has_surface_export_extensions(device) &&
            device_has_required_extensions(device, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME) &&
            device_has_decode_queue(get_queue_family_properties2, device, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)) {
            caps->surface_export = true;
        }
    }

    std::snprintf(caps->summary, sizeof(caps->summary),
                  "Vulkan Video profile caps: h264=%d(%s) h265=%d(%s) h265_10=%d(%s) vp9=%d(%s) av1=%d(%s) export=%d",
                  caps->h264, h264_reason,
                  caps->h265, h265_reason,
                  caps->h265_10, h265_10_reason,
                  caps->vp9, vp9_reason,
                  caps->av1, av1_reason,
                  caps->surface_export);

    return caps->h264 || caps->h265 || caps->h265_10 || caps->vp9 || caps->av1;
}

} // namespace

bool vkvv_probe_vulkan_video(VkvvVideoCaps *caps) {
    if (caps == nullptr) {
        return false;
    }

    std::memset(caps, 0, sizeof(*caps));
    try {
        return probe_impl(caps);
    } catch (const std::bad_alloc &) {
        std::snprintf(caps->summary, sizeof(caps->summary), "out of memory probing Vulkan Video caps");
        return false;
    } catch (...) {
        std::snprintf(caps->summary, sizeof(caps->summary), "unexpected exception probing Vulkan Video caps");
        return false;
    }
}
