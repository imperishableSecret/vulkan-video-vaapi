#include "internal.h"
#include "api.h"

#include <algorithm>
#include <cstdio>
#include <new>
#include <vector>

namespace vkvv {

struct H264FormatSelection {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags create_flags = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
};

bool choose_h264_format(
        VulkanRuntime *runtime,
        const VkVideoProfileInfoKHR *profile,
        H264FormatSelection *selection,
        char *reason,
        size_t reason_size) {
    VkVideoProfileListInfoKHR profile_list{};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = profile;

    VkPhysicalDeviceVideoFormatInfoKHR format_info{};
    format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    format_info.pNext = &profile_list;
    format_info.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;

    uint32_t count = 0;
    VkResult result = runtime->get_video_format_properties(runtime->physical_device, &format_info, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoFormatPropertiesKHR failed: result=%d count=%u", result, count);
        return false;
    }

    std::vector<VkVideoFormatPropertiesKHR> properties(count);
    for (VkVideoFormatPropertiesKHR &property : properties) {
        property.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    result = runtime->get_video_format_properties(runtime->physical_device, &format_info, &count, properties.data());
    if (result != VK_SUCCESS || count == 0) {
        std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoFormatPropertiesKHR failed: %d", result);
        return false;
    }
    properties.resize(count);

    auto preferred = properties.end();
    if (runtime->surface_export) {
        preferred = std::find_if(properties.begin(), properties.end(), [](const VkVideoFormatPropertiesKHR &property) {
            return property.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
                   property.imageTiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        });
    }
    if (preferred == properties.end()) {
        preferred = std::find_if(properties.begin(), properties.end(), [](const VkVideoFormatPropertiesKHR &property) {
            return property.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        });
    }
    if (preferred == properties.end()) {
        preferred = std::find_if(properties.begin(), properties.end(), [](const VkVideoFormatPropertiesKHR &property) {
            return property.imageTiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        });
    }
    const VkVideoFormatPropertiesKHR &selected = preferred != properties.end() ? *preferred : properties.front();
    selection->format = selected.format;
    selection->create_flags = selected.imageCreateFlags;
    selection->tiling = selected.imageTiling;
    return true;
}

VkExtent2D h264_session_extent(VkExtent2D requested, const VkVideoCapabilitiesKHR &capabilities) {
    constexpr uint32_t min_nvidia_h264_session_dimension = 256;
    return {
        std::min(std::max({requested.width, capabilities.minCodedExtent.width, min_nvidia_h264_session_dimension}),
                 capabilities.maxCodedExtent.width),
        std::min(std::max({requested.height, capabilities.minCodedExtent.height, min_nvidia_h264_session_dimension}),
                 capabilities.maxCodedExtent.height),
    };
}
} // namespace vkvv

using namespace vkvv;

void *vkvv_vulkan_h264_session_create(void) {
    try {
        return new H264VideoSession();
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

void vkvv_vulkan_h264_session_destroy(void *runtime_ptr, void *session_ptr) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    auto *session = static_cast<H264VideoSession *>(session_ptr);
    destroy_h264_video_session(runtime, session);
    delete session;
}

VAStatus vkvv_vulkan_ensure_h264_session(
        void *runtime_ptr,
        void *session_ptr,
        unsigned int width,
        unsigned int height,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    auto *session = static_cast<H264VideoSession *>(session_ptr);
    if (runtime == nullptr || session == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan H.264 session state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    const VkExtent2D extent = {
        .width = round_up_16(width),
        .height = round_up_16(height),
    };
    if (session->video.session != VK_NULL_HANDLE &&
        session->video.key.max_coded_extent.width >= extent.width &&
        session->video.key.max_coded_extent.height >= extent.height) {
        std::snprintf(reason, reason_size,
                      "H.264 video session ready: codec=h264 actual=%ux%u format=%d mem=%llu inline_params=%u",
                      session->video.key.max_coded_extent.width,
                      session->video.key.max_coded_extent.height,
                      session->video.key.picture_format,
                      static_cast<unsigned long long>(session->video.memory_bytes),
                      runtime->video_maintenance2);
        return VA_STATUS_SUCCESS;
    }

    destroy_h264_video_session(runtime, session);

    VideoProfileChain profile_chain(h264_profile_spec);
    VideoCapabilitiesChain capabilities(h264_profile_spec);
    VkResult result = runtime->get_video_capabilities(runtime->physical_device, &profile_chain.profile, &capabilities.video);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoCapabilitiesKHR(H.264) failed: %d", result);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    session->bitstream_offset_alignment = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferOffsetAlignment);
    session->bitstream_size_alignment = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferSizeAlignment);
    session->decode_flags = capabilities.decode.flags;
    session->max_level = capabilities.h264.maxLevelIdc;
    if (extent.width > capabilities.video.maxCodedExtent.width ||
        extent.height > capabilities.video.maxCodedExtent.height) {
        std::snprintf(reason, reason_size, "H.264 coded extent %ux%u exceeds Vulkan limit %ux%u",
                      extent.width, extent.height,
                      capabilities.video.maxCodedExtent.width,
                      capabilities.video.maxCodedExtent.height);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }

    H264FormatSelection format_selection{};
    if (!choose_h264_format(runtime, &profile_chain.profile, &format_selection, reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    const VkExtent2D session_extent = h264_session_extent(extent, capabilities.video);

    VkVideoSessionCreateInfoKHR session_info{};
    session_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_info.queueFamilyIndex = runtime->decode_queue_family;
    session_info.pVideoProfile = &profile_chain.profile;
    session_info.pictureFormat = format_selection.format;
    session_info.maxCodedExtent = session_extent;
    session_info.referencePictureFormat = format_selection.format;
    session_info.maxDpbSlots = std::min<uint32_t>(capabilities.video.maxDpbSlots, max_h264_dpb_slots);
    session_info.maxActiveReferencePictures = std::min<uint32_t>(capabilities.video.maxActiveReferencePictures, max_va_h264_reference_frames);
    session_info.pStdHeaderVersion = &capabilities.video.stdHeaderVersion;
    if (runtime->video_maintenance2) {
        session_info.flags |= VK_VIDEO_SESSION_CREATE_INLINE_SESSION_PARAMETERS_BIT_KHR;
    }

    result = runtime->create_video_session(runtime->device, &session_info, nullptr, &session->video.session);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateVideoSessionKHR(H.264) failed: %d", result);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->video.key = {
        .codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
        .codec_profile = static_cast<uint32_t>(profile_chain.h264.stdProfileIdc),
        .picture_format = format_selection.format,
        .reference_picture_format = format_selection.format,
        .max_coded_extent = session_extent,
        .image_usage = h264_surface_image_usage(),
        .image_create_flags = format_selection.create_flags,
        .image_tiling = format_selection.tiling,
        .chroma_subsampling = profile_chain.profile.chromaSubsampling,
        .luma_bit_depth = profile_chain.profile.lumaBitDepth,
        .chroma_bit_depth = profile_chain.profile.chromaBitDepth,
    };

    if (!bind_video_session_memory(runtime, &session->video, reason, reason_size)) {
        destroy_h264_video_session(runtime, session);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->max_dpb_slots = session_info.maxDpbSlots;
    session->max_active_reference_pictures = session_info.maxActiveReferencePictures;
    std::snprintf(reason, reason_size,
                  "H.264 video session ready: codec=h264 requested=%ux%u actual=%ux%u format=%d dpb=%u refs=%u mem=%llu inline_params=%u",
                  extent.width, extent.height, session_extent.width, session_extent.height, format_selection.format,
                  session_info.maxDpbSlots, session_info.maxActiveReferencePictures,
                  static_cast<unsigned long long>(session->video.memory_bytes),
                  runtime->video_maintenance2);
    return VA_STATUS_SUCCESS;
}
