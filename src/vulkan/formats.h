#ifndef VKVV_VULKAN_FORMATS_H
#define VKVV_VULKAN_FORMATS_H

#include "vulkan/runtime_internal.h"

namespace vkvv {

    struct DecodeFormatSelection {
        VkFormat           format                                  = VK_FORMAT_UNDEFINED;
        VkImageCreateFlags create_flags                            = 0;
        VkImageTiling      tiling                                  = VK_IMAGE_TILING_OPTIMAL;
        uint32_t           format_property_count                   = 0;
        uint32_t           export_tiling_candidate_count           = 0;
        uint32_t           preferred_export_tiling_candidate_count = 0;
        bool               direct_export_candidate                 = false;
    };

    bool     choose_decode_format_from_properties(const VkVideoFormatPropertiesKHR* properties, uint32_t count, VkFormat preferred_format, bool prefer_export_tiling,
                                                  DecodeFormatSelection* selection);
    bool     choose_decode_format(VulkanRuntime* runtime, const VkVideoProfileInfoKHR* profile, VkImageUsageFlags query_usage, VkFormat preferred_format, bool prefer_export_tiling,
                                  DecodeFormatSelection* selection, char* reason, size_t reason_size);
    VkFormat preferred_vk_format_for_rt_format(unsigned int rt_format);
    const char* decode_image_tiling_name(VkImageTiling tiling);

} // namespace vkvv

#endif
