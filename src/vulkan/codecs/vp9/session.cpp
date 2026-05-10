#include "internal.h"
#include "api.h"
#include "vulkan/formats.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <new>

namespace vkvv {

    VP9VideoSession* create_vp9_session(VAProfile va_profile, unsigned int va_rt_format, unsigned int va_fourcc, uint8_t bitstream_profile, uint8_t bit_depth,
                                        VideoProfileSpec profile_spec) {
        auto* session              = new VP9VideoSession();
        session->va_profile        = va_profile;
        session->va_rt_format      = va_rt_format;
        session->va_fourcc         = va_fourcc;
        session->bitstream_profile = bitstream_profile;
        session->bit_depth         = bit_depth;
        session->profile_spec      = profile_spec;
        return session;
    }

    VkImageUsageFlags vp9_surface_image_usage() {
        return VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    void destroy_vp9_video_session(VulkanRuntime* runtime, VP9VideoSession* session) {
        if (session == nullptr) {
            return;
        }
        for (UploadBuffer& upload : session->uploads) {
            destroy_upload_buffer(runtime, &upload);
        }
        destroy_video_session(runtime, &session->video);
        for (VP9ReferenceSlot& slot : session->reference_slots) {
            slot = {};
        }
        session->bitstream_offset_alignment    = 1;
        session->bitstream_size_alignment      = 1;
        session->max_level                     = STD_VIDEO_VP9_LEVEL_6_2;
        session->decode_flags                  = 0;
        session->next_dpb_slot                 = 0;
        session->max_dpb_slots                 = 0;
        session->max_active_reference_pictures = 0;
    }

    bool reset_vp9_session(VulkanRuntime* runtime, VP9VideoSession* session, char* reason, size_t reason_size) {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "VP9 session reset", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "VP9 session reset", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "VP9 session reset", reason, reason_size)) {
            return false;
        }

        VkVideoBeginCodingInfoKHR video_begin{};
        video_begin.sType        = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
        video_begin.videoSession = session->video.session;
        runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

        VkVideoCodingControlInfoKHR control{};
        control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
        control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        runtime->cmd_control_video_coding(runtime->command_buffer, &control);

        VkVideoEndCodingInfoKHR video_end{};
        video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
        runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "VP9 session reset", reason, reason_size)) {
            return false;
        }

        if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "VP9 session reset")) {
            return false;
        }

        session->video.initialized = true;
        return true;
    }

    VkExtent2D vp9_session_extent(VkExtent2D requested, const VkVideoCapabilitiesKHR& capabilities) {
        return {
            std::min(std::max(requested.width, capabilities.minCodedExtent.width), capabilities.maxCodedExtent.width),
            std::min(std::max(requested.height, capabilities.minCodedExtent.height), capabilities.maxCodedExtent.height),
        };
    }

} // namespace vkvv

using namespace vkvv;

void* vkvv_vulkan_vp9_session_create(void) {
    try {
        return create_vp9_session(VAProfileVP9Profile0, VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, 0, 8, vp9_profile0_spec);
    } catch (const std::bad_alloc&) { return nullptr; }
}

void* vkvv_vulkan_vp9_profile2_session_create(void) {
    try {
        return create_vp9_session(VAProfileVP9Profile2, VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010, 2, 10, vp9_profile2_10bit_spec);
    } catch (const std::bad_alloc&) { return nullptr; }
}

void vkvv_vulkan_vp9_session_destroy(void* runtime_ptr, void* session_ptr) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<VP9VideoSession*>(session_ptr);
    destroy_vp9_video_session(runtime, session);
    delete session;
}

VAStatus vkvv_vulkan_ensure_vp9_session(void* runtime_ptr, void* session_ptr, unsigned int width, unsigned int height, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<VP9VideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan VP9 session state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    const VkExtent2D extent = {
        .width  = round_up_16(width),
        .height = round_up_16(height),
    };
    if (session->video.session != VK_NULL_HANDLE && session->video.key.max_coded_extent.width >= extent.width && session->video.key.max_coded_extent.height >= extent.height) {
        std::snprintf(reason, reason_size, "VP9 video session ready: codec=vp9 actual=%ux%u format=%d mem=%llu", session->video.key.max_coded_extent.width,
                      session->video.key.max_coded_extent.height, session->video.key.picture_format, static_cast<unsigned long long>(session->video.memory_bytes));
        return VA_STATUS_SUCCESS;
    }

    destroy_vp9_video_session(runtime, session);

    VideoProfileChain      profile_chain(session->profile_spec);
    VideoCapabilitiesChain capabilities(session->profile_spec);
    VkResult               result = runtime->get_video_capabilities(runtime->physical_device, &profile_chain.profile, &capabilities.video);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoCapabilitiesKHR(VP9) failed: %d", result);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    session->bitstream_offset_alignment = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferOffsetAlignment);
    session->bitstream_size_alignment   = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferSizeAlignment);
    session->decode_flags               = capabilities.decode.flags;
    session->max_level                  = capabilities.vp9.maxLevel;
    if (extent.width > capabilities.video.maxCodedExtent.width || extent.height > capabilities.video.maxCodedExtent.height) {
        std::snprintf(reason, reason_size, "VP9 coded extent %ux%u exceeds Vulkan limit %ux%u", extent.width, extent.height, capabilities.video.maxCodedExtent.width,
                      capabilities.video.maxCodedExtent.height);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }

    DecodeFormatSelection format_selection{};
    if (!choose_decode_format(runtime, &profile_chain.profile, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                              preferred_vk_format_for_rt_format(session->va_rt_format), runtime->surface_export, &format_selection, reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    const VkExtent2D            session_extent = vp9_session_extent(extent, capabilities.video);

    VkVideoSessionCreateInfoKHR session_info{};
    session_info.sType                      = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_info.queueFamilyIndex           = runtime->decode_queue_family;
    session_info.pVideoProfile              = &profile_chain.profile;
    session_info.pictureFormat              = format_selection.format;
    session_info.maxCodedExtent             = session_extent;
    session_info.referencePictureFormat     = format_selection.format;
    session_info.maxDpbSlots                = std::min<uint32_t>(capabilities.video.maxDpbSlots, max_vp9_dpb_slots);
    session_info.maxActiveReferencePictures = std::min<uint32_t>(capabilities.video.maxActiveReferencePictures, max_vp9_active_references);
    session_info.pStdHeaderVersion          = &capabilities.video.stdHeaderVersion;

    result = runtime->create_video_session(runtime->device, &session_info, nullptr, &session->video.session);
    if (!record_vk_result(runtime, result, "vkCreateVideoSessionKHR", "VP9 session", reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->video.key = {
        .codec_operation          = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
        .codec_profile            = static_cast<uint32_t>(profile_chain.vp9.stdProfile),
        .picture_format           = format_selection.format,
        .reference_picture_format = format_selection.format,
        .max_coded_extent         = session_extent,
        .image_usage              = vp9_surface_image_usage(),
        .image_create_flags       = format_selection.create_flags,
        .image_tiling             = format_selection.tiling,
        .chroma_subsampling       = profile_chain.profile.chromaSubsampling,
        .luma_bit_depth           = profile_chain.profile.lumaBitDepth,
        .chroma_bit_depth         = profile_chain.profile.chromaBitDepth,
    };

    if (!bind_video_session_memory(runtime, &session->video, reason, reason_size)) {
        destroy_vp9_video_session(runtime, session);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->max_dpb_slots                 = session_info.maxDpbSlots;
    session->max_active_reference_pictures = session_info.maxActiveReferencePictures;
    std::snprintf(reason, reason_size, "VP9 video session ready: codec=vp9 requested=%ux%u actual=%ux%u format=%d dpb=%u refs=%u mem=%llu", extent.width, extent.height,
                  session_extent.width, session_extent.height, format_selection.format, session_info.maxDpbSlots, session_info.maxActiveReferencePictures,
                  static_cast<unsigned long long>(session->video.memory_bytes));
    return VA_STATUS_SUCCESS;
}
