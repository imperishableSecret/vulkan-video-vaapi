#include "internal.h"

#include <algorithm>
#include <cstdio>

namespace vkvv {

StdVideoH264ProfileIdc std_profile_from_va(VAProfile profile) {
    switch (profile) {
        case VAProfileH264ConstrainedBaseline:
            return STD_VIDEO_H264_PROFILE_IDC_BASELINE;
        case VAProfileH264Main:
            return STD_VIDEO_H264_PROFILE_IDC_MAIN;
        case VAProfileH264High:
        default:
            return STD_VIDEO_H264_PROFILE_IDC_HIGH;
    }
}

StdVideoH264ChromaFormatIdc std_chroma_from_va(uint32_t chroma_format_idc) {
    switch (chroma_format_idc) {
        case 0:
            return STD_VIDEO_H264_CHROMA_FORMAT_IDC_MONOCHROME;
        case 1:
            return STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
        case 2:
            return STD_VIDEO_H264_CHROMA_FORMAT_IDC_422;
        case 3:
            return STD_VIDEO_H264_CHROMA_FORMAT_IDC_444;
        default:
            return STD_VIDEO_H264_CHROMA_FORMAT_IDC_INVALID;
    }
}

StdVideoH264PocType std_poc_type_from_va(uint32_t pic_order_cnt_type) {
    switch (pic_order_cnt_type) {
        case 0:
            return STD_VIDEO_H264_POC_TYPE_0;
        case 1:
            return STD_VIDEO_H264_POC_TYPE_1;
        case 2:
            return STD_VIDEO_H264_POC_TYPE_2;
        default:
            return STD_VIDEO_H264_POC_TYPE_INVALID;
    }
}

StdVideoH264LevelIdc clamp_h264_level(StdVideoH264LevelIdc level, StdVideoH264LevelIdc max_level) {
    if (max_level == STD_VIDEO_H264_LEVEL_IDC_INVALID) {
        return level;
    }
    return static_cast<int>(level) <= static_cast<int>(max_level) ? level : max_level;
}

StdVideoH264LevelIdc derive_h264_level_idc(uint32_t width_mbs, uint32_t height_map_units, StdVideoH264LevelIdc max_level) {
    const uint64_t frame_mbs = static_cast<uint64_t>(width_mbs) * height_map_units;

    StdVideoH264LevelIdc level = STD_VIDEO_H264_LEVEL_IDC_6_2;
    if (frame_mbs <= 396) {
        level = STD_VIDEO_H264_LEVEL_IDC_2_0;
    } else if (frame_mbs <= 1620) {
        level = STD_VIDEO_H264_LEVEL_IDC_3_0;
    } else if (frame_mbs <= 3600) {
        level = STD_VIDEO_H264_LEVEL_IDC_3_1;
    } else if (frame_mbs <= 5120) {
        level = STD_VIDEO_H264_LEVEL_IDC_3_2;
    } else if (frame_mbs <= 8192) {
        level = STD_VIDEO_H264_LEVEL_IDC_4_1;
    } else if (frame_mbs <= 22080) {
        level = STD_VIDEO_H264_LEVEL_IDC_5_0;
    } else if (frame_mbs <= 36864) {
        level = STD_VIDEO_H264_LEVEL_IDC_5_2;
    }
    return clamp_h264_level(level, max_level);
}

void build_h264_std_parameters(
        H264VideoSession *session,
        VAProfile profile,
        const VkvvH264DecodeInput *input,
        H264StdParameters *std_params) {
    const VAPictureParameterBufferH264 *pic = input->pic;
    const bool frame_mbs_only = pic->seq_fields.bits.frame_mbs_only_flag != 0;
    uint32_t pic_height_in_map_units_minus1 = pic->picture_height_in_mbs_minus1;
    if (!frame_mbs_only && pic_height_in_map_units_minus1 > 0) {
        pic_height_in_map_units_minus1 = ((pic_height_in_map_units_minus1 + 1) / 2) - 1;
    }

    StdVideoH264SequenceParameterSet &sps = std_params->sps;
    sps = {};
    std_params->sps_scaling = {};
    std_params->pps_scaling = {};
    for (uint32_t i = 0; i < STD_VIDEO_H264_SCALING_LIST_4X4_NUM_LISTS; i++) {
        std::fill(std::begin(std_params->sps_scaling.ScalingList4x4[i]),
                  std::end(std_params->sps_scaling.ScalingList4x4[i]), 16);
        std::fill(std::begin(std_params->pps_scaling.ScalingList4x4[i]),
                  std::end(std_params->pps_scaling.ScalingList4x4[i]), 16);
    }
    for (uint32_t i = 0; i < STD_VIDEO_H264_SCALING_LIST_8X8_NUM_LISTS; i++) {
        std::fill(std::begin(std_params->sps_scaling.ScalingList8x8[i]),
                  std::end(std_params->sps_scaling.ScalingList8x8[i]), 16);
        std::fill(std::begin(std_params->pps_scaling.ScalingList8x8[i]),
                  std::end(std_params->pps_scaling.ScalingList8x8[i]), 16);
    }
    if (input->iq != nullptr) {
        for (uint32_t i = 0; i < 6; i++) {
            std::copy(std::begin(input->iq->ScalingList4x4[i]),
                      std::end(input->iq->ScalingList4x4[i]),
                      std::begin(std_params->pps_scaling.ScalingList4x4[i]));
        }
        for (uint32_t i = 0; i < 2; i++) {
            std::copy(std::begin(input->iq->ScalingList8x8[i]),
                      std::end(input->iq->ScalingList8x8[i]),
                      std::begin(std_params->pps_scaling.ScalingList8x8[i]));
        }
    }
    sps.flags.constraint_set0_flag = profile == VAProfileH264ConstrainedBaseline;
    sps.flags.constraint_set1_flag = profile == VAProfileH264ConstrainedBaseline;
    sps.flags.direct_8x8_inference_flag = pic->seq_fields.bits.direct_8x8_inference_flag;
    sps.flags.mb_adaptive_frame_field_flag = pic->seq_fields.bits.mb_adaptive_frame_field_flag;
    sps.flags.frame_mbs_only_flag = pic->seq_fields.bits.frame_mbs_only_flag;
    sps.flags.delta_pic_order_always_zero_flag = pic->seq_fields.bits.delta_pic_order_always_zero_flag;
    sps.flags.separate_colour_plane_flag = pic->seq_fields.bits.residual_colour_transform_flag;
    sps.flags.gaps_in_frame_num_value_allowed_flag = pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag;
    sps.flags.vui_parameters_present_flag = true;
    sps.profile_idc = std_profile_from_va(profile);
    sps.level_idc = derive_h264_level_idc(pic->picture_width_in_mbs_minus1 + 1,
                                          pic_height_in_map_units_minus1 + 1,
                                          session->max_level);
    sps.chroma_format_idc = std_chroma_from_va(pic->seq_fields.bits.chroma_format_idc);
    sps.seq_parameter_set_id = 0;
    sps.bit_depth_luma_minus8 = pic->bit_depth_luma_minus8;
    sps.bit_depth_chroma_minus8 = pic->bit_depth_chroma_minus8;
    sps.log2_max_frame_num_minus4 = pic->seq_fields.bits.log2_max_frame_num_minus4;
    sps.pic_order_cnt_type = std_poc_type_from_va(pic->seq_fields.bits.pic_order_cnt_type);
    sps.log2_max_pic_order_cnt_lsb_minus4 = pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    sps.max_num_ref_frames = pic->num_ref_frames;
    sps.pic_width_in_mbs_minus1 = pic->picture_width_in_mbs_minus1;
    sps.pic_height_in_map_units_minus1 = pic_height_in_map_units_minus1;
    sps.pScalingLists = &std_params->sps_scaling;
    sps.pSequenceParameterSetVui = &std_params->vui;

    StdVideoH264SequenceParameterSetVui &vui = std_params->vui;
    vui = {};
    std_params->vui_hrd = {};
    vui.flags.aspect_ratio_info_present_flag = true;
    vui.flags.video_signal_type_present_flag = true;
    vui.flags.color_description_present_flag = true;
    vui.flags.chroma_loc_info_present_flag = true;
    vui.flags.timing_info_present_flag = true;
    vui.flags.fixed_frame_rate_flag = true;
    vui.flags.bitstream_restriction_flag = true;
    vui.aspect_ratio_idc = STD_VIDEO_H264_ASPECT_RATIO_IDC_SQUARE;
    vui.sar_width = 1;
    vui.sar_height = 1;
    vui.video_format = 5;
    vui.colour_primaries = 6;
    vui.transfer_characteristics = 6;
    vui.matrix_coefficients = 6;
    vui.num_units_in_tick = 1001;
    vui.time_scale = 60000;
    vui.max_num_reorder_frames = std::min<uint8_t>(2, pic->num_ref_frames);
    vui.max_dec_frame_buffering = pic->num_ref_frames;
    vui.chroma_sample_loc_type_top_field = 0;
    vui.chroma_sample_loc_type_bottom_field = 0;
    vui.pHrdParameters = &std_params->vui_hrd;

    StdVideoH264PictureParameterSet &pps = std_params->pps;
    pps = {};
    pps.flags.transform_8x8_mode_flag = pic->pic_fields.bits.transform_8x8_mode_flag;
    pps.flags.redundant_pic_cnt_present_flag = pic->pic_fields.bits.redundant_pic_cnt_present_flag;
    pps.flags.constrained_intra_pred_flag = pic->pic_fields.bits.constrained_intra_pred_flag;
    pps.flags.deblocking_filter_control_present_flag = pic->pic_fields.bits.deblocking_filter_control_present_flag;
    pps.flags.weighted_pred_flag = pic->pic_fields.bits.weighted_pred_flag;
    pps.flags.bottom_field_pic_order_in_frame_present_flag = pic->pic_fields.bits.pic_order_present_flag;
    pps.flags.entropy_coding_mode_flag = pic->pic_fields.bits.entropy_coding_mode_flag;
    pps.flags.pic_scaling_matrix_present_flag = false;
    pps.seq_parameter_set_id = 0;
    pps.pic_parameter_set_id = static_cast<uint8_t>(std::min<uint32_t>(input->pic_parameter_set_id, 255));
    if (input->last_slice_count > 0 && input->last_slices != nullptr) {
        pps.num_ref_idx_l0_default_active_minus1 =
                static_cast<uint8_t>(std::min<uint32_t>(input->last_slices[0].num_ref_idx_l0_active_minus1, 31));
        pps.num_ref_idx_l1_default_active_minus1 =
                static_cast<uint8_t>(std::min<uint32_t>(input->last_slices[0].num_ref_idx_l1_active_minus1, 31));
    }
    if (input->all_slices_intra && pic->num_ref_frames >= 3) {
        pps.num_ref_idx_l0_default_active_minus1 =
                static_cast<uint8_t>(std::min<uint32_t>(pic->num_ref_frames - 2u, 31));
    }
    pps.weighted_bipred_idc = static_cast<StdVideoH264WeightedBipredIdc>(pic->pic_fields.bits.weighted_bipred_idc);
    pps.pic_init_qp_minus26 = pic->pic_init_qp_minus26;
    pps.pic_init_qs_minus26 = pic->pic_init_qs_minus26;
    pps.chroma_qp_index_offset = pic->chroma_qp_index_offset;
    pps.second_chroma_qp_index_offset = pic->second_chroma_qp_index_offset;
    pps.pScalingLists = &std_params->pps_scaling;
}

bool create_h264_session_parameters(
        VulkanRuntime *runtime,
        H264VideoSession *session,
        const H264StdParameters *std_params,
        VkVideoSessionParametersKHR *parameters,
        char *reason,
        size_t reason_size) {
    VkVideoDecodeH264SessionParametersAddInfoKHR add_info{};
    add_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
    add_info.stdSPSCount = 1;
    add_info.pStdSPSs = &std_params->sps;
    add_info.stdPPSCount = 1;
    add_info.pStdPPSs = &std_params->pps;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_info{};
    h264_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
    h264_info.maxStdSPSCount = 1;
    h264_info.maxStdPPSCount = 1;
    h264_info.pParametersAddInfo = &add_info;

    VkVideoSessionParametersCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    create_info.pNext = &h264_info;
    create_info.videoSession = session->video.session;

    VkResult result = runtime->create_video_session_parameters(runtime->device, &create_info, nullptr, parameters);
    if (!record_vk_result(runtime, result, "vkCreateVideoSessionParametersKHR", "H.264 parameters", reason, reason_size)) {
        return false;
    }
    return true;
}

bool create_empty_h264_session_parameters(
        VulkanRuntime *runtime,
        H264VideoSession *session,
        VkVideoSessionParametersKHR *parameters,
        char *reason,
        size_t reason_size) {
    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_info{};
    h264_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;

    VkVideoSessionParametersCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
    create_info.pNext = &h264_info;
    create_info.videoSession = session->video.session;

    VkResult result = runtime->create_video_session_parameters(runtime->device, &create_info, nullptr, parameters);
    if (!record_vk_result(runtime, result, "vkCreateVideoSessionParametersKHR", "empty H.264 parameters", reason, reason_size)) {
        return false;
    }
    return true;
}
} // namespace vkvv
