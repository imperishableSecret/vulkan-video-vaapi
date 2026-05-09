#include "internal.h"
#include "api.h"

#include <algorithm>
#include <cstdio>
#include <new>

namespace vkvv {

    VkImageUsageFlags h264_encode_input_image_usage() {
        return VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    void destroy_h264_encode_session(VulkanRuntime* runtime, H264EncodeSession* session) {
        if (session == nullptr) {
            return;
        }
        if (runtime != nullptr && session->parameters != VK_NULL_HANDLE && runtime->destroy_video_session_parameters != nullptr) {
            runtime->destroy_video_session_parameters(runtime->device, session->parameters, nullptr);
        }
        session->parameters = VK_NULL_HANDLE;
        destroy_video_session(runtime, &session->video);
        session->max_level                     = STD_VIDEO_H264_LEVEL_IDC_5_2;
        session->encode_flags                  = 0;
        session->rate_control_modes            = 0;
        session->max_quality_levels            = 0;
        session->quality_level                 = 0;
        session->max_dpb_slots                 = 0;
        session->max_active_reference_pictures = 0;
    }

    uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
        if (alignment <= 1) {
            return value;
        }
        return ((value + alignment - 1u) / alignment) * alignment;
    }

    VkExtent2D h264_encode_session_extent(VkExtent2D requested, const VkVideoCapabilitiesKHR& video_caps, const VkVideoEncodeCapabilitiesKHR& encode_caps) {
        const uint32_t granularity_width  = std::max<uint32_t>(1, encode_caps.encodeInputPictureGranularity.width);
        const uint32_t granularity_height = std::max<uint32_t>(1, encode_caps.encodeInputPictureGranularity.height);
        return {
            std::min(std::max(align_up_u32(requested.width, granularity_width), video_caps.minCodedExtent.width), video_caps.maxCodedExtent.width),
            std::min(std::max(align_up_u32(requested.height, granularity_height), video_caps.minCodedExtent.height), video_caps.maxCodedExtent.height),
        };
    }

    StdVideoH264PictureType h264_encode_picture_type(const VkvvH264EncodeInput* input) {
        if (input == nullptr) {
            return STD_VIDEO_H264_PICTURE_TYPE_P;
        }
        switch (input->frame_type) {
            case VKVV_H264_ENCODE_FRAME_IDR: return STD_VIDEO_H264_PICTURE_TYPE_IDR;
            case VKVV_H264_ENCODE_FRAME_I: return STD_VIDEO_H264_PICTURE_TYPE_I;
            case VKVV_H264_ENCODE_FRAME_B: return STD_VIDEO_H264_PICTURE_TYPE_B;
            case VKVV_H264_ENCODE_FRAME_P:
            default: return STD_VIDEO_H264_PICTURE_TYPE_P;
        }
    }

    StdVideoH264SliceType h264_encode_slice_type(const VAEncSliceParameterBufferH264& slice) {
        switch (slice.slice_type % 5) {
            case 2: return STD_VIDEO_H264_SLICE_TYPE_I;
            case 1: return STD_VIDEO_H264_SLICE_TYPE_B;
            case 0:
            default: return STD_VIDEO_H264_SLICE_TYPE_P;
        }
    }

    void build_h264_encode_std_parameters(const H264EncodeSession* session, const VkvvH264EncodeInput* input, H264StdParameters* std_params) {
        const VAEncSequenceParameterBufferH264* seq = input->sequence;
        const VAEncPictureParameterBufferH264*  pic = input->picture;

        *std_params                                = {};
        StdVideoH264SequenceParameterSet& sps      = std_params->sps;
        sps.flags.direct_8x8_inference_flag        = seq->seq_fields.bits.direct_8x8_inference_flag;
        sps.flags.mb_adaptive_frame_field_flag     = seq->seq_fields.bits.mb_adaptive_frame_field_flag;
        sps.flags.frame_mbs_only_flag              = seq->seq_fields.bits.frame_mbs_only_flag;
        sps.flags.delta_pic_order_always_zero_flag = seq->seq_fields.bits.delta_pic_order_always_zero_flag;
        sps.flags.frame_cropping_flag              = seq->frame_cropping_flag;
        sps.flags.seq_scaling_matrix_present_flag  = seq->seq_fields.bits.seq_scaling_matrix_present_flag && input->iq != nullptr;
        sps.flags.vui_parameters_present_flag      = seq->vui_parameters_present_flag;
        sps.profile_idc                            = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        sps.level_idc                              = seq->level_idc != 0 ? clamp_h264_level(static_cast<StdVideoH264LevelIdc>(seq->level_idc), session->max_level) :
                                                                           derive_h264_level_idc(seq->picture_width_in_mbs, seq->picture_height_in_mbs, session->max_level);
        sps.chroma_format_idc                      = std_chroma_from_va(seq->seq_fields.bits.chroma_format_idc);
        sps.seq_parameter_set_id                   = seq->seq_parameter_set_id;
        sps.bit_depth_luma_minus8                  = seq->bit_depth_luma_minus8;
        sps.bit_depth_chroma_minus8                = seq->bit_depth_chroma_minus8;
        sps.log2_max_frame_num_minus4              = seq->seq_fields.bits.log2_max_frame_num_minus4;
        sps.pic_order_cnt_type                     = std_poc_type_from_va(seq->seq_fields.bits.pic_order_cnt_type);
        sps.offset_for_non_ref_pic                 = seq->offset_for_non_ref_pic;
        sps.offset_for_top_to_bottom_field         = seq->offset_for_top_to_bottom_field;
        sps.log2_max_pic_order_cnt_lsb_minus4      = seq->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
        sps.num_ref_frames_in_pic_order_cnt_cycle  = seq->num_ref_frames_in_pic_order_cnt_cycle;
        sps.max_num_ref_frames                     = static_cast<uint8_t>(std::min<uint32_t>(seq->max_num_ref_frames, 16));
        sps.pic_width_in_mbs_minus1                = seq->picture_width_in_mbs > 0 ? seq->picture_width_in_mbs - 1u : 0;
        sps.pic_height_in_map_units_minus1         = seq->picture_height_in_mbs > 0 ? seq->picture_height_in_mbs - 1u : 0;
        sps.frame_crop_left_offset                 = seq->frame_crop_left_offset;
        sps.frame_crop_right_offset                = seq->frame_crop_right_offset;
        sps.frame_crop_top_offset                  = seq->frame_crop_top_offset;
        sps.frame_crop_bottom_offset               = seq->frame_crop_bottom_offset;
        sps.pOffsetForRefFrame                     = seq->offset_for_ref_frame;

        if (input->iq != nullptr) {
            for (uint32_t i = 0; i < 6; i++) {
                std::copy(std::begin(input->iq->ScalingList4x4[i]), std::end(input->iq->ScalingList4x4[i]), std::begin(std_params->sps_scaling.ScalingList4x4[i]));
                std::copy(std::begin(input->iq->ScalingList4x4[i]), std::end(input->iq->ScalingList4x4[i]), std::begin(std_params->pps_scaling.ScalingList4x4[i]));
            }
            for (uint32_t i = 0; i < 2; i++) {
                std::copy(std::begin(input->iq->ScalingList8x8[i]), std::end(input->iq->ScalingList8x8[i]), std::begin(std_params->sps_scaling.ScalingList8x8[i]));
                std::copy(std::begin(input->iq->ScalingList8x8[i]), std::end(input->iq->ScalingList8x8[i]), std::begin(std_params->pps_scaling.ScalingList8x8[i]));
            }
            sps.pScalingLists = &std_params->sps_scaling;
        }

        if (seq->vui_parameters_present_flag) {
            StdVideoH264SequenceParameterSetVui& vui = std_params->vui;
            vui.flags.aspect_ratio_info_present_flag = seq->vui_fields.bits.aspect_ratio_info_present_flag;
            vui.flags.timing_info_present_flag       = seq->vui_fields.bits.timing_info_present_flag;
            vui.flags.bitstream_restriction_flag     = seq->vui_fields.bits.bitstream_restriction_flag;
            vui.flags.fixed_frame_rate_flag          = seq->vui_fields.bits.fixed_frame_rate_flag;
            vui.aspect_ratio_idc                     = static_cast<StdVideoH264AspectRatioIdc>(seq->aspect_ratio_idc);
            vui.sar_width                            = static_cast<uint16_t>(std::min<uint32_t>(seq->sar_width, UINT16_MAX));
            vui.sar_height                           = static_cast<uint16_t>(std::min<uint32_t>(seq->sar_height, UINT16_MAX));
            vui.num_units_in_tick                    = seq->num_units_in_tick;
            vui.time_scale                           = seq->time_scale;
            vui.max_num_reorder_frames               = 0;
            vui.max_dec_frame_buffering              = static_cast<uint8_t>(std::min<uint32_t>(seq->max_num_ref_frames, 16));
            sps.pSequenceParameterSetVui             = &vui;
        }

        StdVideoH264PictureParameterSet& pps                   = std_params->pps;
        pps.flags.transform_8x8_mode_flag                      = pic->pic_fields.bits.transform_8x8_mode_flag;
        pps.flags.redundant_pic_cnt_present_flag               = pic->pic_fields.bits.redundant_pic_cnt_present_flag;
        pps.flags.constrained_intra_pred_flag                  = pic->pic_fields.bits.constrained_intra_pred_flag;
        pps.flags.deblocking_filter_control_present_flag       = pic->pic_fields.bits.deblocking_filter_control_present_flag;
        pps.flags.weighted_pred_flag                           = pic->pic_fields.bits.weighted_pred_flag;
        pps.flags.bottom_field_pic_order_in_frame_present_flag = pic->pic_fields.bits.pic_order_present_flag;
        pps.flags.entropy_coding_mode_flag                     = pic->pic_fields.bits.entropy_coding_mode_flag;
        pps.flags.pic_scaling_matrix_present_flag              = pic->pic_fields.bits.pic_scaling_matrix_present_flag && input->iq != nullptr;
        pps.seq_parameter_set_id                               = pic->seq_parameter_set_id;
        pps.pic_parameter_set_id                               = pic->pic_parameter_set_id;
        pps.num_ref_idx_l0_default_active_minus1               = std::min<uint8_t>(pic->num_ref_idx_l0_active_minus1, 31);
        pps.num_ref_idx_l1_default_active_minus1               = std::min<uint8_t>(pic->num_ref_idx_l1_active_minus1, 31);
        pps.weighted_bipred_idc                                = static_cast<StdVideoH264WeightedBipredIdc>(pic->pic_fields.bits.weighted_bipred_idc);
        pps.pic_init_qp_minus26                                = static_cast<int8_t>(static_cast<int>(pic->pic_init_qp) - 26);
        pps.chroma_qp_index_offset                             = pic->chroma_qp_index_offset;
        pps.second_chroma_qp_index_offset                      = pic->second_chroma_qp_index_offset;
        if (pps.flags.pic_scaling_matrix_present_flag) {
            pps.pScalingLists = &std_params->pps_scaling;
        }
    }

    bool create_h264_encode_session_parameters(VulkanRuntime* runtime, H264EncodeSession* session, const H264StdParameters* std_params, char* reason, size_t reason_size) {
        if (session->parameters != VK_NULL_HANDLE) {
            runtime->destroy_video_session_parameters(runtime->device, session->parameters, nullptr);
            session->parameters = VK_NULL_HANDLE;
        }

        VkVideoEncodeH264SessionParametersAddInfoKHR add_info{};
        add_info.sType       = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
        add_info.stdSPSCount = 1;
        add_info.pStdSPSs    = &std_params->sps;
        add_info.stdPPSCount = 1;
        add_info.pStdPPSs    = &std_params->pps;

        VkVideoEncodeH264SessionParametersCreateInfoKHR h264_info{};
        h264_info.sType              = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
        h264_info.maxStdSPSCount     = 1;
        h264_info.maxStdPPSCount     = 1;
        h264_info.pParametersAddInfo = &add_info;

        VkVideoSessionParametersCreateInfoKHR create_info{};
        create_info.sType        = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        create_info.pNext        = &h264_info;
        create_info.videoSession = session->video.session;

        VkResult result = runtime->create_video_session_parameters(runtime->device, &create_info, nullptr, &session->parameters);
        return record_vk_result(runtime, result, "vkCreateVideoSessionParametersKHR", "H.264 encode parameters", reason, reason_size);
    }

} // namespace vkvv

using namespace vkvv;

void* vkvv_vulkan_h264_encode_session_create(void) {
    try {
        return new H264EncodeSession();
    } catch (const std::bad_alloc&) { return nullptr; }
}

void vkvv_vulkan_h264_encode_session_destroy(void* runtime_ptr, void* session_ptr) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<H264EncodeSession*>(session_ptr);
    destroy_h264_encode_session(runtime, session);
    delete session;
}

VAStatus vkvv_vulkan_ensure_h264_encode_session(void* runtime_ptr, void* session_ptr, const VkvvH264EncodeInput* input, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<H264EncodeSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr || input == nullptr || input->sequence == nullptr || input->picture == nullptr) {
        std::snprintf(reason, reason_size, "missing Vulkan H.264 encode session state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if ((runtime->enabled_encode_operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == 0 || runtime->encode_queue_family == invalid_queue_family) {
        std::snprintf(reason, reason_size, "Vulkan H.264 encode operation is not enabled");
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    const VkExtent2D requested_extent{
        .width  = round_up_16(input->width),
        .height = round_up_16(input->height),
    };

    VideoProfileChain      profile_chain(h264_encode_profile_spec);
    VideoCapabilitiesChain capabilities(h264_encode_profile_spec);
    VkResult               result = runtime->get_video_capabilities(runtime->physical_device, &profile_chain.profile, &capabilities.video);
    if (!record_vk_result(runtime, result, "vkGetPhysicalDeviceVideoCapabilitiesKHR", "H.264 encode capabilities", reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (input->slice_count == 0 || input->slice_count > capabilities.h264_encode.maxSliceCount) {
        std::snprintf(reason, reason_size, "H.264 encode slice count %u exceeds Vulkan limit %u", input->slice_count, capabilities.h264_encode.maxSliceCount);
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (requested_extent.width > capabilities.video.maxCodedExtent.width || requested_extent.height > capabilities.video.maxCodedExtent.height) {
        std::snprintf(reason, reason_size, "H.264 encode extent %ux%u exceeds Vulkan limit %ux%u", requested_extent.width, requested_extent.height,
                      capabilities.video.maxCodedExtent.width, capabilities.video.maxCodedExtent.height);
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }

    const VkExtent2D session_extent = h264_encode_session_extent(requested_extent, capabilities.video, capabilities.encode);
    if (session->video.session == VK_NULL_HANDLE || session->video.key.max_coded_extent.width < session_extent.width ||
        session->video.key.max_coded_extent.height < session_extent.height) {
        destroy_h264_encode_session(runtime, session);

        VkVideoEncodeH264SessionCreateInfoKHR h264_info{};
        h264_info.sType          = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_CREATE_INFO_KHR;
        h264_info.useMaxLevelIdc = VK_TRUE;
        h264_info.maxLevelIdc    = capabilities.h264_encode.maxLevelIdc;

        VkVideoEncodeQualityLevelInfoKHR quality{};
        void*                            session_pnext = &h264_info;
        if (capabilities.encode.maxQualityLevels > 0) {
            quality.sType        = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
            quality.pNext        = session_pnext;
            quality.qualityLevel = 0;
            session_pnext        = &quality;
        }

        VkVideoSessionCreateInfoKHR session_info{};
        session_info.sType                      = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
        session_info.pNext                      = session_pnext;
        session_info.queueFamilyIndex           = runtime->encode_queue_family;
        session_info.pVideoProfile              = &profile_chain.profile;
        session_info.pictureFormat              = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        session_info.maxCodedExtent             = session_extent;
        session_info.referencePictureFormat     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        session_info.maxDpbSlots                = std::min<uint32_t>(std::max<uint32_t>(1, capabilities.video.maxDpbSlots), 2);
        session_info.maxActiveReferencePictures = std::min<uint32_t>(std::max<uint32_t>(1, capabilities.video.maxActiveReferencePictures), 1);
        session_info.pStdHeaderVersion          = &capabilities.video.stdHeaderVersion;

        result = runtime->create_video_session(runtime->device, &session_info, nullptr, &session->video.session);
        if (!record_vk_result(runtime, result, "vkCreateVideoSessionKHR", "H.264 encode session", reason, reason_size)) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        session->video.key = {
            .codec_operation          = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
            .codec_profile            = static_cast<uint32_t>(profile_chain.h264_encode.stdProfileIdc),
            .picture_format           = session_info.pictureFormat,
            .reference_picture_format = session_info.referencePictureFormat,
            .max_coded_extent         = session_extent,
            .image_usage              = h264_encode_input_image_usage(),
            .image_create_flags       = 0,
            .image_tiling             = VK_IMAGE_TILING_OPTIMAL,
            .chroma_subsampling       = profile_chain.profile.chromaSubsampling,
            .luma_bit_depth           = profile_chain.profile.lumaBitDepth,
            .chroma_bit_depth         = profile_chain.profile.chromaBitDepth,
        };

        if (!bind_video_session_memory(runtime, &session->video, reason, reason_size)) {
            destroy_h264_encode_session(runtime, session);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        session->max_dpb_slots                 = session_info.maxDpbSlots;
        session->max_active_reference_pictures = session_info.maxActiveReferencePictures;
        session->max_level                     = capabilities.h264_encode.maxLevelIdc;
        session->encode_flags                  = capabilities.h264_encode.flags;
        session->rate_control_modes            = capabilities.encode.rateControlModes;
        session->max_quality_levels            = capabilities.encode.maxQualityLevels;
        session->quality_level                 = 0;
    }

    H264StdParameters std_params{};
    build_h264_encode_std_parameters(session, input, &std_params);
    if (!create_h264_encode_session_parameters(runtime, session, &std_params, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    std::snprintf(reason, reason_size, "H.264 encode session ready: requested=%ux%u actual=%ux%u format=%d dpb=%u refs=%u mem=%llu params=1 slices=%u rc_modes=0x%x",
                  requested_extent.width, requested_extent.height, session->video.key.max_coded_extent.width, session->video.key.max_coded_extent.height,
                  session->video.key.picture_format, session->max_dpb_slots, session->max_active_reference_pictures, static_cast<unsigned long long>(session->video.memory_bytes),
                  input->slice_count, session->rate_control_modes);
    return VA_STATUS_SUCCESS;
}
