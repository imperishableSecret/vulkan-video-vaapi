#include "internal.h"

#include <algorithm>
#include <cstdio>

namespace vkvv {

    StdVideoH265ProfileIdc std_hevc_profile_from_va(VAProfile profile) {
        switch (profile) {
            case VAProfileHEVCMain: return STD_VIDEO_H265_PROFILE_IDC_MAIN;
            case VAProfileHEVCMain10: return STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
            case VAProfileHEVCMain12: return STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
            default: return STD_VIDEO_H265_PROFILE_IDC_MAIN;
        }
    }

    void fill_default_hevc_scaling_lists(StdVideoH265ScalingLists* scaling) {
        *scaling = {};
        for (uint32_t i = 0; i < STD_VIDEO_H265_SCALING_LIST_4X4_NUM_LISTS; i++) {
            std::fill(std::begin(scaling->ScalingList4x4[i]), std::end(scaling->ScalingList4x4[i]), 16);
        }
        for (uint32_t i = 0; i < STD_VIDEO_H265_SCALING_LIST_8X8_NUM_LISTS; i++) {
            std::fill(std::begin(scaling->ScalingList8x8[i]), std::end(scaling->ScalingList8x8[i]), 16);
            std::fill(std::begin(scaling->ScalingList16x16[i]), std::end(scaling->ScalingList16x16[i]), 16);
            scaling->ScalingListDCCoef16x16[i] = 16;
        }
        for (uint32_t i = 0; i < STD_VIDEO_H265_SCALING_LIST_32X32_NUM_LISTS; i++) {
            std::fill(std::begin(scaling->ScalingList32x32[i]), std::end(scaling->ScalingList32x32[i]), 16);
            scaling->ScalingListDCCoef32x32[i] = 16;
        }
    }

    void build_hevc_std_parameters(HEVCVideoSession* session, VAProfile profile, const VkvvHEVCDecodeInput* input, HEVCStdParameters* std_params) {
        const VAPictureParameterBufferHEVC* pic = input->pic;
        *std_params                             = {};

        std_params->ptl.flags.general_tier_flag                  = 0;
        std_params->ptl.flags.general_progressive_source_flag    = 1;
        std_params->ptl.flags.general_interlaced_source_flag     = 0;
        std_params->ptl.flags.general_non_packed_constraint_flag = 1;
        std_params->ptl.flags.general_frame_only_constraint_flag = 1;
        std_params->ptl.general_profile_idc                      = std_hevc_profile_from_va(profile);
        std_params->ptl.general_level_idc                        = session->max_level;

        for (uint32_t i = 0; i < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; i++) {
            std_params->vps_dpb.max_dec_pic_buffering_minus1[i] = pic->sps_max_dec_pic_buffering_minus1;
            std_params->vps_dpb.max_num_reorder_pics[i]         = pic->pic_fields.bits.NoPicReorderingFlag ? 0 : std::min<uint8_t>(pic->sps_max_dec_pic_buffering_minus1, 4);
            std_params->vps_dpb.max_latency_increase_plus1[i]   = 0;
            std_params->sps_dpb.max_dec_pic_buffering_minus1[i] = pic->sps_max_dec_pic_buffering_minus1;
            std_params->sps_dpb.max_num_reorder_pics[i]         = std_params->vps_dpb.max_num_reorder_pics[i];
            std_params->sps_dpb.max_latency_increase_plus1[i]   = 0;
        }

        std_params->vps.flags.vps_temporal_id_nesting_flag             = 1;
        std_params->vps.flags.vps_sub_layer_ordering_info_present_flag = 0;
        std_params->vps.vps_video_parameter_set_id                     = 0;
        std_params->vps.vps_max_sub_layers_minus1                      = 0;
        std_params->vps.pDecPicBufMgr                                  = &std_params->vps_dpb;
        std_params->vps.pProfileTierLevel                              = &std_params->ptl;

        fill_default_hevc_scaling_lists(&std_params->scaling);
        if (input->has_iq && input->iq != nullptr) {
            for (uint32_t i = 0; i < 6; i++) {
                std::copy(std::begin(input->iq->ScalingList4x4[i]), std::end(input->iq->ScalingList4x4[i]), std::begin(std_params->scaling.ScalingList4x4[i]));
                std::copy(std::begin(input->iq->ScalingList8x8[i]), std::end(input->iq->ScalingList8x8[i]), std::begin(std_params->scaling.ScalingList8x8[i]));
                std::copy(std::begin(input->iq->ScalingList16x16[i]), std::end(input->iq->ScalingList16x16[i]), std::begin(std_params->scaling.ScalingList16x16[i]));
                std_params->scaling.ScalingListDCCoef16x16[i] = input->iq->ScalingListDC16x16[i];
            }
            for (uint32_t i = 0; i < 2; i++) {
                std::copy(std::begin(input->iq->ScalingList32x32[i]), std::end(input->iq->ScalingList32x32[i]), std::begin(std_params->scaling.ScalingList32x32[i]));
                std_params->scaling.ScalingListDCCoef32x32[i] = input->iq->ScalingListDC32x32[i];
            }
        }

        StdVideoH265SequenceParameterSet& sps            = std_params->sps;
        sps.flags.sps_temporal_id_nesting_flag           = 1;
        sps.flags.separate_colour_plane_flag             = pic->pic_fields.bits.separate_colour_plane_flag;
        sps.flags.scaling_list_enabled_flag              = pic->pic_fields.bits.scaling_list_enabled_flag;
        sps.flags.sps_scaling_list_data_present_flag     = pic->pic_fields.bits.scaling_list_enabled_flag;
        sps.flags.amp_enabled_flag                       = pic->pic_fields.bits.amp_enabled_flag;
        sps.flags.sample_adaptive_offset_enabled_flag    = pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag;
        sps.flags.pcm_enabled_flag                       = pic->pic_fields.bits.pcm_enabled_flag;
        sps.flags.pcm_loop_filter_disabled_flag          = pic->pic_fields.bits.pcm_loop_filter_disabled_flag;
        sps.flags.long_term_ref_pics_present_flag        = pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag;
        sps.flags.sps_temporal_mvp_enabled_flag          = pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag;
        sps.flags.strong_intra_smoothing_enabled_flag    = pic->pic_fields.bits.strong_intra_smoothing_enabled_flag;
        sps.flags.vui_parameters_present_flag            = 1;
        sps.chroma_format_idc                            = static_cast<StdVideoH265ChromaFormatIdc>(pic->pic_fields.bits.chroma_format_idc);
        sps.pic_width_in_luma_samples                    = pic->pic_width_in_luma_samples;
        sps.pic_height_in_luma_samples                   = pic->pic_height_in_luma_samples;
        sps.sps_video_parameter_set_id                   = 0;
        sps.sps_max_sub_layers_minus1                    = 0;
        sps.sps_seq_parameter_set_id                     = 0;
        sps.bit_depth_luma_minus8                        = pic->bit_depth_luma_minus8;
        sps.bit_depth_chroma_minus8                      = pic->bit_depth_chroma_minus8;
        sps.log2_max_pic_order_cnt_lsb_minus4            = pic->log2_max_pic_order_cnt_lsb_minus4;
        sps.log2_min_luma_coding_block_size_minus3       = pic->log2_min_luma_coding_block_size_minus3;
        sps.log2_diff_max_min_luma_coding_block_size     = pic->log2_diff_max_min_luma_coding_block_size;
        sps.log2_min_luma_transform_block_size_minus2    = pic->log2_min_transform_block_size_minus2;
        sps.log2_diff_max_min_luma_transform_block_size  = pic->log2_diff_max_min_transform_block_size;
        sps.max_transform_hierarchy_depth_inter          = pic->max_transform_hierarchy_depth_inter;
        sps.max_transform_hierarchy_depth_intra          = pic->max_transform_hierarchy_depth_intra;
        sps.num_short_term_ref_pic_sets                  = std::min<uint8_t>(pic->num_short_term_ref_pic_sets, STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS);
        sps.num_long_term_ref_pics_sps                   = std::min<uint8_t>(pic->num_long_term_ref_pic_sps, STD_VIDEO_H265_MAX_LONG_TERM_REF_PICS_SPS);
        sps.pcm_sample_bit_depth_luma_minus1             = pic->pcm_sample_bit_depth_luma_minus1;
        sps.pcm_sample_bit_depth_chroma_minus1           = pic->pcm_sample_bit_depth_chroma_minus1;
        sps.log2_min_pcm_luma_coding_block_size_minus3   = pic->log2_min_pcm_luma_coding_block_size_minus3;
        sps.log2_diff_max_min_pcm_luma_coding_block_size = pic->log2_diff_max_min_pcm_luma_coding_block_size;
        sps.pProfileTierLevel                            = &std_params->ptl;
        sps.pDecPicBufMgr                                = &std_params->sps_dpb;
        sps.pScalingLists                                = &std_params->scaling;
        sps.pSequenceParameterSetVui                     = &std_params->sps_vui;

        StdVideoH265SequenceParameterSetVui& vui  = std_params->sps_vui;
        vui.flags.aspect_ratio_info_present_flag  = 1;
        vui.flags.video_signal_type_present_flag  = 1;
        vui.flags.colour_description_present_flag = 1;
        vui.flags.vui_timing_info_present_flag    = 1;
        vui.flags.vui_hrd_parameters_present_flag = 0;
        vui.aspect_ratio_idc                      = STD_VIDEO_H265_ASPECT_RATIO_IDC_SQUARE;
        vui.sar_width                             = 1;
        vui.sar_height                            = 1;
        vui.video_format                          = 5;
        vui.colour_primaries                      = 2;
        vui.transfer_characteristics              = 2;
        vui.matrix_coeffs                         = 2;
        vui.vui_num_units_in_tick                 = 1001;
        vui.vui_time_scale                        = 60000;

        StdVideoH265PictureParameterSet& pps                  = std_params->pps;
        pps.flags.dependent_slice_segments_enabled_flag       = pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag;
        pps.flags.output_flag_present_flag                    = pic->slice_parsing_fields.bits.output_flag_present_flag;
        pps.flags.sign_data_hiding_enabled_flag               = pic->pic_fields.bits.sign_data_hiding_enabled_flag;
        pps.flags.cabac_init_present_flag                     = pic->slice_parsing_fields.bits.cabac_init_present_flag;
        pps.flags.constrained_intra_pred_flag                 = pic->pic_fields.bits.constrained_intra_pred_flag;
        pps.flags.transform_skip_enabled_flag                 = pic->pic_fields.bits.transform_skip_enabled_flag;
        pps.flags.cu_qp_delta_enabled_flag                    = pic->pic_fields.bits.cu_qp_delta_enabled_flag;
        pps.flags.pps_slice_chroma_qp_offsets_present_flag    = pic->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag;
        pps.flags.weighted_pred_flag                          = pic->pic_fields.bits.weighted_pred_flag;
        pps.flags.weighted_bipred_flag                        = pic->pic_fields.bits.weighted_bipred_flag;
        pps.flags.transquant_bypass_enabled_flag              = pic->pic_fields.bits.transquant_bypass_enabled_flag;
        pps.flags.tiles_enabled_flag                          = pic->pic_fields.bits.tiles_enabled_flag;
        pps.flags.entropy_coding_sync_enabled_flag            = pic->pic_fields.bits.entropy_coding_sync_enabled_flag;
        pps.flags.loop_filter_across_tiles_enabled_flag       = pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag;
        pps.flags.pps_loop_filter_across_slices_enabled_flag  = pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag;
        pps.flags.deblocking_filter_control_present_flag      = 1;
        pps.flags.deblocking_filter_override_enabled_flag     = pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag;
        pps.flags.pps_deblocking_filter_disabled_flag         = pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;
        pps.flags.pps_scaling_list_data_present_flag          = pic->pic_fields.bits.scaling_list_enabled_flag;
        pps.flags.lists_modification_present_flag             = pic->slice_parsing_fields.bits.lists_modification_present_flag;
        pps.flags.slice_segment_header_extension_present_flag = pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag;
        pps.pps_pic_parameter_set_id                          = 0;
        pps.pps_seq_parameter_set_id                          = 0;
        pps.sps_video_parameter_set_id                        = 0;
        pps.num_extra_slice_header_bits                       = pic->num_extra_slice_header_bits;
        pps.num_ref_idx_l0_default_active_minus1              = pic->num_ref_idx_l0_default_active_minus1;
        pps.num_ref_idx_l1_default_active_minus1              = pic->num_ref_idx_l1_default_active_minus1;
        pps.init_qp_minus26                                   = pic->init_qp_minus26;
        pps.diff_cu_qp_delta_depth                            = pic->diff_cu_qp_delta_depth;
        pps.pps_cb_qp_offset                                  = pic->pps_cb_qp_offset;
        pps.pps_cr_qp_offset                                  = pic->pps_cr_qp_offset;
        pps.pps_beta_offset_div2                              = pic->pps_beta_offset_div2;
        pps.pps_tc_offset_div2                                = pic->pps_tc_offset_div2;
        pps.log2_parallel_merge_level_minus2                  = pic->log2_parallel_merge_level_minus2;
        pps.num_tile_columns_minus1                           = pic->num_tile_columns_minus1;
        pps.num_tile_rows_minus1                              = pic->num_tile_rows_minus1;
        std::copy(std::begin(pic->column_width_minus1), std::end(pic->column_width_minus1), std::begin(pps.column_width_minus1));
        std::copy(std::begin(pic->row_height_minus1), std::end(pic->row_height_minus1), std::begin(pps.row_height_minus1));
        pps.pScalingLists = &std_params->scaling;
    }

    bool create_hevc_session_parameters(VulkanRuntime* runtime, HEVCVideoSession* session, const HEVCStdParameters* std_params, VkVideoSessionParametersKHR* parameters,
                                        char* reason, size_t reason_size) {
        VkVideoDecodeH265SessionParametersAddInfoKHR add_info{};
        add_info.sType       = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
        add_info.stdVPSCount = 1;
        add_info.pStdVPSs    = &std_params->vps;
        add_info.stdSPSCount = 1;
        add_info.pStdSPSs    = &std_params->sps;
        add_info.stdPPSCount = 1;
        add_info.pStdPPSs    = &std_params->pps;

        VkVideoDecodeH265SessionParametersCreateInfoKHR h265_info{};
        h265_info.sType              = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
        h265_info.maxStdVPSCount     = 1;
        h265_info.maxStdSPSCount     = 1;
        h265_info.maxStdPPSCount     = 1;
        h265_info.pParametersAddInfo = &add_info;

        VkVideoSessionParametersCreateInfoKHR create_info{};
        create_info.sType        = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        create_info.pNext        = &h265_info;
        create_info.videoSession = session->video.session;

        VkResult result = runtime->create_video_session_parameters(runtime->device, &create_info, nullptr, parameters);
        if (!record_vk_result(runtime, result, "vkCreateVideoSessionParametersKHR", "HEVC parameters", reason, reason_size)) {
            return false;
        }
        return true;
    }

    bool create_empty_hevc_session_parameters(VulkanRuntime* runtime, HEVCVideoSession* session, VkVideoSessionParametersKHR* parameters, char* reason, size_t reason_size) {
        VkVideoDecodeH265SessionParametersCreateInfoKHR h265_info{};
        h265_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;

        VkVideoSessionParametersCreateInfoKHR create_info{};
        create_info.sType        = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
        create_info.pNext        = &h265_info;
        create_info.videoSession = session->video.session;

        VkResult result = runtime->create_video_session_parameters(runtime->device, &create_info, nullptr, parameters);
        if (!record_vk_result(runtime, result, "vkCreateVideoSessionParametersKHR", "empty HEVC parameters", reason, reason_size)) {
            return false;
        }
        return true;
    }

} // namespace vkvv
