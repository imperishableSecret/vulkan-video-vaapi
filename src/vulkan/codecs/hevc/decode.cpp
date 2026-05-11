#include "internal.h"
#include "api.h"
#include "telemetry.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace vkvv;

namespace {

    void mark_slice_references(const VASliceParameterBufferHEVC& slice, bool active_refs[max_va_hevc_reference_frames], uint32_t* unique_count, uint32_t* l0_count,
                               uint32_t* l1_count) {
        auto mark_ref = [&](uint8_t ref_index, uint32_t* list_count) {
            if (ref_index >= max_va_hevc_reference_frames) {
                return;
            }
            if (list_count != nullptr) {
                (*list_count)++;
            }
            if (!active_refs[ref_index]) {
                active_refs[ref_index] = true;
                if (unique_count != nullptr) {
                    (*unique_count)++;
                }
            }
        };

        const uint32_t l0_entries = std::min<uint32_t>(static_cast<uint32_t>(slice.num_ref_idx_l0_active_minus1) + 1U, max_va_hevc_reference_frames);
        const uint32_t l1_entries = std::min<uint32_t>(static_cast<uint32_t>(slice.num_ref_idx_l1_active_minus1) + 1U, max_va_hevc_reference_frames);
        for (uint32_t i = 0; i < l0_entries; i++) {
            mark_ref(slice.RefPicList[0][i], l0_count);
        }
        for (uint32_t i = 0; i < l1_entries; i++) {
            mark_ref(slice.RefPicList[1][i], l1_count);
        }
    }

} // namespace

VAStatus vkvv_vulkan_decode_hevc(void* runtime_ptr, void* session_ptr, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvHEVCDecodeInput* input,
                                 char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<HEVCVideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr || drv == nullptr || vctx == nullptr || target == nullptr || input == nullptr || input->pic == nullptr) {
        std::snprintf(reason, reason_size, "missing HEVC decode state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (profile != session->va_profile) {
        std::snprintf(reason, reason_size, "HEVC Vulkan profile mismatch: context=%d session=%d", profile, session->va_profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (target->rt_format != session->va_rt_format || target->fourcc != session->va_fourcc) {
        std::snprintf(reason, reason_size, "HEVC target surface format mismatch: rt=0x%x fourcc=0x%x expected_rt=0x%x expected_fourcc=0x%x", target->rt_format, target->fourcc,
                      session->va_rt_format, session->va_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (session->video.session == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "missing HEVC video session");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (input->bitstream == nullptr || input->bitstream_size == 0 || input->slice_offsets == nullptr || input->slice_count == 0) {
        std::snprintf(reason, reason_size, "missing HEVC bitstream data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    const uint8_t expected_bit_depth_minus8 = session->bit_depth > 8 ? static_cast<uint8_t>(session->bit_depth - 8U) : 0;
    if (input->pic->bit_depth_luma_minus8 != expected_bit_depth_minus8 || input->pic->bit_depth_chroma_minus8 != expected_bit_depth_minus8) {
        std::snprintf(reason, reason_size, "HEVC Vulkan bit-depth mismatch: luma_minus8=%u chroma_minus8=%u expected=%u profile=%d", input->pic->bit_depth_luma_minus8,
                      input->pic->bit_depth_chroma_minus8, expected_bit_depth_minus8, profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (input->pic->pic_fields.bits.chroma_format_idc != 1) {
        std::snprintf(reason, reason_size, "HEVC Vulkan path currently supports only 4:2:0");
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if ((input->pic->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) != 0) {
        std::snprintf(reason, reason_size, "HEVC Vulkan path currently supports only progressive pictures");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if ((session->decode_flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) == 0) {
        std::snprintf(reason, reason_size, "HEVC decode requires coincident DPB/output support");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    const VkExtent2D coded_extent = {
        round_up_16(input->pic->pic_width_in_luma_samples),
        round_up_16(input->pic->pic_height_in_luma_samples),
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
        std::snprintf(reason, reason_size, "missing HEVC target surface id");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    bool used_slots[max_hevc_dpb_slots] = {};

    struct ReferenceRecord {
        uint32_t                        ref_index = 0;
        VkvvSurface*                    surface   = nullptr;
        SurfaceResource*                resource  = nullptr;
        uint8_t                         slot      = STD_VIDEO_H265_NO_REFERENCE_PICTURE;
        bool                            active    = false;
        VkVideoPictureResourceInfoKHR   picture{};
        StdVideoDecodeH265ReferenceInfo std_ref{};
        VkVideoDecodeH265DpbSlotInfoKHR h265_slot{};
        VkVideoReferenceSlotInfoKHR     slot_info{};
    };

    std::array<ReferenceRecord, max_va_hevc_reference_frames> references{};
    uint32_t                                                  reference_count = 0;
    for (uint32_t i = 0; i < max_va_hevc_reference_frames; i++) {
        const VAPictureHEVC& ref_pic = input->pic->ReferenceFrames[i];
        if (hevc_picture_is_invalid(ref_pic)) {
            continue;
        }

        auto* ref_surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, ref_pic.picture_id, VKVV_OBJECT_SURFACE));
        if (ref_surface == nullptr) {
            std::snprintf(reason, reason_size, "HEVC reference surface %u is missing", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        VAStatus ref_status = complete_pending_surface_work_if_needed(runtime, ref_surface, "HEVC reference", reason, reason_size);
        if (ref_status != VA_STATUS_SUCCESS) {
            return ref_status;
        }
        const int ref_dpb_slot = hevc_dpb_slot_for_surface(session, ref_pic.picture_id);
        if (!ref_surface->decoded || ref_surface->vulkan == nullptr || ref_dpb_slot < 0) {
            std::snprintf(reason, reason_size, "HEVC reference surface %u is not decoded yet", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (static_cast<uint32_t>(ref_dpb_slot) >= max_hevc_dpb_slots) {
            std::snprintf(reason, reason_size, "HEVC reference surface %u has invalid DPB slot", ref_pic.picture_id);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        used_slots[ref_dpb_slot] = true;

        auto*            resource = static_cast<SurfaceResource*>(ref_surface->vulkan);
        ReferenceRecord& record   = references[reference_count++];
        record.ref_index          = i;
        record.surface            = ref_surface;
        record.resource           = resource;
        record.slot               = static_cast<uint8_t>(ref_dpb_slot);
        record.picture            = make_picture_resource(resource, resource->extent);
        fill_hevc_reference_info(ref_pic, &record.std_ref);
        record.h265_slot.sType             = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
        record.h265_slot.pStdReferenceInfo = &record.std_ref;
        record.slot_info.sType             = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        record.slot_info.pNext             = &record.h265_slot;
        record.slot_info.slotIndex         = ref_dpb_slot;
        record.slot_info.pPictureResource  = &record.picture;
    }

    int target_dpb_slot = hevc_dpb_slot_for_surface(session, target_surface_id);
    if (target_dpb_slot < 0 || target_dpb_slot >= static_cast<int>(max_hevc_dpb_slots) || used_slots[target_dpb_slot]) {
        target_dpb_slot = allocate_hevc_dpb_slot(session, used_slots);
        if (target_dpb_slot < 0) {
            std::snprintf(reason, reason_size, "no free HEVC DPB slot for current picture");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        hevc_set_dpb_slot_for_surface(session, target_surface_id, target_dpb_slot);
    }
    used_slots[target_dpb_slot] = true;

    bool     active_slice_refs[max_va_hevc_reference_frames] = {};
    uint32_t active_slice_ref_count                          = 0;
    uint32_t active_l0_count                                 = 0;
    uint32_t active_l1_count                                 = 0;
    if (input->slices != nullptr && input->slice_count > 0) {
        mark_slice_references(input->slices[0], active_slice_refs, &active_slice_ref_count, &active_l0_count, &active_l1_count);
    }

    HEVCStdParameters std_params{};
    build_hevc_std_parameters(session, profile, input, &std_params);

    const bool                  use_inline_parameters = runtime->video_maintenance2;
    VkVideoSessionParametersKHR parameters            = VK_NULL_HANDLE;
    if (!use_inline_parameters && !create_hevc_session_parameters(runtime, session, &std_params, &parameters, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!session->video.initialized) {
        VkVideoSessionParametersKHR reset_parameters         = VK_NULL_HANDLE;
        bool                        created_reset_parameters = false;
        if (!use_inline_parameters) {
            if (!create_empty_hevc_session_parameters(runtime, session, &reset_parameters, reason, reason_size)) {
                runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            created_reset_parameters = true;
        }
        const bool reset = reset_hevc_session(runtime, session, reset_parameters, reason, reason_size);
        if (created_reset_parameters) {
            runtime->destroy_video_session_parameters(runtime->device, reset_parameters, nullptr);
        }
        if (!reset) {
            runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    VAStatus capacity_status = ensure_command_slot_capacity(runtime, "HEVC decode", reason, reason_size);
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
    if (!ensure_bitstream_upload_buffer(runtime, session->profile_spec, input->bitstream, input->bitstream_size, session->bitstream_size_alignment,
                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, upload, "HEVC bitstream", reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkResetFences", "HEVC decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkResetCommandBuffer", "HEVC decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkBeginCommandBuffer", "HEVC decode", reason, reason_size);
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

    StdVideoDecodeH265ReferenceInfo    setup_std_ref{};
    VkVideoDecodeH265DpbSlotInfoKHR    setup_h265_slot{};
    VkVideoReferenceSlotInfoKHR        setup_slot{};
    VkVideoPictureResourceInfoKHR      setup_picture{};
    const VkVideoReferenceSlotInfoKHR* setup_slot_ptr = nullptr;
    setup_picture                                     = make_picture_resource(target_resource, coded_extent);
    fill_hevc_reference_info(input->pic->CurrPic, &setup_std_ref);
    setup_h265_slot.sType             = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
    setup_h265_slot.pStdReferenceInfo = &setup_std_ref;
    setup_slot.sType                  = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    setup_slot.pNext                  = &setup_h265_slot;
    setup_slot.slotIndex              = target_dpb_slot;
    setup_slot.pPictureResource       = &setup_picture;
    setup_slot_ptr                    = &setup_slot;

    std::array<VkVideoReferenceSlotInfoKHR, max_hevc_dpb_slots> begin_reference_slots{};
    uint32_t                                                    begin_reference_count = 0;
    for (uint32_t i = 0; i < reference_count; i++) {
        begin_reference_slots[begin_reference_count++] = references[i].slot_info;
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

    StdVideoDecodeH265PictureInfo std_picture{};
    std_picture.flags.IrapPicFlag                     = input->pic->slice_parsing_fields.bits.RapPicFlag;
    std_picture.flags.IdrPicFlag                      = input->pic->slice_parsing_fields.bits.IdrPicFlag;
    std_picture.flags.IsReference                     = 1;
    std_picture.flags.short_term_ref_pic_set_sps_flag = 0;
    std_picture.sps_video_parameter_set_id            = 0;
    std_picture.pps_seq_parameter_set_id              = 0;
    std_picture.pps_pic_parameter_set_id              = 0;
    std_picture.NumDeltaPocsOfRefRpsIdx               = 0;
    std_picture.PicOrderCntVal                        = input->pic->CurrPic.pic_order_cnt;
    std_picture.NumBitsForSTRefPicSetInSlice          = static_cast<uint16_t>(std::min<uint32_t>(input->pic->st_rps_bits, 0xffff));

    std::array<HEVCDpbReference, max_va_hevc_reference_frames> rps_references{};
    uint32_t                                                   rps_reference_count = 0;
    for (uint32_t i = 0; i < reference_count; i++) {
        const uint32_t       ref_index = references[i].ref_index;
        const VAPictureHEVC& ref_pic   = input->pic->ReferenceFrames[ref_index];
        if (hevc_picture_is_current_reference(ref_pic)) {
            references[i].active                  = true;
            rps_references[rps_reference_count++] = {
                .slot          = references[i].slot,
                .pic_order_cnt = ref_pic.pic_order_cnt,
                .flags         = ref_pic.flags,
            };
        }
    }
    const HEVCRpsCounts             rps_counts = fill_hevc_picture_rps(rps_references.data(), rps_reference_count, &std_picture);

    VkVideoDecodeH265PictureInfoKHR h265_picture{};
    h265_picture.sType                = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
    h265_picture.pStdPictureInfo      = &std_picture;
    h265_picture.sliceSegmentCount    = input->slice_count;
    h265_picture.pSliceSegmentOffsets = input->slice_offsets;
    VkVideoDecodeH265InlineSessionParametersInfoKHR inline_parameters{};
    if (use_inline_parameters) {
        inline_parameters.sType   = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_INLINE_SESSION_PARAMETERS_INFO_KHR;
        inline_parameters.pStdVPS = &std_params.vps;
        inline_parameters.pStdSPS = &std_params.sps;
        inline_parameters.pStdPPS = &std_params.pps;
        h265_picture.pNext        = &inline_parameters;
    }

    VkVideoPictureResourceInfoKHR                                         dst_picture = make_picture_resource(target_resource, coded_extent);
    std::array<VkVideoReferenceSlotInfoKHR, max_va_hevc_reference_frames> decode_reference_slots{};
    uint32_t                                                              decode_reference_count = 0;
    for (uint32_t i = 0; i < reference_count; i++) {
        if (!references[i].active) {
            continue;
        }
        decode_reference_slots[decode_reference_count++] = references[i].slot_info;
    }

    VkVideoDecodeInfoKHR decode_info{};
    decode_info.sType               = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext               = &h265_picture;
    decode_info.srcBuffer           = upload->buffer;
    decode_info.srcBufferOffset     = 0;
    decode_info.srcBufferRange      = upload->size;
    decode_info.dstPictureResource  = dst_picture;
    decode_info.pSetupReferenceSlot = setup_slot_ptr;
    decode_info.referenceSlotCount  = decode_reference_count;
    decode_info.pReferenceSlots     = decode_reference_count > 0 ? decode_reference_slots.data() : nullptr;
    runtime->cmd_decode_video(runtime->command_buffer, &decode_info);

    VkVideoEndCodingInfoKHR video_end{};
    video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        if (result == VK_ERROR_DEVICE_LOST) {
            record_vk_result(runtime, result, "vkEndCommandBuffer", "HEVC decode", reason, reason_size);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        std::snprintf(reason, reason_size, "vkEndCommandBuffer for HEVC decode failed: %d profile=%d refs=%u active_refs=%u setup=%u slot=%d poc=%d slices=%u bytes=%zu", result,
                      profile, reference_count, decode_reference_count, setup_slot_ptr != nullptr, target_dpb_slot, std_picture.PicOrderCntVal, input->slice_count,
                      input->bitstream_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VkDeviceSize upload_allocation_size = upload->allocation_size;
    const bool         submitted              = submit_command_buffer(runtime, reason, reason_size, "HEVC decode");
    if (!submitted) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    track_pending_decode(runtime, target, parameters, upload_allocation_size, true, "HEVC decode");
    VKVV_SUCCESS_REASON(reason, reason_size,
                        "submitted async HEVC Vulkan decode: %ux%u slices=%u bytes=%zu refs=%u active_refs=%u slot=%d poc=%d st_rps_sps=%u st_rps_bits=%u rps=%u/%u/%u "
                        "rps_refs=%u slice_refs=%u l0=%u l1=%u "
                        "decode_mem=%llu upload_mem=%llu session_mem=%llu",
                        coded_extent.width, coded_extent.height, input->slice_count, input->bitstream_size, reference_count, decode_reference_count, target_dpb_slot,
                        std_picture.PicOrderCntVal, std_picture.flags.short_term_ref_pic_set_sps_flag, input->pic->st_rps_bits, rps_counts.st_curr_before, rps_counts.st_curr_after,
                        rps_counts.lt_curr, rps_reference_count, active_slice_ref_count, active_l0_count, active_l1_count,
                        static_cast<unsigned long long>(target_resource->allocation_size), static_cast<unsigned long long>(upload_allocation_size),
                        static_cast<unsigned long long>(session->video.memory_bytes));
    return VA_STATUS_SUCCESS;
}
