#include "vulkan/formats.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace vkvv {

    VkFormat preferred_vk_format_for_rt_format(unsigned int rt_format) {
        if (rt_format & VA_RT_FORMAT_YUV420_12) {
            return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        }
        if (rt_format & VA_RT_FORMAT_YUV420_10) {
            return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        }
        return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    }

    bool choose_decode_format(VulkanRuntime* runtime, const VkVideoProfileInfoKHR* profile, VkImageUsageFlags query_usage, VkFormat preferred_format, bool prefer_export_tiling,
                              DecodeFormatSelection* selection, char* reason, size_t reason_size) {
        VkVideoProfileListInfoKHR profile_list{};
        profile_list.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        profile_list.profileCount = 1;
        profile_list.pProfiles    = profile;

        VkPhysicalDeviceVideoFormatInfoKHR format_info{};
        format_info.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
        format_info.pNext      = &profile_list;
        format_info.imageUsage = query_usage;

        uint32_t count  = 0;
        VkResult result = runtime->get_video_format_properties(runtime->physical_device, &format_info, &count, nullptr);
        if (result != VK_SUCCESS || count == 0) {
            std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoFormatPropertiesKHR failed: result=%d count=%u", result, count);
            return false;
        }

        std::vector<VkVideoFormatPropertiesKHR> properties(count);
        for (VkVideoFormatPropertiesKHR& property : properties) {
            property.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
        }
        result = runtime->get_video_format_properties(runtime->physical_device, &format_info, &count, properties.data());
        if (result != VK_SUCCESS || count == 0) {
            std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoFormatPropertiesKHR failed: %d", result);
            return false;
        }
        properties.resize(count);

        auto preferred = properties.end();
        if (prefer_export_tiling) {
            preferred = std::find_if(properties.begin(), properties.end(), [preferred_format](const VkVideoFormatPropertiesKHR& property) {
                return property.format == preferred_format && property.imageTiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            });
        }
        if (preferred == properties.end()) {
            preferred =
                std::find_if(properties.begin(), properties.end(), [preferred_format](const VkVideoFormatPropertiesKHR& property) { return property.format == preferred_format; });
        }
        if (preferred == properties.end()) {
            preferred = std::find_if(properties.begin(), properties.end(),
                                     [](const VkVideoFormatPropertiesKHR& property) { return property.imageTiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; });
        }

        const VkVideoFormatPropertiesKHR& selected = preferred != properties.end() ? *preferred : properties.front();
        selection->format                          = selected.format;
        selection->create_flags                    = selected.imageCreateFlags;
        selection->tiling                          = selected.imageTiling;
        return true;
    }

} // namespace vkvv
