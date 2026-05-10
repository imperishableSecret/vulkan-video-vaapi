#include "vulkan/formats.h"
#include "telemetry.h"

#include <cstdio>
#include <vector>

namespace vkvv {

    const char* decode_image_tiling_name(VkImageTiling tiling) {
        switch (tiling) {
            case VK_IMAGE_TILING_OPTIMAL: return "optimal";
            case VK_IMAGE_TILING_LINEAR: return "linear";
            case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: return "drm-modifier";
            default: return "unknown";
        }
    }

    bool decode_tiling_has_export_layout(VkImageTiling tiling) {
        return tiling == VK_IMAGE_TILING_LINEAR || tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    }

    VkFormat preferred_vk_format_for_rt_format(unsigned int rt_format) {
        if (rt_format & VA_RT_FORMAT_YUV420_12) {
            return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
        }
        if (rt_format & VA_RT_FORMAT_YUV420_10) {
            return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        }
        return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    }

    bool choose_decode_format_from_properties(const VkVideoFormatPropertiesKHR* properties, uint32_t count, VkFormat preferred_format, bool prefer_export_tiling,
                                              DecodeFormatSelection* selection) {
        if (properties == nullptr || count == 0 || selection == nullptr) {
            return false;
        }

        *selection                       = {};
        selection->format_property_count = count;

        const VkVideoFormatPropertiesKHR* preferred_export = nullptr;
        const VkVideoFormatPropertiesKHR* preferred_any    = nullptr;
        const VkVideoFormatPropertiesKHR* export_any       = nullptr;

        for (uint32_t i = 0; i < count; i++) {
            const VkVideoFormatPropertiesKHR& property      = properties[i];
            const bool                        export_tiling = decode_tiling_has_export_layout(property.imageTiling);
            const bool                        preferred     = property.format == preferred_format;
            if (export_tiling) {
                selection->export_tiling_candidate_count++;
                if (export_any == nullptr) {
                    export_any = &property;
                }
            }
            if (preferred) {
                if (preferred_any == nullptr) {
                    preferred_any = &property;
                }
                if (export_tiling) {
                    selection->preferred_export_tiling_candidate_count++;
                    if (preferred_export == nullptr) {
                        preferred_export = &property;
                    }
                }
            }
        }

        const VkVideoFormatPropertiesKHR* selected = nullptr;
        if (prefer_export_tiling && preferred_export != nullptr) {
            selected = preferred_export;
        } else if (preferred_any != nullptr) {
            selected = preferred_any;
        } else if (export_any != nullptr) {
            selected = export_any;
        } else {
            selected = &properties[0];
        }

        selection->format                  = selected->format;
        selection->create_flags            = selected->imageCreateFlags;
        selection->tiling                  = selected->imageTiling;
        selection->direct_export_candidate = selected->format == preferred_format && decode_tiling_has_export_layout(selected->imageTiling);
        return true;
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

        if (!choose_decode_format_from_properties(properties.data(), count, preferred_format, prefer_export_tiling, selection)) {
            std::snprintf(reason, reason_size, "failed to select decode video format");
            return false;
        }

        vkvv_trace("decode-format-select",
                   "usage=0x%x preferred=%d prefer_export=%u properties=%u export_tiling=%u preferred_export_tiling=%u selected_format=%d selected_tiling=%s "
                   "direct_export=%u create_flags=0x%x",
                   query_usage, preferred_format, prefer_export_tiling ? 1U : 0U, selection->format_property_count, selection->export_tiling_candidate_count,
                   selection->preferred_export_tiling_candidate_count, selection->format, decode_image_tiling_name(selection->tiling), selection->direct_export_candidate ? 1U : 0U,
                   selection->create_flags);
        return true;
    }

} // namespace vkvv
