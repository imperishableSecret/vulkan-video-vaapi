#include "internal.h"
#include "api.h"
#include "va/private.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace vkvv {

    VkImageUsageFlags h264_encode_input_image_usage() {
        return VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    VkImageUsageFlags h264_encode_dpb_image_usage() {
        return VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    void destroy_h264_encode_session(VulkanRuntime* runtime, H264EncodeSession* session) {
        if (session == nullptr) {
            return;
        }
        if (runtime != nullptr && session->query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(runtime->device, session->query_pool, nullptr);
        }
        session->query_pool = VK_NULL_HANDLE;
        destroy_upload_buffer(runtime, &session->output);
        destroy_upload_buffer(runtime, &session->readback);
        if (runtime != nullptr && session->parameters != VK_NULL_HANDLE && runtime->destroy_video_session_parameters != nullptr) {
            runtime->destroy_video_session_parameters(runtime->device, session->parameters, nullptr);
        }
        session->parameters = VK_NULL_HANDLE;
        destroy_video_session(runtime, &session->video);
        session->max_level                     = STD_VIDEO_H264_LEVEL_IDC_5_2;
        session->encode_flags                  = 0;
        session->rate_control_modes            = 0;
        session->feedback_flags                = 0;
        session->max_quality_levels            = 0;
        session->quality_level                 = 0;
        session->max_dpb_slots                 = 0;
        session->max_active_reference_pictures = 0;
        session->next_dpb_slot                 = 0;
        session->surface_slots.clear();
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

    EncodeImageKey h264_encode_reconstructed_key(const H264EncodeSession* session, VkExtent2D coded_extent, unsigned int rt_format, unsigned int fourcc) {
        return {
            .codec_operation    = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
            .codec_profile      = session->video.key.codec_profile,
            .picture_format     = session->video.key.reference_picture_format,
            .va_rt_format       = rt_format,
            .va_fourcc          = fourcc,
            .coded_extent       = coded_extent,
            .usage              = h264_encode_dpb_image_usage(),
            .create_flags       = 0,
            .tiling             = VK_IMAGE_TILING_OPTIMAL,
            .chroma_subsampling = session->video.key.chroma_subsampling,
            .luma_bit_depth     = session->video.key.luma_bit_depth,
            .chroma_bit_depth   = session->video.key.chroma_bit_depth,
        };
    }

    bool ensure_buffer(VulkanRuntime* runtime, const VideoProfileSpec* profile_spec, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags preferred_memory,
                       VkMemoryPropertyFlags fallback_memory, UploadBuffer* buffer, const char* label, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, label) || buffer == nullptr || size == 0) {
            return false;
        }
        buffer->size = size;
        if (buffer->buffer != VK_NULL_HANDLE && buffer->capacity >= size) {
            return true;
        }

        destroy_upload_buffer(runtime, buffer);
        buffer->size = size;

        VideoProfileChain         profile_chain(profile_spec != nullptr ? *profile_spec : h264_encode_profile_spec);
        VkVideoProfileListInfoKHR profile_list{};
        profile_list.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        profile_list.profileCount = 1;
        profile_list.pProfiles    = &profile_chain.profile;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.pNext       = profile_spec != nullptr ? &profile_list : nullptr;
        buffer_info.size        = size;
        buffer_info.usage       = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &buffer->buffer);
        if (!record_vk_result(runtime, result, "vkCreateBuffer", label, reason, reason_size)) {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(runtime->device, buffer->buffer, &requirements);
        buffer->capacity        = size;
        buffer->allocation_size = requirements.size;

        uint32_t              memory_type_index = 0;
        VkMemoryPropertyFlags selected_memory   = preferred_memory;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, preferred_memory, &memory_type_index)) {
            selected_memory = fallback_memory;
            if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, fallback_memory, &memory_type_index)) {
                destroy_upload_buffer(runtime, buffer);
                std::snprintf(reason, reason_size, "no memory type for %s", label);
                return false;
            }
        }
        buffer->coherent = (selected_memory & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;
        result                        = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &buffer->memory);
        if (!record_vk_result(runtime, result, "vkAllocateMemory", label, reason, reason_size)) {
            destroy_upload_buffer(runtime, buffer);
            return false;
        }

        result = vkBindBufferMemory(runtime->device, buffer->buffer, buffer->memory, 0);
        if (!record_vk_result(runtime, result, "vkBindBufferMemory", label, reason, reason_size)) {
            destroy_upload_buffer(runtime, buffer);
            return false;
        }
        return true;
    }

    bool ensure_h264_encode_query_pool(VulkanRuntime* runtime, H264EncodeSession* session, char* reason, size_t reason_size) {
        constexpr VkVideoEncodeFeedbackFlagsKHR required_feedback =
            VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR | VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
        if ((session->feedback_flags & required_feedback) != required_feedback) {
            std::snprintf(reason, reason_size, "H.264 encode feedback flags missing required bits: supported=0x%x", session->feedback_flags);
            return false;
        }
        if (session->query_pool != VK_NULL_HANDLE) {
            return true;
        }

        VideoProfileChain                           profile_chain(h264_encode_profile_spec);

        VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback_info{};
        feedback_info.sType               = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
        feedback_info.pNext               = &profile_chain.profile;
        feedback_info.encodeFeedbackFlags = required_feedback;

        VkQueryPoolCreateInfo query_info{};
        query_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.pNext      = &feedback_info;
        query_info.queryType  = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
        query_info.queryCount = 1;

        VkResult result = vkCreateQueryPool(runtime->device, &query_info, nullptr, &session->query_pool);
        return record_vk_result(runtime, result, "vkCreateQueryPool", "H.264 encode feedback", reason, reason_size);
    }

    bool reset_h264_encode_session(VulkanRuntime* runtime, H264EncodeSession* session, char* reason, size_t reason_size) {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources_for_queue(runtime, runtime->encode_queue_family, runtime->encode_queue, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "H.264 encode session reset", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "H.264 encode session reset", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "H.264 encode session reset", reason, reason_size)) {
            return false;
        }

        VkVideoBeginCodingInfoKHR video_begin{};
        video_begin.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
        video_begin.videoSession           = session->video.session;
        video_begin.videoSessionParameters = session->parameters;
        runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

        VkVideoCodingControlInfoKHR control{};
        control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
        control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
        runtime->cmd_control_video_coding(runtime->command_buffer, &control);

        VkVideoEndCodingInfoKHR video_end{};
        video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
        runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "H.264 encode session reset", reason, reason_size)) {
            return false;
        }
        if (!submit_command_buffer_and_wait_on_queue(runtime, runtime->encode_queue, reason, reason_size, "H.264 encode session reset")) {
            return false;
        }
        session->video.initialized = true;
        return true;
    }

    bool h264_encode_reference_requested(const VkvvH264EncodeInput* input) {
        return input != nullptr && input->picture != nullptr && input->picture->pic_fields.bits.reference_pic_flag != 0;
    }

    bool h264_encode_picture_is_invalid(const VAPictureH264& picture) {
        return picture.picture_id == VA_INVALID_ID || (picture.flags & VA_PICTURE_H264_INVALID) != 0;
    }

    const VAPictureH264* h264_encode_l0_reference(const VkvvH264EncodeInput* input) {
        if (input == nullptr || input->frame_type != VKVV_H264_ENCODE_FRAME_P || input->slices == nullptr || input->slice_count == 0) {
            return nullptr;
        }
        const VAPictureH264& slice_ref = input->slices[0].RefPicList0[0];
        if (!h264_encode_picture_is_invalid(slice_ref)) {
            return &slice_ref;
        }
        for (uint32_t i = 0; i < 16; i++) {
            const VAPictureH264& picture_ref = input->picture->ReferenceFrames[i];
            if (!h264_encode_picture_is_invalid(picture_ref)) {
                return &picture_ref;
            }
        }
        return nullptr;
    }

    int h264_encode_dpb_slot_for_surface(const H264EncodeSession* session, VASurfaceID surface_id) {
        if (session == nullptr || surface_id == VA_INVALID_ID) {
            return -1;
        }
        for (const H264SurfaceDpbSlot& record : session->surface_slots) {
            if (record.surface_id == surface_id) {
                return record.slot;
            }
        }
        return -1;
    }

    void h264_encode_set_dpb_slot_for_surface(H264EncodeSession* session, VASurfaceID surface_id, int slot) {
        if (session == nullptr || surface_id == VA_INVALID_ID || slot < 0) {
            return;
        }
        for (H264SurfaceDpbSlot& record : session->surface_slots) {
            if (record.surface_id == surface_id) {
                record.slot = slot;
                return;
            }
        }
        session->surface_slots.push_back({surface_id, slot});
    }

    int allocate_h264_encode_dpb_slot(H264EncodeSession* session, int forbidden_slot) {
        if (session == nullptr || session->max_dpb_slots == 0) {
            return -1;
        }
        const uint32_t slot_count = std::min<uint32_t>(session->max_dpb_slots, 2);
        for (uint32_t attempt = 0; attempt < slot_count; attempt++) {
            const uint32_t slot = (session->next_dpb_slot + attempt) % slot_count;
            if (static_cast<int>(slot) == forbidden_slot) {
                continue;
            }
            session->next_dpb_slot = (slot + 1) % slot_count;
            return static_cast<int>(slot);
        }
        return -1;
    }

    void fill_h264_encode_reference_lists(const VkvvH264EncodeInput* input, int l0_slot, StdVideoEncodeH264ReferenceListsInfo* lists) {
        *lists                              = {};
        lists->num_ref_idx_l0_active_minus1 = input->picture->num_ref_idx_l0_active_minus1;
        lists->num_ref_idx_l1_active_minus1 = input->picture->num_ref_idx_l1_active_minus1;
        lists->RefPicList0[0]               = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
        lists->RefPicList1[0]               = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
        for (uint32_t i = 1; i < STD_VIDEO_H264_MAX_NUM_LIST_REF; i++) {
            lists->RefPicList0[i] = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
            lists->RefPicList1[i] = STD_VIDEO_H264_NO_REFERENCE_PICTURE;
        }
        if (l0_slot >= 0) {
            lists->num_ref_idx_l0_active_minus1 = 0;
            lists->num_ref_idx_l1_active_minus1 = 0;
            lists->RefPicList0[0]               = static_cast<uint8_t>(l0_slot);
        }
    }

    void fill_h264_encode_picture_info(const VkvvH264EncodeInput* input, const StdVideoEncodeH264ReferenceListsInfo* lists, StdVideoEncodeH264PictureInfo* picture) {
        *picture                      = {};
        picture->flags.IdrPicFlag     = input->frame_type == VKVV_H264_ENCODE_FRAME_IDR;
        picture->flags.is_reference   = h264_encode_reference_requested(input);
        picture->seq_parameter_set_id = input->sequence->seq_parameter_set_id;
        picture->pic_parameter_set_id = input->picture->pic_parameter_set_id;
        picture->idr_pic_id           = input->slices[0].idr_pic_id;
        picture->primary_pic_type     = h264_encode_picture_type(input);
        picture->frame_num            = input->picture->frame_num;
        picture->PicOrderCnt          = input->picture->CurrPic.TopFieldOrderCnt;
        picture->temporal_id          = 0;
        picture->pRefLists            = lists;
    }

    void fill_h264_encode_slice_header(const VkvvH264EncodeInput* input, StdVideoEncodeH264SliceHeader* header) {
        const VAEncSliceParameterBufferH264& slice     = input->slices[0];
        *header                                        = {};
        header->flags.direct_spatial_mv_pred_flag      = slice.direct_spatial_mv_pred_flag;
        header->flags.num_ref_idx_active_override_flag = slice.num_ref_idx_active_override_flag;
        header->first_mb_in_slice                      = slice.macroblock_address;
        header->slice_type                             = h264_encode_slice_type(slice);
        header->slice_alpha_c0_offset_div2             = slice.slice_alpha_c0_offset_div2;
        header->slice_beta_offset_div2                 = slice.slice_beta_offset_div2;
        header->slice_qp_delta                         = slice.slice_qp_delta;
        header->cabac_init_idc                         = static_cast<StdVideoH264CabacInitIdc>(slice.cabac_init_idc);
        header->disable_deblocking_filter_idc          = static_cast<StdVideoH264DisableDeblockingFilterIdc>(slice.disable_deblocking_filter_idc);
    }

    void fill_h264_encode_rate_control(const VkvvH264EncodeInput* input, VkVideoEncodeH264RateControlInfoKHR* h264_rate_control, VkVideoEncodeRateControlInfoKHR* rate_control) {
        const uint32_t gop_frames = input != nullptr && input->sequence != nullptr && input->sequence->intra_period > 0 ? input->sequence->intra_period : 1;
        const uint32_t idr_period = input != nullptr && input->sequence != nullptr && input->sequence->intra_idr_period > 0 ? input->sequence->intra_idr_period : gop_frames;

        *h264_rate_control                        = {};
        h264_rate_control->sType                  = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR;
        h264_rate_control->gopFrameCount          = gop_frames;
        h264_rate_control->idrPeriod              = idr_period;
        h264_rate_control->consecutiveBFrameCount = 0;
        h264_rate_control->temporalLayerCount     = 1;

        *rate_control                 = {};
        rate_control->sType           = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
        rate_control->pNext           = h264_rate_control;
        rate_control->rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
        rate_control->layerCount      = 0;
        rate_control->pLayers         = nullptr;
    }

    void fill_h264_encode_reference_info(const VkvvH264EncodeInput* input, StdVideoEncodeH264ReferenceInfo* reference) {
        *reference                     = {};
        reference->primary_pic_type    = h264_encode_picture_type(input);
        reference->FrameNum            = input->picture->frame_num;
        reference->PicOrderCnt         = input->picture->CurrPic.TopFieldOrderCnt;
        reference->long_term_pic_num   = 0;
        reference->long_term_frame_idx = 0;
        reference->temporal_id         = 0;
    }

    void fill_h264_encode_reference_info_from_picture(const VAPictureH264& picture, StdVideoEncodeH264ReferenceInfo* reference) {
        *reference                     = {};
        reference->primary_pic_type    = STD_VIDEO_H264_PICTURE_TYPE_P;
        reference->FrameNum            = picture.frame_idx;
        reference->PicOrderCnt         = picture.TopFieldOrderCnt;
        reference->long_term_pic_num   = 0;
        reference->long_term_frame_idx = 0;
        reference->temporal_id         = 0;
    }

    bool fetch_h264_encode_parameter_bytes(VulkanRuntime* runtime, H264EncodeSession* session, const VkvvH264EncodeInput* input, std::vector<uint8_t>* parameter_bytes,
                                           char* reason, size_t reason_size) {
        if (runtime == nullptr || session == nullptr || input == nullptr || input->sequence == nullptr || input->picture == nullptr || parameter_bytes == nullptr) {
            std::snprintf(reason, reason_size, "missing H.264 encode parameter fetch state");
            return false;
        }
        parameter_bytes->clear();
        if (input->frame_type != VKVV_H264_ENCODE_FRAME_IDR) {
            return true;
        }

        VkVideoEncodeH264SessionParametersGetInfoKHR h264_get{};
        h264_get.sType       = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR;
        h264_get.writeStdSPS = VK_TRUE;
        h264_get.writeStdPPS = VK_TRUE;
        h264_get.stdSPSId    = input->sequence->seq_parameter_set_id;
        h264_get.stdPPSId    = input->picture->pic_parameter_set_id;

        VkVideoEncodeSessionParametersGetInfoKHR get{};
        get.sType                  = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
        get.pNext                  = &h264_get;
        get.videoSessionParameters = session->parameters;

        VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264_feedback{};
        h264_feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;

        VkVideoEncodeSessionParametersFeedbackInfoKHR feedback{};
        feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
        feedback.pNext = &h264_feedback;

        size_t   data_size = 0;
        VkResult result    = runtime->get_encoded_video_session_parameters(runtime->device, &get, &feedback, &data_size, nullptr);
        if (!record_vk_result(runtime, result, "vkGetEncodedVideoSessionParametersKHR", "H.264 encode parameter size", reason, reason_size)) {
            return false;
        }
        if (data_size == 0) {
            std::snprintf(reason, reason_size, "H.264 encode returned empty SPS/PPS parameters");
            return false;
        }

        try {
            parameter_bytes->resize(data_size);
        } catch (const std::bad_alloc&) {
            std::snprintf(reason, reason_size, "failed to allocate H.264 parameter bytes");
            return false;
        }
        result = runtime->get_encoded_video_session_parameters(runtime->device, &get, &feedback, &data_size, parameter_bytes->data());
        if (!record_vk_result(runtime, result, "vkGetEncodedVideoSessionParametersKHR", "H.264 encode parameters", reason, reason_size)) {
            parameter_bytes->clear();
            return false;
        }
        parameter_bytes->resize(data_size);
        return true;
    }

    bool copy_encoded_output_to_coded_buffer(VulkanRuntime* runtime, H264EncodeSession* session, VkvvBuffer* coded, const std::vector<uint8_t>& parameter_prefix, uint32_t offset,
                                             uint32_t bytes, char* reason, size_t reason_size) {
        if (coded == nullptr || coded->coded_payload == nullptr) {
            std::snprintf(reason, reason_size, "missing H.264 coded output buffer");
            return false;
        }
        if (bytes == 0 || static_cast<VkDeviceSize>(offset) + bytes > session->readback.size) {
            std::snprintf(reason, reason_size, "invalid H.264 encoded byte range offset=%u bytes=%u capacity=%llu", offset, bytes,
                          static_cast<unsigned long long>(session->readback.size));
            return false;
        }

        void*    mapped = nullptr;
        VkResult result = vkMapMemory(runtime->device, session->readback.memory, 0, session->readback.size, 0, &mapped);
        if (!record_vk_result(runtime, result, "vkMapMemory", "H.264 encoded readback", reason, reason_size)) {
            return false;
        }
        if (!session->readback.coherent) {
            VkMappedMemoryRange range{};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = session->readback.memory;
            range.offset = 0;
            range.size   = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(runtime->device, 1, &range);
        }

        const uint64_t generation = coded->coded_payload->generation;
        VAStatus       status     = VA_STATUS_SUCCESS;
        if (parameter_prefix.empty()) {
            status = vkvv_coded_buffer_store(coded, static_cast<uint8_t*>(mapped) + offset, bytes, 0, generation);
        } else {
            const size_t total_size = parameter_prefix.size() + bytes;
            if (total_size > coded->coded_payload->capacity) {
                vkUnmapMemory(runtime->device, session->readback.memory);
                std::snprintf(reason, reason_size, "H.264 coded output plus parameters exceeds coded buffer: params=%llu bytes=%u capacity=%u",
                              static_cast<unsigned long long>(parameter_prefix.size()), bytes, coded->coded_payload->capacity);
                return false;
            }
            std::vector<uint8_t> combined;
            try {
                combined.resize(total_size);
            } catch (const std::bad_alloc&) {
                vkUnmapMemory(runtime->device, session->readback.memory);
                std::snprintf(reason, reason_size, "failed to allocate combined H.264 coded output");
                return false;
            }
            std::memcpy(combined.data(), parameter_prefix.data(), parameter_prefix.size());
            std::memcpy(combined.data() + parameter_prefix.size(), static_cast<uint8_t*>(mapped) + offset, bytes);
            status = vkvv_coded_buffer_store(coded, combined.data(), combined.size(), 0, generation);
        }
        vkUnmapMemory(runtime->device, session->readback.memory);
        if (status != VA_STATUS_SUCCESS) {
            std::snprintf(reason, reason_size, "failed to store H.264 coded output: %s", vaErrorStr(status));
            return false;
        }
        return true;
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
        session->feedback_flags                = capabilities.encode.supportedEncodeFeedbackFlags;
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

VAStatus vkvv_vulkan_encode_h264(void* runtime_ptr, void* session_ptr, VkvvDriver* drv, VkvvContext* vctx, const VkvvH264EncodeInput* input, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<H264EncodeSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr || drv == nullptr || vctx == nullptr || input == nullptr || input->picture == nullptr || input->slices == nullptr) {
        std::snprintf(reason, reason_size, "missing H.264 encode submission state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (session->video.session == VK_NULL_HANDLE || session->parameters == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "missing H.264 encode session");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (input->frame_type == VKVV_H264_ENCODE_FRAME_B) {
        std::snprintf(reason, reason_size, "H.264 Vulkan encode does not support B pictures yet");
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }

    auto* input_surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, input->input_surface, VKVV_OBJECT_SURFACE));
    if (input_surface == nullptr || input_surface->vulkan == nullptr) {
        std::snprintf(reason, reason_size, "missing H.264 encode input surface %u", input->input_surface);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto* input_resource = static_cast<SurfaceResource*>(input_surface->vulkan);
    if (input_resource->image == VK_NULL_HANDLE || (input_resource->encode_key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) == 0 || input_resource->content_generation == 0) {
        std::snprintf(reason, reason_size, "H.264 encode input surface %u has no uploaded input image", input->input_surface);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    auto* coded = static_cast<VkvvBuffer*>(vkvv_object_get(drv, input->coded_buffer, VKVV_OBJECT_BUFFER));
    if (coded == nullptr || coded->buffer_class != VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT || coded->coded_payload == nullptr) {
        std::snprintf(reason, reason_size, "invalid H.264 encode coded buffer %u", input->coded_buffer);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    const uint64_t coded_generation = coded->coded_payload->generation + 1;
    vkvv_coded_buffer_mark_pending(coded, coded_generation);
    auto fail_coded = [coded, coded_generation](VAStatus status) {
        vkvv_coded_buffer_fail(coded, status, coded_generation);
        return status;
    };

    const VkExtent2D coded_extent{
        .width  = round_up_16(input->width),
        .height = round_up_16(input->height),
    };

    const VAPictureH264* l0_reference_picture  = h264_encode_l0_reference(input);
    VkvvSurface*         l0_reference_surface  = nullptr;
    SurfaceResource*     l0_reference_resource = nullptr;
    int                  l0_reference_slot     = -1;
    if (input->frame_type == VKVV_H264_ENCODE_FRAME_P) {
        if (l0_reference_picture == nullptr) {
            std::snprintf(reason, reason_size, "missing H.264 P-frame L0 reference");
            return fail_coded(VA_STATUS_ERROR_INVALID_SURFACE);
        }
        if (input->reconstructed_surface == l0_reference_picture->picture_id) {
            std::snprintf(reason, reason_size, "H.264 P-frame reconstructed surface must differ from L0 reference surface %u", input->reconstructed_surface);
            return fail_coded(VA_STATUS_ERROR_INVALID_SURFACE);
        }
        l0_reference_surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, l0_reference_picture->picture_id, VKVV_OBJECT_SURFACE));
        if (l0_reference_surface == nullptr || !l0_reference_surface->decoded || l0_reference_surface->vulkan == nullptr) {
            std::snprintf(reason, reason_size, "H.264 P-frame reference surface %u is not ready", l0_reference_picture->picture_id);
            return fail_coded(VA_STATUS_ERROR_INVALID_SURFACE);
        }
        l0_reference_resource = static_cast<SurfaceResource*>(l0_reference_surface->vulkan);
        if (l0_reference_resource->image == VK_NULL_HANDLE || (l0_reference_resource->encode_key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR) == 0) {
            std::snprintf(reason, reason_size, "H.264 P-frame reference surface %u has no encode DPB image", l0_reference_picture->picture_id);
            return fail_coded(VA_STATUS_ERROR_INVALID_SURFACE);
        }
        l0_reference_slot = h264_encode_dpb_slot_for_surface(session, l0_reference_picture->picture_id);
        if (l0_reference_slot < 0) {
            std::snprintf(reason, reason_size, "H.264 P-frame reference surface %u has no DPB slot", l0_reference_picture->picture_id);
            return fail_coded(VA_STATUS_ERROR_INVALID_SURFACE);
        }
    }

    VkvvSurface*     reconstructed_surface  = nullptr;
    SurfaceResource* reconstructed_resource = nullptr;
    int              reconstructed_slot     = -1;
    if (h264_encode_reference_requested(input)) {
        reconstructed_surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, input->reconstructed_surface, VKVV_OBJECT_SURFACE));
        if (reconstructed_surface == nullptr) {
            std::snprintf(reason, reason_size, "missing H.264 encode reconstructed surface %u", input->reconstructed_surface);
            return fail_coded(VA_STATUS_ERROR_INVALID_SURFACE);
        }
        reconstructed_surface->driver_instance_id = input_surface->driver_instance_id;
        reconstructed_surface->stream_id          = vctx->stream_id;
        reconstructed_surface->codec_operation    = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        const EncodeImageKey reconstructed_key    = h264_encode_reconstructed_key(session, coded_extent, input_surface->rt_format, input_surface->fourcc);
        if (!ensure_encode_input_resource(runtime, reconstructed_surface, reconstructed_key, reason, reason_size)) {
            return fail_coded(VA_STATUS_ERROR_ALLOCATION_FAILED);
        }
        reconstructed_resource = static_cast<SurfaceResource*>(reconstructed_surface->vulkan);
        reconstructed_slot     = h264_encode_dpb_slot_for_surface(session, input->reconstructed_surface);
        if (reconstructed_slot < 0 || reconstructed_slot == l0_reference_slot) {
            reconstructed_slot = allocate_h264_encode_dpb_slot(session, l0_reference_slot);
            if (reconstructed_slot < 0) {
                std::snprintf(reason, reason_size, "no free H.264 encode DPB slot for reconstructed surface %u", input->reconstructed_surface);
                return fail_coded(VA_STATUS_ERROR_ALLOCATION_FAILED);
            }
            h264_encode_set_dpb_slot_for_surface(session, input->reconstructed_surface, reconstructed_slot);
        }
    }

    if (!ensure_h264_encode_query_pool(runtime, session, reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (!session->video.initialized && !reset_h264_encode_session(runtime, session, reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    std::vector<uint8_t> parameter_prefix;
    if (!fetch_h264_encode_parameter_bytes(runtime, session, input, &parameter_prefix, reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (parameter_prefix.size() >= coded->coded_payload->capacity) {
        std::snprintf(reason, reason_size, "H.264 SPS/PPS parameters exceed coded buffer capacity: params=%llu capacity=%u",
                      static_cast<unsigned long long>(parameter_prefix.size()), coded->coded_payload->capacity);
        return fail_coded(VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
    }

    const VkDeviceSize output_capacity = std::max<VkDeviceSize>(coded->coded_payload->capacity - parameter_prefix.size(), 1);
    if (!ensure_buffer(runtime, &h264_encode_profile_spec, output_capacity, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &session->output, "H.264 encoded output", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    if (!ensure_buffer(runtime, nullptr, output_capacity, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &session->readback, "H.264 encoded readback", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    if ((session->rate_control_modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) == 0) {
        std::snprintf(reason, reason_size, "H.264 encode CQP requires disabled Vulkan rate-control mode, supported=0x%x", session->rate_control_modes);
        return fail_coded(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    }

    StdVideoEncodeH264ReferenceListsInfo reference_lists{};
    fill_h264_encode_reference_lists(input, l0_reference_slot, &reference_lists);
    StdVideoEncodeH264PictureInfo std_picture{};
    fill_h264_encode_picture_info(input, &reference_lists, &std_picture);
    StdVideoEncodeH264SliceHeader std_slice{};
    fill_h264_encode_slice_header(input, &std_slice);

    VkVideoEncodeH264NaluSliceInfoKHR nalu_slice{};
    nalu_slice.sType           = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
    nalu_slice.constantQp      = std::clamp<int32_t>(input->picture->pic_init_qp, 0, 51);
    nalu_slice.pStdSliceHeader = &std_slice;

    VkVideoEncodeH264PictureInfoKHR h264_picture{};
    h264_picture.sType               = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
    h264_picture.naluSliceEntryCount = 1;
    h264_picture.pNaluSliceEntries   = &nalu_slice;
    h264_picture.pStdPictureInfo     = &std_picture;
    h264_picture.generatePrefixNalu  = VK_FALSE;

    StdVideoEncodeH264ReferenceInfo    setup_std_reference{};
    VkVideoEncodeH264DpbSlotInfoKHR    setup_h264_slot{};
    VkVideoReferenceSlotInfoKHR        setup_slot{};
    VkVideoPictureResourceInfoKHR      setup_picture{};
    const VkVideoReferenceSlotInfoKHR* setup_slot_ptr = nullptr;
    if (reconstructed_resource != nullptr) {
        fill_h264_encode_reference_info(input, &setup_std_reference);
        setup_h264_slot.sType             = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
        setup_h264_slot.pStdReferenceInfo = &setup_std_reference;
        setup_picture                     = make_picture_resource(reconstructed_resource, coded_extent);
        setup_slot.sType                  = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        setup_slot.pNext                  = &setup_h264_slot;
        setup_slot.slotIndex              = reconstructed_slot;
        setup_slot.pPictureResource       = &setup_picture;
        setup_slot_ptr                    = &setup_slot;
    }

    StdVideoEncodeH264ReferenceInfo l0_std_reference{};
    VkVideoEncodeH264DpbSlotInfoKHR l0_h264_slot{};
    VkVideoReferenceSlotInfoKHR     l0_slot{};
    VkVideoPictureResourceInfoKHR   l0_picture{};
    const bool                      has_l0_reference = l0_reference_resource != nullptr && l0_reference_slot >= 0;
    if (has_l0_reference) {
        fill_h264_encode_reference_info_from_picture(*l0_reference_picture, &l0_std_reference);
        l0_h264_slot.sType             = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
        l0_h264_slot.pStdReferenceInfo = &l0_std_reference;
        l0_picture                     = make_picture_resource(l0_reference_resource, coded_extent);
        l0_slot.sType                  = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        l0_slot.pNext                  = &l0_h264_slot;
        l0_slot.slotIndex              = l0_reference_slot;
        l0_slot.pPictureResource       = &l0_picture;
    }

    VkVideoEncodeH264RateControlInfoKHR h264_rate_control{};
    VkVideoEncodeRateControlInfoKHR     rate_control{};
    fill_h264_encode_rate_control(input, &h264_rate_control, &rate_control);

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources_for_queue(runtime, runtime->encode_queue_family, runtime->encode_queue, reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (!record_vk_result(runtime, result, "vkResetFences", "H.264 encode", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "H.264 encode", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "H.264 encode", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    std::vector<VkImageMemoryBarrier2> image_barriers;
    add_image_layout_barrier(&image_barriers, input_resource, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);
    if (l0_reference_resource != nullptr) {
        add_image_layout_barrier(&image_barriers, l0_reference_resource, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR, VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);
    }
    if (reconstructed_resource != nullptr) {
        add_image_layout_barrier(&image_barriers, reconstructed_resource, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
                                 VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR);
    }
    if (!image_barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers.size());
        dependency.pImageMemoryBarriers    = image_barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }

    vkCmdResetQueryPool(runtime->command_buffer, session->query_pool, 0, 1);

    VkVideoBeginCodingInfoKHR video_begin{};
    video_begin.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession           = session->video.session;
    video_begin.videoSessionParameters = session->parameters;
    video_begin.referenceSlotCount     = has_l0_reference ? 1u : 0u;
    video_begin.pReferenceSlots        = has_l0_reference ? &l0_slot : nullptr;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    VkVideoCodingControlInfoKHR rate_control_command{};
    rate_control_command.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
    rate_control_command.pNext = &rate_control;
    rate_control_command.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
    runtime->cmd_control_video_coding(runtime->command_buffer, &rate_control_command);

    VkVideoPictureResourceInfoKHR src_picture = make_picture_resource(input_resource, coded_extent);
    VkVideoEncodeInfoKHR          encode_info{};
    encode_info.sType               = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
    encode_info.pNext               = &h264_picture;
    encode_info.dstBuffer           = session->output.buffer;
    encode_info.dstBufferOffset     = 0;
    encode_info.dstBufferRange      = session->output.size;
    encode_info.srcPictureResource  = src_picture;
    encode_info.pSetupReferenceSlot = setup_slot_ptr;
    encode_info.referenceSlotCount  = has_l0_reference ? 1u : 0u;
    encode_info.pReferenceSlots     = has_l0_reference ? &l0_slot : nullptr;

    vkCmdBeginQuery(runtime->command_buffer, session->query_pool, 0, 0);
    runtime->cmd_encode_video(runtime->command_buffer, &encode_info);
    vkCmdEndQuery(runtime->command_buffer, session->query_pool, 0);

    VkVideoEndCodingInfoKHR video_end{};
    video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

    VkBufferMemoryBarrier2 encoded_barrier{};
    encoded_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    encoded_barrier.srcStageMask        = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    encoded_barrier.srcAccessMask       = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
    encoded_barrier.dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    encoded_barrier.dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
    encoded_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    encoded_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    encoded_barrier.buffer              = session->output.buffer;
    encoded_barrier.offset              = 0;
    encoded_barrier.size                = session->output.size;

    VkDependencyInfo encoded_dependency{};
    encoded_dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    encoded_dependency.bufferMemoryBarrierCount = 1;
    encoded_dependency.pBufferMemoryBarriers    = &encoded_barrier;
    vkCmdPipelineBarrier2(runtime->command_buffer, &encoded_dependency);

    VkBufferCopy copy_region{};
    copy_region.size = session->output.size;
    vkCmdCopyBuffer(runtime->command_buffer, session->output.buffer, session->readback.buffer, 1, &copy_region);

    VkBufferMemoryBarrier2 readback_barrier{};
    readback_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    readback_barrier.srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    readback_barrier.srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    readback_barrier.dstStageMask        = VK_PIPELINE_STAGE_2_HOST_BIT;
    readback_barrier.dstAccessMask       = VK_ACCESS_2_HOST_READ_BIT;
    readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.buffer              = session->readback.buffer;
    readback_barrier.offset              = 0;
    readback_barrier.size                = session->readback.size;

    VkDependencyInfo readback_dependency{};
    readback_dependency.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    readback_dependency.bufferMemoryBarrierCount = 1;
    readback_dependency.pBufferMemoryBarriers    = &readback_barrier;
    vkCmdPipelineBarrier2(runtime->command_buffer, &readback_dependency);

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "H.264 encode", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (!submit_command_buffer_and_wait_on_queue(runtime, runtime->encode_queue, reason, reason_size, "H.264 encode")) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    struct EncodeFeedback {
        uint32_t offset;
        uint32_t bytes_written;
        int32_t  status;
    } feedback{};
    result = vkGetQueryPoolResults(runtime->device, session->query_pool, 0, 1, sizeof(feedback), &feedback, sizeof(feedback), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
    if (!record_vk_result(runtime, result, "vkGetQueryPoolResults", "H.264 encode feedback", reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (feedback.status <= 0) {
        std::snprintf(reason, reason_size, "H.264 encode feedback reported status=%d bytes=%u", feedback.status, feedback.bytes_written);
        return fail_coded(feedback.status == VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR ? VA_STATUS_ERROR_NOT_ENOUGH_BUFFER : VA_STATUS_ERROR_OPERATION_FAILED);
    }
    if (!copy_encoded_output_to_coded_buffer(runtime, session, coded, parameter_prefix, feedback.offset, feedback.bytes_written, reason, reason_size)) {
        return fail_coded(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    if (reconstructed_surface != nullptr) {
        reconstructed_surface->decoded     = true;
        reconstructed_surface->sync_status = VA_STATUS_SUCCESS;
        reconstructed_surface->work_state  = VKVV_SURFACE_WORK_READY;
        if (reconstructed_resource != nullptr) {
            reconstructed_resource->content_generation++;
        }
    }
    std::snprintf(reason, reason_size, "submitted H.264 Vulkan encode: %ux%u params=%llu bytes=%u coded=%u rc=disabled input_mem=%llu recon_mem=%llu output_mem=%llu",
                  coded_extent.width, coded_extent.height, static_cast<unsigned long long>(parameter_prefix.size()), feedback.bytes_written, input->coded_buffer,
                  static_cast<unsigned long long>(input_resource->allocation_size),
                  static_cast<unsigned long long>(reconstructed_resource != nullptr ? reconstructed_resource->allocation_size : 0),
                  static_cast<unsigned long long>(session->output.allocation_size));
    return VA_STATUS_SUCCESS;
}
