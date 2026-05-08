#ifndef VKVV_VULKAN_VIDEO_PROFILE_H
#define VKVV_VULKAN_VIDEO_PROFILE_H

#include <cstdint>
#include <vulkan/vulkan.h>

namespace vkvv {

struct VideoProfileSpec {
    VkVideoCodecOperationFlagBitsKHR operation = VK_VIDEO_CODEC_OPERATION_NONE_KHR;
    VkVideoComponentBitDepthFlagsKHR bit_depth = VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
    uint32_t std_profile = UINT32_MAX;
};

struct VideoCapabilitiesChain {
    VkVideoDecodeH264CapabilitiesKHR h264{};
    VkVideoDecodeH265CapabilitiesKHR h265{};
    VkVideoDecodeVP9CapabilitiesKHR vp9{};
    VkVideoDecodeAV1CapabilitiesKHR av1{};
    VkVideoDecodeCapabilitiesKHR decode{};
    VkVideoCapabilitiesKHR video{};

    explicit VideoCapabilitiesChain(const VideoProfileSpec &spec);
};

struct VideoProfileChain {
    VkVideoDecodeH264ProfileInfoKHR h264{};
    VkVideoDecodeH265ProfileInfoKHR h265{};
    VkVideoDecodeVP9ProfileInfoKHR vp9{};
    VkVideoDecodeAV1ProfileInfoKHR av1{};
    VkVideoDecodeUsageInfoKHR usage{};
    VkVideoProfileInfoKHR profile{};

    explicit VideoProfileChain(const VideoProfileSpec &spec);
};

} // namespace vkvv

#endif
