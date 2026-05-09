#ifndef VKVV_VULKAN_FORMATS_H
#define VKVV_VULKAN_FORMATS_H

#include "vulkan/runtime_internal.h"

namespace vkvv {

struct DecodeFormatSelection {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags create_flags = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
};

bool choose_decode_format(
        VulkanRuntime *runtime,
        const VkVideoProfileInfoKHR *profile,
        VkImageUsageFlags query_usage,
        VkFormat preferred_format,
        bool prefer_export_tiling,
        DecodeFormatSelection *selection,
        char *reason,
        size_t reason_size);
VkFormat preferred_vk_format_for_rt_format(unsigned int rt_format);

} // namespace vkvv

#endif
