#include "internal.h"
#include "api.h"
#include "vulkan/formats.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <new>

namespace vkvv {

    namespace {

        uint8_t bits_needed_minus1(uint32_t value) {
            uint8_t bits = 0;
            do {
                bits++;
                value >>= 1;
            } while (value != 0);
            return bits > 0 ? static_cast<uint8_t>(bits - 1) : 0;
        }

        StdVideoAV1MatrixCoefficients av1_matrix_coefficients(uint8_t matrix_coefficients) {
            if (matrix_coefficients <= STD_VIDEO_AV1_MATRIX_COEFFICIENTS_ICTCP) {
                return static_cast<StdVideoAV1MatrixCoefficients>(matrix_coefficients);
            }
            return STD_VIDEO_AV1_MATRIX_COEFFICIENTS_UNSPECIFIED;
        }

        bool av1_uses_restoration(const VADecPictureParameterBufferAV1* pic) {
            return pic != nullptr &&
                (pic->loop_restoration_fields.bits.yframe_restoration_type != 0 || pic->loop_restoration_fields.bits.cbframe_restoration_type != 0 ||
                 pic->loop_restoration_fields.bits.crframe_restoration_type != 0);
        }

        VkExtent2D av1_session_extent(VkExtent2D requested, const VkVideoCapabilitiesKHR& capabilities) {
            return {
                std::min(std::max(requested.width, capabilities.minCodedExtent.width), capabilities.maxCodedExtent.width),
                std::min(std::max(requested.height, capabilities.minCodedExtent.height), capabilities.maxCodedExtent.height),
            };
        }

    } // namespace

    AV1VideoSession* create_av1_session(VAProfile va_profile, unsigned int va_rt_format, unsigned int va_fourcc, uint8_t bitstream_profile, uint8_t bit_depth,
                                        VideoProfileSpec profile_spec) {
        auto* session              = new AV1VideoSession();
        session->va_profile        = va_profile;
        session->va_rt_format      = va_rt_format;
        session->va_fourcc         = va_fourcc;
        session->bitstream_profile = bitstream_profile;
        session->bit_depth         = bit_depth;
        session->profile_spec      = profile_spec;
        return session;
    }

    VkImageUsageFlags av1_surface_image_usage() {
        return VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    void destroy_av1_video_session(VulkanRuntime* runtime, AV1VideoSession* session) {
        if (session == nullptr) {
            return;
        }
        destroy_upload_buffer(runtime, &session->upload);
        destroy_video_session(runtime, &session->video);
        for (AV1ReferenceSlot& slot : session->reference_slots) {
            slot = {};
        }
        session->surface_slots.clear();
        session->bitstream_offset_alignment    = 1;
        session->bitstream_size_alignment      = 1;
        session->max_level                     = STD_VIDEO_AV1_LEVEL_6_2;
        session->decode_flags                  = 0;
        session->next_dpb_slot                 = 0;
        session->max_dpb_slots                 = 0;
        session->max_active_reference_pictures = 0;
    }

    bool reset_av1_session(VulkanRuntime* runtime, AV1VideoSession* session, VkVideoSessionParametersKHR parameters, char* reason, size_t reason_size) {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "AV1 session reset", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "AV1 session reset", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "AV1 session reset", reason, reason_size)) {
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
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "AV1 session reset", reason, reason_size)) {
            return false;
        }

        if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "AV1 session reset")) {
            return false;
        }

        session->video.initialized = true;
        return true;
    }

    void build_av1_session_parameters(const VkvvAV1DecodeInput* input, AV1SessionStdParameters* std_params) {
        if (input == nullptr || input->pic == nullptr || std_params == nullptr) {
            return;
        }
        const VADecPictureParameterBufferAV1* pic = input->pic;

        std_params->color                          = {};
        std_params->color.flags.mono_chrome        = pic->seq_info_fields.fields.mono_chrome;
        std_params->color.flags.color_range        = pic->seq_info_fields.fields.color_range;
        std_params->color.BitDepth                 = input->bit_depth;
        std_params->color.subsampling_x            = pic->seq_info_fields.fields.subsampling_x;
        std_params->color.subsampling_y            = pic->seq_info_fields.fields.subsampling_y;
        std_params->color.color_primaries          = STD_VIDEO_AV1_COLOR_PRIMARIES_UNSPECIFIED;
        std_params->color.transfer_characteristics = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
        std_params->color.matrix_coefficients      = av1_matrix_coefficients(pic->matrix_coefficients);
        std_params->color.chroma_sample_position   = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN;

        std_params->sequence                                  = {};
        std_params->sequence.flags.still_picture              = pic->seq_info_fields.fields.still_picture;
        std_params->sequence.flags.use_128x128_superblock     = pic->seq_info_fields.fields.use_128x128_superblock;
        std_params->sequence.flags.enable_filter_intra        = pic->seq_info_fields.fields.enable_filter_intra;
        std_params->sequence.flags.enable_intra_edge_filter   = pic->seq_info_fields.fields.enable_intra_edge_filter;
        std_params->sequence.flags.enable_interintra_compound = pic->seq_info_fields.fields.enable_interintra_compound;
        std_params->sequence.flags.enable_masked_compound     = pic->seq_info_fields.fields.enable_masked_compound;
        std_params->sequence.flags.enable_warped_motion       = pic->pic_info_fields.bits.allow_warped_motion;
        std_params->sequence.flags.enable_dual_filter         = pic->seq_info_fields.fields.enable_dual_filter;
        std_params->sequence.flags.enable_order_hint          = pic->seq_info_fields.fields.enable_order_hint;
        std_params->sequence.flags.enable_jnt_comp            = pic->seq_info_fields.fields.enable_jnt_comp;
        std_params->sequence.flags.enable_ref_frame_mvs       = pic->pic_info_fields.bits.use_ref_frame_mvs;
        std_params->sequence.flags.enable_superres            = pic->pic_info_fields.bits.use_superres;
        std_params->sequence.flags.enable_cdef                = pic->seq_info_fields.fields.enable_cdef;
        std_params->sequence.flags.enable_restoration         = av1_uses_restoration(pic);
        std_params->sequence.flags.film_grain_params_present  = pic->seq_info_fields.fields.film_grain_params_present;
        std_params->sequence.seq_profile                      = STD_VIDEO_AV1_PROFILE_MAIN;
        std_params->sequence.frame_width_bits_minus_1         = bits_needed_minus1(pic->frame_width_minus1);
        std_params->sequence.frame_height_bits_minus_1        = bits_needed_minus1(pic->frame_height_minus1);
        std_params->sequence.max_frame_width_minus_1          = pic->frame_width_minus1;
        std_params->sequence.max_frame_height_minus_1         = pic->frame_height_minus1;
        std_params->sequence.order_hint_bits_minus_1          = pic->order_hint_bits_minus_1;
        std_params->sequence.seq_force_integer_mv             = STD_VIDEO_AV1_SELECT_INTEGER_MV;
        std_params->sequence.seq_force_screen_content_tools   = STD_VIDEO_AV1_SELECT_SCREEN_CONTENT_TOOLS;
        std_params->sequence.pColorConfig                     = &std_params->color;
    }

    bool create_av1_session_parameters(VulkanRuntime* runtime, AV1VideoSession* session, const AV1SessionStdParameters* std_params, VkVideoSessionParametersKHR* parameters,
                                       char* reason, size_t reason_size) {
        VkVideoDecodeAV1SessionParametersCreateInfoKHR av1_info{};
        av1_info.sType              = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR;
        av1_info.pStdSequenceHeader = &std_params->sequence;

        VkVideoSessionParametersCreateInfoKHR create_info{};
        create_info.sType        = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        create_info.pNext        = &av1_info;
        create_info.videoSession = session->video.session;

        VkResult result = runtime->create_video_session_parameters(runtime->device, &create_info, nullptr, parameters);
        if (!record_vk_result(runtime, result, "vkCreateVideoSessionParametersKHR", "AV1 parameters", reason, reason_size)) {
            return false;
        }
        return true;
    }

} // namespace vkvv

using namespace vkvv;

void* vkvv_vulkan_av1_session_create(void) {
    try {
        return create_av1_session(VAProfileAV1Profile0, VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, 0, 8, av1_profile0_spec);
    } catch (const std::bad_alloc&) { return nullptr; }
}

void* vkvv_vulkan_av1_p010_session_create(void) {
    try {
        return create_av1_session(VAProfileAV1Profile0, VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010, 0, 10, av1_profile0_10bit_spec);
    } catch (const std::bad_alloc&) { return nullptr; }
}

void* vkvv_vulkan_av1_session_create_for_config(const VkvvConfig* config) {
    if (config != nullptr && (config->rt_format & VA_RT_FORMAT_YUV420_10) != 0) {
        return vkvv_vulkan_av1_p010_session_create();
    }
    return vkvv_vulkan_av1_session_create();
}

void vkvv_vulkan_av1_session_destroy(void* runtime_ptr, void* session_ptr) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<AV1VideoSession*>(session_ptr);
    destroy_av1_video_session(runtime, session);
    delete session;
}

VAStatus vkvv_vulkan_configure_av1_session(void* runtime_ptr, void* session_ptr, const VkvvSurface* target, const VkvvAV1DecodeInput* input, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<AV1VideoSession*>(session_ptr);
    if (session == nullptr || target == nullptr || input == nullptr || input->pic == nullptr) {
        std::snprintf(reason, reason_size, "missing AV1 session format selection state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (input->pic->profile != 0) {
        std::snprintf(reason, reason_size, "unsupported AV1 bitstream profile: %u", input->pic->profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (input->bit_depth != 8 && input->bit_depth != 10) {
        std::snprintf(reason, reason_size, "unsupported AV1 bit depth: %u", input->bit_depth);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    const bool         wants_p010         = input->bit_depth > 8;
    const unsigned int expected_rt_format = wants_p010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    const unsigned int expected_fourcc    = wants_p010 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    if (input->rt_format != expected_rt_format || input->fourcc != expected_fourcc) {
        std::snprintf(reason, reason_size, "AV1 parser/output format mismatch: rt=0x%x fourcc=0x%x expected_rt=0x%x expected_fourcc=0x%x", input->rt_format, input->fourcc,
                      expected_rt_format, expected_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (target->rt_format != expected_rt_format || target->fourcc != expected_fourcc) {
        std::snprintf(reason, reason_size, "AV1 target surface format mismatch before session creation: rt=0x%x fourcc=0x%x expected_rt=0x%x expected_fourcc=0x%x",
                      target->rt_format, target->fourcc, expected_rt_format, expected_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    const VideoProfileSpec expected_spec = wants_p010 ? av1_profile0_10bit_spec : av1_profile0_spec;
    if (session->va_rt_format == expected_rt_format && session->va_fourcc == expected_fourcc && session->bitstream_profile == input->pic->profile &&
        session->bit_depth == input->bit_depth && session->profile_spec.bit_depth == expected_spec.bit_depth) {
        std::snprintf(reason, reason_size, "AV1 session format ready: depth=%u fourcc=0x%x", session->bit_depth, session->va_fourcc);
        return VA_STATUS_SUCCESS;
    }

    if (runtime == nullptr && (session->video.session != VK_NULL_HANDLE || session->upload.buffer != VK_NULL_HANDLE || session->upload.memory != VK_NULL_HANDLE)) {
        std::snprintf(reason, reason_size, "missing Vulkan runtime for AV1 session retarget");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    const bool had_session = session->video.session != VK_NULL_HANDLE || session->upload.buffer != VK_NULL_HANDLE || session->upload.memory != VK_NULL_HANDLE ||
        !session->surface_slots.empty() || session->max_dpb_slots != 0;
    if (had_session) {
        destroy_av1_video_session(runtime, session);
    }

    session->va_profile        = VAProfileAV1Profile0;
    session->va_rt_format      = expected_rt_format;
    session->va_fourcc         = expected_fourcc;
    session->bitstream_profile = input->pic->profile;
    session->bit_depth         = input->bit_depth;
    session->profile_spec      = expected_spec;
    std::snprintf(reason, reason_size, "AV1 session format selected from bitstream: depth=%u fourcc=0x%x recreated=%u", session->bit_depth, session->va_fourcc,
                  had_session ? 1U : 0U);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_ensure_av1_session(void* runtime_ptr, void* session_ptr, unsigned int width, unsigned int height, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<AV1VideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan AV1 session state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    const VkExtent2D extent = {
        .width  = round_up_16(width),
        .height = round_up_16(height),
    };
    if (session->video.session != VK_NULL_HANDLE && session->video.key.max_coded_extent.width >= extent.width && session->video.key.max_coded_extent.height >= extent.height) {
        std::snprintf(reason, reason_size, "AV1 video session ready: codec=av1 actual=%ux%u format=%d mem=%llu", session->video.key.max_coded_extent.width,
                      session->video.key.max_coded_extent.height, session->video.key.picture_format, static_cast<unsigned long long>(session->video.memory_bytes));
        return VA_STATUS_SUCCESS;
    }

    destroy_av1_video_session(runtime, session);

    VideoProfileChain      profile_chain(session->profile_spec);
    VideoCapabilitiesChain capabilities(session->profile_spec);
    VkResult               result = runtime->get_video_capabilities(runtime->physical_device, &profile_chain.profile, &capabilities.video);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetPhysicalDeviceVideoCapabilitiesKHR(AV1) failed: %d", result);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    session->bitstream_offset_alignment = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferOffsetAlignment);
    session->bitstream_size_alignment   = std::max<VkDeviceSize>(1, capabilities.video.minBitstreamBufferSizeAlignment);
    session->decode_flags               = capabilities.decode.flags;
    session->max_level                  = capabilities.av1.maxLevel;
    if (extent.width > capabilities.video.maxCodedExtent.width || extent.height > capabilities.video.maxCodedExtent.height) {
        std::snprintf(reason, reason_size, "AV1 coded extent %ux%u exceeds Vulkan limit %ux%u", extent.width, extent.height, capabilities.video.maxCodedExtent.width,
                      capabilities.video.maxCodedExtent.height);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }

    DecodeFormatSelection format_selection{};
    if (!choose_decode_format(runtime, &profile_chain.profile, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
                              preferred_vk_format_for_rt_format(session->va_rt_format), runtime->surface_export, &format_selection, reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    const VkExtent2D            session_extent = av1_session_extent(extent, capabilities.video);

    VkVideoSessionCreateInfoKHR session_info{};
    session_info.sType                      = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_info.queueFamilyIndex           = runtime->decode_queue_family;
    session_info.pVideoProfile              = &profile_chain.profile;
    session_info.pictureFormat              = format_selection.format;
    session_info.maxCodedExtent             = session_extent;
    session_info.referencePictureFormat     = format_selection.format;
    session_info.maxDpbSlots                = std::min<uint32_t>(capabilities.video.maxDpbSlots, max_av1_dpb_slots);
    session_info.maxActiveReferencePictures = std::min<uint32_t>(capabilities.video.maxActiveReferencePictures, max_av1_active_references);
    session_info.pStdHeaderVersion          = &capabilities.video.stdHeaderVersion;

    result = runtime->create_video_session(runtime->device, &session_info, nullptr, &session->video.session);
    if (!record_vk_result(runtime, result, "vkCreateVideoSessionKHR", "AV1 session", reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->video.key = {
        .codec_operation          = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
        .codec_profile            = static_cast<uint32_t>(profile_chain.av1.stdProfile),
        .picture_format           = format_selection.format,
        .reference_picture_format = format_selection.format,
        .max_coded_extent         = session_extent,
        .image_usage              = av1_surface_image_usage(),
        .image_create_flags       = format_selection.create_flags,
        .image_tiling             = format_selection.tiling,
        .chroma_subsampling       = profile_chain.profile.chromaSubsampling,
        .luma_bit_depth           = profile_chain.profile.lumaBitDepth,
        .chroma_bit_depth         = profile_chain.profile.chromaBitDepth,
    };

    if (!bind_video_session_memory(runtime, &session->video, reason, reason_size)) {
        destroy_av1_video_session(runtime, session);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    session->max_dpb_slots                 = session_info.maxDpbSlots;
    session->max_active_reference_pictures = session_info.maxActiveReferencePictures;
    std::snprintf(reason, reason_size, "AV1 video session ready: codec=av1 requested=%ux%u actual=%ux%u format=%d dpb=%u refs=%u mem=%llu", extent.width, extent.height,
                  session_extent.width, session_extent.height, format_selection.format, session_info.maxDpbSlots, session_info.maxActiveReferencePictures,
                  static_cast<unsigned long long>(session->video.memory_bytes));
    return VA_STATUS_SUCCESS;
}
