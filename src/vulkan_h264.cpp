#include "vulkan_runtime_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <new>
#include <vector>

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

bool h264_picture_is_invalid(const VAPictureH264 &picture) {
    return (picture.flags & VA_PICTURE_H264_INVALID) != 0 ||
           picture.picture_id == VA_INVALID_ID;
}

int allocate_dpb_slot(VkvvContext *vctx, const bool used_slots[max_h264_dpb_slots]) {
    for (uint32_t attempt = 0; attempt < max_h264_dpb_slots; attempt++) {
        const uint32_t slot = (vctx->next_dpb_slot + attempt) % max_h264_dpb_slots;
        if (!used_slots[slot]) {
            vctx->next_dpb_slot = (slot + 1) % max_h264_dpb_slots;
            return static_cast<int>(slot);
        }
    }
    return -1;
}

void fill_reference_info(const VAPictureH264 &picture, uint16_t frame_num, StdVideoDecodeH264ReferenceInfo *info) {
    *info = {};
    info->flags.top_field_flag = (picture.flags & VA_PICTURE_H264_TOP_FIELD) != 0;
    info->flags.bottom_field_flag = (picture.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    info->flags.used_for_long_term_reference = (picture.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) != 0;
    info->FrameNum = frame_num;
    info->PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_TOP] = picture.TopFieldOrderCnt;
    info->PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_BOTTOM] = picture.BottomFieldOrderCnt;
}

bool bitstream_has_idr(const VkvvH264DecodeInput *input) {
    for (uint32_t i = 0; i < input->slice_count; i++) {
        size_t offset = input->slice_offsets[i];
        if (offset + 3 < input->bitstream_size &&
            input->bitstream[offset] == 0 &&
            input->bitstream[offset + 1] == 0 &&
            input->bitstream[offset + 2] == 1) {
            offset += 3;
        } else if (offset + 4 < input->bitstream_size &&
                   input->bitstream[offset] == 0 &&
                   input->bitstream[offset + 1] == 0 &&
                   input->bitstream[offset + 2] == 0 &&
                   input->bitstream[offset + 3] == 1) {
            offset += 4;
        }
        if (offset < input->bitstream_size && (input->bitstream[offset] & 0x1f) == 5) {
            return true;
        }
    }
    return false;
}

bool choose_h264_format(VulkanRuntime *runtime, const VkVideoProfileInfoKHR *profile, VkFormat *format, char *reason, size_t reason_size) {
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
    *format = selected.format;
    runtime->h264_image_create_flags = selected.imageCreateFlags;
    runtime->h264_image_tiling = selected.imageTiling;
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

struct H264StdParameters {
    StdVideoH264SequenceParameterSet sps{};
    StdVideoH264SequenceParameterSetVui vui{};
    StdVideoH264HrdParameters vui_hrd{};
    StdVideoH264ScalingLists sps_scaling{};
    StdVideoH264ScalingLists pps_scaling{};
    StdVideoH264PictureParameterSet pps{};
};

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
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateVideoSessionParametersKHR(H.264) failed: %d", result);
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
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateVideoSessionParametersKHR(empty H.264) failed: %d", result);
        return false;
    }
    return true;
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
                      runtime->h264_picture_format,
                      static_cast<unsigned long long>(session->video.memory_bytes),
                      runtime->video_maintenance2);
        return VA_STATUS_SUCCESS;
    }

    destroy_h264_video_session(runtime, session);

    VideoProfileChain profile_chain;
    VideoCapabilitiesChain capabilities;
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

    VkFormat picture_format = VK_FORMAT_UNDEFINED;
    if (!choose_h264_format(runtime, &profile_chain.profile, &picture_format, reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    const VkExtent2D session_extent = h264_session_extent(extent, capabilities.video);

    VkVideoSessionCreateInfoKHR session_info{};
    session_info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    session_info.queueFamilyIndex = runtime->decode_queue_family;
    session_info.pVideoProfile = &profile_chain.profile;
    session_info.pictureFormat = picture_format;
    session_info.maxCodedExtent = session_extent;
    session_info.referencePictureFormat = picture_format;
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
        .picture_format = picture_format,
        .reference_picture_format = picture_format,
        .max_coded_extent = session_extent,
        .chroma_subsampling = profile_chain.profile.chromaSubsampling,
        .luma_bit_depth = profile_chain.profile.lumaBitDepth,
        .chroma_bit_depth = profile_chain.profile.chromaBitDepth,
    };

    if (!bind_video_session_memory(runtime, &session->video, reason, reason_size)) {
        destroy_h264_video_session(runtime, session);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    runtime->h264_picture_format = picture_format;
    session->max_dpb_slots = session_info.maxDpbSlots;
    session->max_active_reference_pictures = session_info.maxActiveReferencePictures;
    std::snprintf(reason, reason_size,
                  "H.264 video session ready: codec=h264 requested=%ux%u actual=%ux%u format=%d dpb=%u refs=%u mem=%llu inline_params=%u",
                  extent.width, extent.height, session_extent.width, session_extent.height, picture_format,
                  session_info.maxDpbSlots, session_info.maxActiveReferencePictures,
                  static_cast<unsigned long long>(session->video.memory_bytes),
                  runtime->video_maintenance2);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_decode_h264(
        void *runtime_ptr,
        void *session_ptr,
        VkvvDriver *drv,
        VkvvContext *vctx,
        VkvvSurface *target,
        VAProfile profile,
        const VkvvH264DecodeInput *input,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    auto *session = static_cast<H264VideoSession *>(session_ptr);
    if (runtime == nullptr || session == nullptr || drv == nullptr || vctx == nullptr ||
        target == nullptr || input == nullptr || input->pic == nullptr) {
        std::snprintf(reason, reason_size, "missing H.264 decode state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (session->video.session == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "missing H.264 video session");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (input->bitstream == nullptr || input->bitstream_size == 0 || input->slice_offsets == nullptr || input->slice_count == 0) {
        std::snprintf(reason, reason_size, "missing H.264 bitstream data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (input->pic->bit_depth_luma_minus8 != 0 || input->pic->bit_depth_chroma_minus8 != 0) {
        std::snprintf(reason, reason_size, "H.264 Vulkan path currently supports only 8-bit decode");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (input->pic->pic_fields.bits.field_pic_flag) {
        std::snprintf(reason, reason_size, "H.264 Vulkan path currently supports only progressive pictures");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if ((session->decode_flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) == 0) {
        std::snprintf(reason, reason_size, "H.264 decode requires coincident DPB/output support in this prototype");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    const VkExtent2D coded_extent = {
        round_up_16(static_cast<uint32_t>(input->pic->picture_width_in_mbs_minus1 + 1) * 16),
        round_up_16(static_cast<uint32_t>(input->pic->picture_height_in_mbs_minus1 + 1) * 16),
    };
    if (!ensure_surface_resource(runtime, target, coded_extent, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    bool used_slots[max_h264_dpb_slots] = {};

    struct ReferenceRecord {
        VkvvSurface *surface = nullptr;
        SurfaceResource *resource = nullptr;
        VkVideoPictureResourceInfoKHR picture{};
        StdVideoDecodeH264ReferenceInfo std_ref{};
        VkVideoDecodeH264DpbSlotInfoKHR h264_slot{};
        VkVideoReferenceSlotInfoKHR slot{};
    };

    std::array<ReferenceRecord, max_h264_dpb_slots> references{};
    uint32_t reference_count = 0;
    for (uint32_t i = 0; i < max_va_h264_reference_frames; i++) {
        const VAPictureH264 &ref_pic = input->pic->ReferenceFrames[i];
        if (h264_picture_is_invalid(ref_pic)) {
            continue;
        }

        auto *ref_surface = static_cast<VkvvSurface *>(vkvv_object_get(drv, ref_pic.picture_id, VKVV_OBJECT_SURFACE));
        if (ref_surface == nullptr) {
            std::snprintf(reason, reason_size, "H.264 reference surface %u is missing", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (!ref_surface->decoded || ref_surface->vulkan == nullptr || ref_surface->dpb_slot < 0) {
            std::snprintf(reason, reason_size, "H.264 reference surface %u is not decoded yet", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        const uint32_t slot = static_cast<uint32_t>(ref_surface->dpb_slot);
        if (slot >= max_h264_dpb_slots) {
            std::snprintf(reason, reason_size, "H.264 reference surface %u has invalid DPB slot", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        used_slots[slot] = true;

        auto *resource = static_cast<SurfaceResource *>(ref_surface->vulkan);
        ReferenceRecord &record = references[reference_count++];
        record.surface = ref_surface;
        record.resource = resource;
        record.picture = make_picture_resource(resource, resource->extent);
        fill_reference_info(ref_pic, static_cast<uint16_t>(ref_pic.frame_idx), &record.std_ref);
        record.h264_slot.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
        record.h264_slot.pStdReferenceInfo = &record.std_ref;
        record.slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        record.slot.pNext = &record.h264_slot;
        record.slot.slotIndex = ref_surface->dpb_slot;
        record.slot.pPictureResource = &record.picture;
    }

    const bool current_is_reference = input->has_slice_header ?
                                      input->first_nal_ref_idc != 0 :
                                      input->pic->pic_fields.bits.reference_pic_flag != 0;
    const bool current_is_idr = input->has_slice_header ?
                                input->first_nal_unit_type == 5 :
                                bitstream_has_idr(input);
    const uint8_t current_pps_id = static_cast<uint8_t>(std::min<uint32_t>(input->pic_parameter_set_id, 255));
    const uint16_t current_frame_num = input->has_slice_header ? input->parsed_frame_num : input->pic->frame_num;
    const uint16_t current_idr_pic_id = static_cast<uint16_t>(std::min<uint32_t>(input->idr_pic_id, 65535));
    const int32_t current_top_poc = input->pic->CurrPic.TopFieldOrderCnt;
    const int32_t current_bottom_poc = input->pic->CurrPic.BottomFieldOrderCnt;
    if (target->dpb_slot < 0 || target->dpb_slot >= static_cast<int>(max_h264_dpb_slots) || used_slots[target->dpb_slot]) {
        const int slot = allocate_dpb_slot(vctx, used_slots);
        if (slot < 0) {
            std::snprintf(reason, reason_size, "no free H.264 DPB slot for current picture");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        target->dpb_slot = slot;
    }
    used_slots[target->dpb_slot] = true;

    H264StdParameters std_params{};
    build_h264_std_parameters(session, profile, input, &std_params);

    const bool use_inline_parameters = runtime->video_maintenance2;
    VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
    if (!use_inline_parameters &&
        !create_h264_session_parameters(runtime, session, &std_params, &parameters, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!session->video.initialized) {
        VkVideoSessionParametersKHR reset_parameters = VK_NULL_HANDLE;
        bool created_reset_parameters = false;
        if (!use_inline_parameters) {
            if (!create_empty_h264_session_parameters(runtime, session, &reset_parameters, reason, reason_size)) {
                runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            created_reset_parameters = true;
        }
        const bool reset = reset_h264_session(runtime, session, reset_parameters, reason, reason_size);
        if (created_reset_parameters) {
            runtime->destroy_video_session_parameters(runtime->device, reset_parameters, nullptr);
        }
        if (!reset) {
            runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    UploadBuffer upload{};
    if (!create_upload_buffer(runtime, session, input, &upload, reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        destroy_upload_buffer(runtime, &upload);
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, &upload);
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        std::snprintf(reason, reason_size, "vkResetFences for H.264 decode failed: %d", result);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, &upload);
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        std::snprintf(reason, reason_size, "vkResetCommandBuffer for H.264 decode failed: %d", result);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, &upload);
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        std::snprintf(reason, reason_size, "vkBeginCommandBuffer for H.264 decode failed: %d", result);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    auto *target_resource = static_cast<SurfaceResource *>(target->vulkan);
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(reference_count + 1);
    for (uint32_t i = 0; i < reference_count; i++) {
        add_image_layout_barrier(&barriers, references[i].resource, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                                 VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR);
    }
    add_image_layout_barrier(&barriers, target_resource, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                             VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }

    StdVideoDecodeH264ReferenceInfo setup_std_ref{};
    VkVideoDecodeH264DpbSlotInfoKHR setup_h264_slot{};
    VkVideoReferenceSlotInfoKHR setup_slot{};
    VkVideoPictureResourceInfoKHR setup_picture{};
    const VkVideoReferenceSlotInfoKHR *setup_slot_ptr = nullptr;
    setup_picture = make_picture_resource(target_resource, coded_extent);
    fill_reference_info(input->pic->CurrPic, current_frame_num, &setup_std_ref);
    setup_std_ref.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_TOP] = current_top_poc;
    setup_std_ref.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_BOTTOM] = current_bottom_poc;
    setup_h264_slot.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
    setup_h264_slot.pStdReferenceInfo = &setup_std_ref;
    setup_slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    setup_slot.pNext = &setup_h264_slot;
    setup_slot.slotIndex = target->dpb_slot;
    setup_slot.pPictureResource = &setup_picture;
    setup_slot_ptr = &setup_slot;

    std::array<VkVideoReferenceSlotInfoKHR, max_h264_dpb_slots> begin_reference_slots{};
    uint32_t begin_reference_count = 0;
    for (uint32_t i = 0; i < reference_count; i++) {
        begin_reference_slots[begin_reference_count++] = references[i].slot;
    }
    if (setup_slot_ptr != nullptr && begin_reference_count < begin_reference_slots.size()) {
        begin_reference_slots[begin_reference_count] = *setup_slot_ptr;
        begin_reference_slots[begin_reference_count].slotIndex = -1;
        begin_reference_count++;
    }

    VkVideoBeginCodingInfoKHR video_begin{};
    video_begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession = session->video.session;
    video_begin.videoSessionParameters = parameters;
    video_begin.referenceSlotCount = begin_reference_count;
    video_begin.pReferenceSlots = begin_reference_count > 0 ? begin_reference_slots.data() : nullptr;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    StdVideoDecodeH264PictureInfo std_picture{};
    std_picture.flags.field_pic_flag = input->pic->pic_fields.bits.field_pic_flag;
    std_picture.flags.is_intra = input->all_slices_intra;
    std_picture.flags.IdrPicFlag = current_is_idr;
    std_picture.flags.bottom_field_flag = (input->pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    std_picture.flags.is_reference = current_is_reference;
    std_picture.flags.complementary_field_pair =
            (input->pic->CurrPic.flags & VA_PICTURE_H264_TOP_FIELD) != 0 &&
            (input->pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    std_picture.seq_parameter_set_id = 0;
    std_picture.pic_parameter_set_id = current_pps_id;
    std_picture.frame_num = current_frame_num;
    std_picture.idr_pic_id = current_idr_pic_id;
    std_picture.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_TOP] = current_top_poc;
    std_picture.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_BOTTOM] = current_bottom_poc;

    std::vector<uint32_t> vulkan_slice_offsets(input->slice_offsets, input->slice_offsets + input->slice_count);

    VkVideoDecodeH264PictureInfoKHR h264_picture{};
    h264_picture.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
    h264_picture.pStdPictureInfo = &std_picture;
    h264_picture.sliceCount = input->slice_count;
    h264_picture.pSliceOffsets = vulkan_slice_offsets.data();
    VkVideoDecodeH264InlineSessionParametersInfoKHR inline_parameters{};
    if (use_inline_parameters) {
        inline_parameters.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_INLINE_SESSION_PARAMETERS_INFO_KHR;
        inline_parameters.pStdSPS = &std_params.sps;
        inline_parameters.pStdPPS = &std_params.pps;
        h264_picture.pNext = &inline_parameters;
    }

    VkVideoPictureResourceInfoKHR dst_picture = make_picture_resource(target_resource, coded_extent);

    std::array<VkVideoReferenceSlotInfoKHR, max_h264_dpb_slots> decode_reference_slots{};
    for (uint32_t i = 0; i < reference_count; i++) {
        decode_reference_slots[i] = references[i].slot;
    }

    VkVideoDecodeInfoKHR decode_info{};
    decode_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext = &h264_picture;
    decode_info.srcBuffer = upload.buffer;
    decode_info.srcBufferOffset = 0;
    decode_info.srcBufferRange = upload.size;
    decode_info.dstPictureResource = dst_picture;
    decode_info.pSetupReferenceSlot = setup_slot_ptr;
    decode_info.referenceSlotCount = reference_count;
    decode_info.pReferenceSlots = reference_count > 0 ? decode_reference_slots.data() : nullptr;
    runtime->cmd_decode_video(runtime->command_buffer, &decode_info);

    VkVideoEndCodingInfoKHR video_end{};
    video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, &upload);
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        const VASliceParameterBufferH264 *first_slice =
                (input->last_slices != nullptr && input->last_slice_count > 0) ? &input->last_slices[0] : nullptr;
        std::snprintf(reason, reason_size,
                      "vkEndCommandBuffer for H.264 decode failed: %d vaProfile=%d nal=%u/%u ref=%u setup=%u slot=%d pps=%u frame=%u idr=%u refs=%u off0=%u bytes=%02x%02x%02x%02x seq=%08x pic=%08x curr(flags=%08x idx=%u rawpoc=%d/%d poc=%d/%d) s0(type=%u bit=%u l0=%u l1=%u) sps(profile=%d level=%d refs=%u poc=%d log2poc=%u) pps(l0=%u l1=%u)",
                      result, profile, input->first_nal_unit_type, input->first_nal_ref_idc,
                      current_is_reference, setup_slot_ptr != nullptr, target->dpb_slot,
                      std_picture.pic_parameter_set_id,
                      std_picture.frame_num, std_picture.idr_pic_id, reference_count,
                      vulkan_slice_offsets.empty() ? 0 : vulkan_slice_offsets[0],
                      input->bitstream_size > 0 ? input->bitstream[0] : 0,
                      input->bitstream_size > 1 ? input->bitstream[1] : 0,
                      input->bitstream_size > 2 ? input->bitstream[2] : 0,
                      input->bitstream_size > 3 ? input->bitstream[3] : 0,
                      input->pic->seq_fields.value, input->pic->pic_fields.value,
                      input->pic->CurrPic.flags, input->pic->CurrPic.frame_idx,
                      input->pic->CurrPic.TopFieldOrderCnt, input->pic->CurrPic.BottomFieldOrderCnt,
                      current_top_poc, current_bottom_poc,
                      first_slice != nullptr ? first_slice->slice_type : 0,
                      first_slice != nullptr ? first_slice->slice_data_bit_offset : 0,
                      first_slice != nullptr ? first_slice->num_ref_idx_l0_active_minus1 : 0,
                      first_slice != nullptr ? first_slice->num_ref_idx_l1_active_minus1 : 0,
                      std_params.sps.profile_idc,
                      std_params.sps.level_idc, std_params.sps.max_num_ref_frames,
                      std_params.sps.pic_order_cnt_type, std_params.sps.log2_max_pic_order_cnt_lsb_minus4,
                      std_params.pps.num_ref_idx_l0_default_active_minus1,
                      std_params.pps.num_ref_idx_l1_default_active_minus1);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const bool submitted = submit_command_buffer_and_wait(runtime, reason, reason_size, "H.264 decode");
    destroy_upload_buffer(runtime, &upload);
    runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
    if (!submitted) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    target->decoded = true;
    std::snprintf(reason, reason_size, "submitted H.264 Vulkan decode: %ux%u slices=%u bytes=%zu refs=%u slot=%d",
                  coded_extent.width, coded_extent.height, input->slice_count, input->bitstream_size,
                  reference_count, target->dpb_slot);
    return VA_STATUS_SUCCESS;
}
