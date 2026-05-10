#include "internal.h"
#include "api.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace vkvv;

VAStatus vkvv_vulkan_decode_h264(void* runtime_ptr, void* session_ptr, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvH264DecodeInput* input,
                                 char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<H264VideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr || drv == nullptr || vctx == nullptr || target == nullptr || input == nullptr || input->pic == nullptr) {
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
    const DecodeImageKey decode_key = {
        .codec_operation          = session->video.key.codec_operation,
        .codec_profile            = session->video.key.codec_profile,
        .picture_format           = session->video.key.picture_format,
        .reference_picture_format = session->video.key.reference_picture_format,
        .va_rt_format             = target->rt_format,
        .va_fourcc                = target->fourcc,
        .coded_extent             = coded_extent,
        .usage                    = session->video.key.image_usage,
        .create_flags             = session->video.key.image_create_flags,
        .tiling                   = session->video.key.image_tiling,
        .chroma_subsampling       = session->video.key.chroma_subsampling,
        .luma_bit_depth           = session->video.key.luma_bit_depth,
        .chroma_bit_depth         = session->video.key.chroma_bit_depth,
    };
    if (!ensure_surface_resource(runtime, target, decode_key, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    const VASurfaceID target_surface_id = vctx->render_target;
    if (target_surface_id == VA_INVALID_ID) {
        std::snprintf(reason, reason_size, "missing H.264 target surface id");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    bool used_slots[max_h264_dpb_slots] = {};

    struct ReferenceRecord {
        VkvvSurface*                    surface  = nullptr;
        SurfaceResource*                resource = nullptr;
        VkVideoPictureResourceInfoKHR   picture{};
        StdVideoDecodeH264ReferenceInfo std_ref{};
        VkVideoDecodeH264DpbSlotInfoKHR h264_slot{};
        VkVideoReferenceSlotInfoKHR     slot{};
    };

    std::array<ReferenceRecord, max_h264_dpb_slots> references{};
    uint32_t                                        reference_count = 0;
    for (uint32_t i = 0; i < max_va_h264_reference_frames; i++) {
        const VAPictureH264& ref_pic = input->pic->ReferenceFrames[i];
        if (h264_picture_is_invalid(ref_pic)) {
            continue;
        }

        auto* ref_surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, ref_pic.picture_id, VKVV_OBJECT_SURFACE));
        if (ref_surface == nullptr) {
            std::snprintf(reason, reason_size, "H.264 reference surface %u is missing", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        VAStatus ref_status = complete_pending_surface_work_if_needed(runtime, ref_surface, "H.264 reference", reason, reason_size);
        if (ref_status != VA_STATUS_SUCCESS) {
            return ref_status;
        }
        const int ref_dpb_slot = h264_dpb_slot_for_surface(session, ref_pic.picture_id);
        if (!ref_surface->decoded || ref_surface->vulkan == nullptr || ref_dpb_slot < 0) {
            std::snprintf(reason, reason_size, "H.264 reference surface %u is not decoded yet", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        const uint32_t slot = static_cast<uint32_t>(ref_dpb_slot);
        if (slot >= max_h264_dpb_slots) {
            std::snprintf(reason, reason_size, "H.264 reference surface %u has invalid DPB slot", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        used_slots[slot] = true;

        auto*            resource = static_cast<SurfaceResource*>(ref_surface->vulkan);
        ReferenceRecord& record   = references[reference_count++];
        record.surface            = ref_surface;
        record.resource           = resource;
        record.picture            = make_picture_resource(resource, resource->extent);
        fill_reference_info(ref_pic, static_cast<uint16_t>(ref_pic.frame_idx), &record.std_ref);
        record.h264_slot.sType             = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
        record.h264_slot.pStdReferenceInfo = &record.std_ref;
        record.slot.sType                  = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        record.slot.pNext                  = &record.h264_slot;
        record.slot.slotIndex              = ref_dpb_slot;
        record.slot.pPictureResource       = &record.picture;
    }

    const bool     current_is_reference = input->has_slice_header ? input->first_nal_ref_idc != 0 : input->pic->pic_fields.bits.reference_pic_flag != 0;
    const bool     current_is_idr       = input->has_slice_header ? input->first_nal_unit_type == 5 : bitstream_has_idr(input);
    const uint8_t  current_pps_id       = static_cast<uint8_t>(std::min<uint32_t>(input->pic_parameter_set_id, 255));
    const uint16_t current_frame_num    = input->has_slice_header ? input->parsed_frame_num : input->pic->frame_num;
    const uint16_t current_idr_pic_id   = static_cast<uint16_t>(std::min<uint32_t>(input->idr_pic_id, 65535));
    const int32_t  current_top_poc      = input->pic->CurrPic.TopFieldOrderCnt;
    const int32_t  current_bottom_poc   = input->pic->CurrPic.BottomFieldOrderCnt;
    int            target_dpb_slot      = h264_dpb_slot_for_surface(session, target_surface_id);
    if (target_dpb_slot < 0 || target_dpb_slot >= static_cast<int>(max_h264_dpb_slots) || used_slots[target_dpb_slot]) {
        target_dpb_slot = allocate_dpb_slot(session, used_slots);
        if (target_dpb_slot < 0) {
            std::snprintf(reason, reason_size, "no free H.264 DPB slot for current picture");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        h264_set_dpb_slot_for_surface(session, target_surface_id, target_dpb_slot);
    }
    used_slots[target_dpb_slot] = true;

    H264StdParameters std_params{};
    build_h264_std_parameters(session, profile, input, &std_params);

    const bool                  use_inline_parameters = runtime->video_maintenance2;
    VkVideoSessionParametersKHR parameters            = VK_NULL_HANDLE;
    if (!use_inline_parameters && !create_h264_session_parameters(runtime, session, &std_params, &parameters, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!session->video.initialized) {
        VkVideoSessionParametersKHR reset_parameters         = VK_NULL_HANDLE;
        bool                        created_reset_parameters = false;
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

    VAStatus capacity_status = ensure_command_slot_capacity(runtime, "H.264 decode", reason, reason_size);
    if (capacity_status != VA_STATUS_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return capacity_status;
    }

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    UploadBuffer* upload = &session->uploads[runtime->active_command_slot];
    if (!ensure_bitstream_upload_buffer(runtime, h264_profile_spec, input->bitstream, input->bitstream_size, session->bitstream_size_alignment,
                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, upload, "H.264 bitstream", reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkResetFences", "H.264 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkResetCommandBuffer", "H.264 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkBeginCommandBuffer", "H.264 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    auto*                              target_resource = static_cast<SurfaceResource*>(target->vulkan);
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(reference_count + 1);
    for (uint32_t i = 0; i < reference_count; i++) {
        add_image_layout_barrier(&barriers, references[i].resource, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR);
    }
    add_image_layout_barrier(&barriers, target_resource, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers    = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }

    StdVideoDecodeH264ReferenceInfo    setup_std_ref{};
    VkVideoDecodeH264DpbSlotInfoKHR    setup_h264_slot{};
    VkVideoReferenceSlotInfoKHR        setup_slot{};
    VkVideoPictureResourceInfoKHR      setup_picture{};
    const VkVideoReferenceSlotInfoKHR* setup_slot_ptr = nullptr;
    setup_picture                                     = make_picture_resource(target_resource, coded_extent);
    fill_reference_info(input->pic->CurrPic, current_frame_num, &setup_std_ref);
    setup_std_ref.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_TOP]    = current_top_poc;
    setup_std_ref.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_BOTTOM] = current_bottom_poc;
    setup_h264_slot.sType                                                     = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR;
    setup_h264_slot.pStdReferenceInfo                                         = &setup_std_ref;
    setup_slot.sType                                                          = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    setup_slot.pNext                                                          = &setup_h264_slot;
    setup_slot.slotIndex                                                      = target_dpb_slot;
    setup_slot.pPictureResource                                               = &setup_picture;
    setup_slot_ptr                                                            = &setup_slot;

    std::array<VkVideoReferenceSlotInfoKHR, max_h264_dpb_slots> begin_reference_slots{};
    uint32_t                                                    begin_reference_count = 0;
    for (uint32_t i = 0; i < reference_count; i++) {
        begin_reference_slots[begin_reference_count++] = references[i].slot;
    }
    if (setup_slot_ptr != nullptr && begin_reference_count < begin_reference_slots.size()) {
        begin_reference_slots[begin_reference_count]           = *setup_slot_ptr;
        begin_reference_slots[begin_reference_count].slotIndex = -1;
        begin_reference_count++;
    }

    VkVideoBeginCodingInfoKHR video_begin{};
    video_begin.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession           = session->video.session;
    video_begin.videoSessionParameters = parameters;
    video_begin.referenceSlotCount     = begin_reference_count;
    video_begin.pReferenceSlots        = begin_reference_count > 0 ? begin_reference_slots.data() : nullptr;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    StdVideoDecodeH264PictureInfo std_picture{};
    std_picture.flags.field_pic_flag           = input->pic->pic_fields.bits.field_pic_flag;
    std_picture.flags.is_intra                 = input->all_slices_intra;
    std_picture.flags.IdrPicFlag               = current_is_idr;
    std_picture.flags.bottom_field_flag        = (input->pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    std_picture.flags.is_reference             = current_is_reference;
    std_picture.flags.complementary_field_pair = (input->pic->CurrPic.flags & VA_PICTURE_H264_TOP_FIELD) != 0 && (input->pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD) != 0;
    std_picture.seq_parameter_set_id           = 0;
    std_picture.pic_parameter_set_id           = current_pps_id;
    std_picture.frame_num                      = current_frame_num;
    std_picture.idr_pic_id                     = current_idr_pic_id;
    std_picture.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_TOP]    = current_top_poc;
    std_picture.PicOrderCnt[STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_BOTTOM] = current_bottom_poc;

    VkVideoDecodeH264PictureInfoKHR h264_picture{};
    h264_picture.sType           = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR;
    h264_picture.pStdPictureInfo = &std_picture;
    h264_picture.sliceCount      = input->slice_count;
    h264_picture.pSliceOffsets   = input->slice_offsets;
    VkVideoDecodeH264InlineSessionParametersInfoKHR inline_parameters{};
    if (use_inline_parameters) {
        inline_parameters.sType   = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_INLINE_SESSION_PARAMETERS_INFO_KHR;
        inline_parameters.pStdSPS = &std_params.sps;
        inline_parameters.pStdPPS = &std_params.pps;
        h264_picture.pNext        = &inline_parameters;
    }

    VkVideoPictureResourceInfoKHR                               dst_picture = make_picture_resource(target_resource, coded_extent);

    std::array<VkVideoReferenceSlotInfoKHR, max_h264_dpb_slots> decode_reference_slots{};
    for (uint32_t i = 0; i < reference_count; i++) {
        decode_reference_slots[i] = references[i].slot;
    }

    VkVideoDecodeInfoKHR decode_info{};
    decode_info.sType               = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext               = &h264_picture;
    decode_info.srcBuffer           = upload->buffer;
    decode_info.srcBufferOffset     = 0;
    decode_info.srcBufferRange      = upload->size;
    decode_info.dstPictureResource  = dst_picture;
    decode_info.pSetupReferenceSlot = setup_slot_ptr;
    decode_info.referenceSlotCount  = reference_count;
    decode_info.pReferenceSlots     = reference_count > 0 ? decode_reference_slots.data() : nullptr;
    runtime->cmd_decode_video(runtime->command_buffer, &decode_info);

    VkVideoEndCodingInfoKHR video_end{};
    video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        if (result == VK_ERROR_DEVICE_LOST) {
            record_vk_result(runtime, result, "vkEndCommandBuffer", "H.264 decode", reason, reason_size);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        const VASliceParameterBufferH264* first_slice = (input->last_slices != nullptr && input->last_slice_count > 0) ? &input->last_slices[0] : nullptr;
        std::snprintf(
            reason, reason_size,
            "vkEndCommandBuffer for H.264 decode failed: %d vaProfile=%d nal=%u/%u ref=%u setup=%u slot=%d pps=%u frame=%u idr=%u refs=%u off0=%u bytes=%02x%02x%02x%02x seq=%08x "
            "pic=%08x curr(flags=%08x idx=%u rawpoc=%d/%d poc=%d/%d) s0(type=%u bit=%u l0=%u l1=%u) sps(profile=%d level=%d refs=%u poc=%d log2poc=%u) pps(l0=%u l1=%u)",
            result, profile, input->first_nal_unit_type, input->first_nal_ref_idc, current_is_reference, setup_slot_ptr != nullptr, target_dpb_slot,
            std_picture.pic_parameter_set_id, std_picture.frame_num, std_picture.idr_pic_id, reference_count, input->slice_count > 0 ? input->slice_offsets[0] : 0,
            input->bitstream_size > 0 ? input->bitstream[0] : 0, input->bitstream_size > 1 ? input->bitstream[1] : 0, input->bitstream_size > 2 ? input->bitstream[2] : 0,
            input->bitstream_size > 3 ? input->bitstream[3] : 0, input->pic->seq_fields.value, input->pic->pic_fields.value, input->pic->CurrPic.flags,
            input->pic->CurrPic.frame_idx, input->pic->CurrPic.TopFieldOrderCnt, input->pic->CurrPic.BottomFieldOrderCnt, current_top_poc, current_bottom_poc,
            first_slice != nullptr ? first_slice->slice_type : 0, first_slice != nullptr ? first_slice->slice_data_bit_offset : 0,
            first_slice != nullptr ? first_slice->num_ref_idx_l0_active_minus1 : 0, first_slice != nullptr ? first_slice->num_ref_idx_l1_active_minus1 : 0,
            std_params.sps.profile_idc, std_params.sps.level_idc, std_params.sps.max_num_ref_frames, std_params.sps.pic_order_cnt_type,
            std_params.sps.log2_max_pic_order_cnt_lsb_minus4, std_params.pps.num_ref_idx_l0_default_active_minus1, std_params.pps.num_ref_idx_l1_default_active_minus1);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VkDeviceSize upload_allocation_size = upload->allocation_size;
    const bool         submitted              = submit_command_buffer(runtime, reason, reason_size, "H.264 decode");
    if (!submitted) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    track_pending_decode(runtime, target, parameters, upload_allocation_size, true, "H.264 decode");
    std::snprintf(reason, reason_size, "submitted async H.264 Vulkan decode: %ux%u slices=%u bytes=%zu refs=%u slot=%d decode_mem=%llu upload_mem=%llu session_mem=%llu",
                  coded_extent.width, coded_extent.height, input->slice_count, input->bitstream_size, reference_count, target_dpb_slot,
                  static_cast<unsigned long long>(target_resource->allocation_size), static_cast<unsigned long long>(upload_allocation_size),
                  static_cast<unsigned long long>(session->video.memory_bytes));
    return VA_STATUS_SUCCESS;
}
