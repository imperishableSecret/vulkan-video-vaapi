#include "internal.h"
#include "api.h"
#include "vulkan/formats.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <new>

namespace vkvv {

    HEVCVideoSession* create_hevc_session(VAProfile va_profile, unsigned int va_rt_format, unsigned int va_fourcc, uint8_t bit_depth, VideoProfileSpec profile_spec) {
        auto* session         = new HEVCVideoSession();
        session->va_profile   = va_profile;
        session->va_rt_format = va_rt_format;
        session->va_fourcc    = va_fourcc;
        session->bit_depth    = bit_depth;
        session->profile_spec = profile_spec;
        return session;
    }

    VkImageUsageFlags hevc_surface_image_usage() {
        return VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    void destroy_hevc_video_session(VulkanRuntime* runtime, HEVCVideoSession* session) {
        if (session == nullptr) {
            return;
        }
        for (UploadBuffer& upload : session->uploads) {
            destroy_upload_buffer(runtime, &upload);
        }
        destroy_video_session(runtime, &session->video);
        session->surface_slots.clear();
        session->bitstream_offset_alignment    = 1;
        session->bitstream_size_alignment      = 1;
        session->max_level                     = STD_VIDEO_H265_LEVEL_IDC_6_2;
        session->decode_flags                  = 0;
        session->next_dpb_slot                 = 0;
        session->max_dpb_slots                 = 0;
        session->max_active_reference_pictures = 0;
    }

    bool reset_hevc_session(VulkanRuntime* runtime, HEVCVideoSession* session, VkVideoSessionParametersKHR parameters, char* reason, size_t reason_size) {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "HEVC session reset", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "HEVC session reset", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "HEVC session reset", reason, reason_size)) {
            return false;
        }

        VkVideoBeginCodingInfoKHR video_begin{};
        video_begin.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
        video_begin.videoSession           = session->video.session;
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
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "HEVC session reset", reason, reason_size)) {
            return false;
        }

        if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "HEVC session reset")) {
            return false;
        }

        session->video.initialized = true;
        return true;
    }

    VkExtent2D hevc_session_extent(VkExtent2D requested, const VkVideoCapabilitiesKHR& capabilities) {
        return {
            std::min(std::max(requested.width, capabilities.minCodedExtent.width), capabilities.maxCodedExtent.width),
            std::min(std::max(requested.height, capabilities.minCodedExtent.height), capabilities.maxCodedExtent.height),
        };
    }

} // namespace vkvv

using namespace vkvv;

void* vkvv_vulkan_hevc_session_create(void) {
    try {
        return create_hevc_session(VAProfileHEVCMain, VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, 8, hevc_main_profile_spec);
    } catch (const std::bad_alloc&) { return nullptr; }
}

void* vkvv_vulkan_hevc_main10_session_create(void) {
    try {
        return create_hevc_session(VAProfileHEVCMain10, VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010, 10, hevc_main10_profile_spec);
    } catch (const std::bad_alloc&) { return nullptr; }
}

void* vkvv_vulkan_hevc_session_create_for_config(const VkvvConfig* config) {
    if (config != nullptr && config->profile == VAProfileHEVCMain10) {
        return vkvv_vulkan_hevc_main10_session_create();
    }
    return vkvv_vulkan_hevc_session_create();
}

void vkvv_vulkan_hevc_session_destroy(void* runtime_ptr, void* session_ptr) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<HEVCVideoSession*>(session_ptr);
    destroy_hevc_video_session(runtime, session);
    delete session;
}

VAStatus vkvv_vulkan_ensure_hevc_session(void* runtime_ptr, void* session_ptr, unsigned int width, unsigned int height, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<HEVCVideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan HEVC session state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    const VkExtent2D extent = {
        .width  = round_up_16(width),
        .height = round_up_16(height),
    };
    if (session->video.session != VK_NULL_HANDLE && session->video.key.max_coded_extent.width >= extent.width && session->video.key.max_coded_extent.height >= extent.height) {
        std::snprintf(reason, reason_size, "HEVC video session ready: codec=hevc actual=%ux%u format=%d tiling=%s mem=%llu inline_params=%u",
                      session->video.key.max_coded_extent.width, session->video.key.max_coded_extent.height, session->video.key.picture_format,
                      decode_image_tiling_name(session->video.key.image_tiling), static_cast<unsigned long long>(session->video.memory_bytes), runtime->video_maintenance2);
        return VA_STATUS_SUCCESS;
    }

    destroy_hevc_video_session(runtime, session);

    VideoProfileChain      profile_chain(session->profile_spec);
    VideoCapabilitiesChain capabilities(session->profile_spec);
    VkResult               result = runtime->get_video_capabilities(runtime->physical_device, &profile_chain.profile, &capabilities.video);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoCapabilitiesKHR(HEVC) failed: %d", result);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    session->bitstream_offset_alignment = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferOffsetAlignment);
    session->bitstream_size_alignment   = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferSizeAlignment);
    session->decode_flags               = capabilities.decode.flags;
    session->max_level                  = capabilities.h265.maxLevelIdc;
    if (extent.width > capabilities.video.maxCodedExtent.width || extent.height > capabilities.video.maxCodedExtent.height) {
        std::snprintf(reason, reason_size, "HEVC coded extent %ux%u exceeds Vulkan limit %ux%u", extent.width, extent.height, capabilities.video.maxCodedExtent.width,
                      capabilities.video.maxCodedExtent.height);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }

    DecodeFormatSelection format_selection{};
    if (!choose_decode_format(runtime, &profile_chain.profile, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                              preferred_vk_format_for_rt_format(session->va_rt_format), runtime->surface_export, &format_selection, reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    const VkExtent2D            session_extent = hevc_session_extent(extent, capabilities.video);

    VkVideoSessionCreateInfoKHR session_info{};
    session_info.sType                      = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_info.queueFamilyIndex           = runtime->decode_queue_family;
    session_info.pVideoProfile              = &profile_chain.profile;
    session_info.pictureFormat              = format_selection.format;
    session_info.maxCodedExtent             = session_extent;
    session_info.referencePictureFormat     = format_selection.format;
    session_info.maxDpbSlots                = std::min<uint32_t>(capabilities.video.maxDpbSlots, max_hevc_dpb_slots);
    session_info.maxActiveReferencePictures = std::min<uint32_t>(capabilities.video.maxActiveReferencePictures, max_va_hevc_reference_frames);
    session_info.pStdHeaderVersion          = &capabilities.video.stdHeaderVersion;
    if (runtime->video_maintenance2) {
        session_info.flags |= VK_VIDEO_SESSION_CREATE_INLINE_SESSION_PARAMETERS_BIT_KHR;
    }

    result = runtime->create_video_session(runtime->device, &session_info, nullptr, &session->video.session);
    if (!record_vk_result(runtime, result, "vkCreateVideoSessionKHR", "HEVC session", reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->video.key = {
        .codec_operation          = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        .codec_profile            = static_cast<uint32_t>(profile_chain.h265.stdProfileIdc),
        .picture_format           = format_selection.format,
        .reference_picture_format = format_selection.format,
        .max_coded_extent         = session_extent,
        .image_usage              = hevc_surface_image_usage(),
        .image_create_flags       = format_selection.create_flags,
        .image_tiling             = format_selection.tiling,
        .chroma_subsampling       = profile_chain.profile.chromaSubsampling,
        .luma_bit_depth           = profile_chain.profile.lumaBitDepth,
        .chroma_bit_depth         = profile_chain.profile.chromaBitDepth,
    };

    if (!bind_video_session_memory(runtime, &session->video, reason, reason_size)) {
        destroy_hevc_video_session(runtime, session);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->max_dpb_slots                 = session_info.maxDpbSlots;
    session->max_active_reference_pictures = session_info.maxActiveReferencePictures;
    std::snprintf(
        reason, reason_size,
        "HEVC video session ready: codec=hevc requested=%ux%u actual=%ux%u format=%d tiling=%s direct_export=%u export_tiling=%u preferred_export_tiling=%u dpb=%u refs=%u "
        "mem=%llu inline_params=%u",
        extent.width, extent.height, session_extent.width, session_extent.height, format_selection.format, decode_image_tiling_name(format_selection.tiling),
        format_selection.direct_export_candidate ? 1U : 0U, format_selection.export_tiling_candidate_count, format_selection.preferred_export_tiling_candidate_count,
        session_info.maxDpbSlots, session_info.maxActiveReferencePictures, static_cast<unsigned long long>(session->video.memory_bytes), runtime->video_maintenance2);
    return VA_STATUS_SUCCESS;
}
