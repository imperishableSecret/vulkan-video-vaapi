#include "internal.h"
#include "api.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

using namespace vkvv;

namespace {

    uint8_t vp9_ref_frame_sign_bias_mask(const VADecPictureParameterBufferVP9* pic) {
        uint8_t mask = 0;
        if (pic->pic_fields.bits.last_ref_frame_sign_bias) {
            mask |= 1u << STD_VIDEO_VP9_REFERENCE_NAME_LAST_FRAME;
        }
        if (pic->pic_fields.bits.golden_ref_frame_sign_bias) {
            mask |= 1u << STD_VIDEO_VP9_REFERENCE_NAME_GOLDEN_FRAME;
        }
        if (pic->pic_fields.bits.alt_ref_frame_sign_bias) {
            mask |= 1u << STD_VIDEO_VP9_REFERENCE_NAME_ALTREF_FRAME;
        }
        return mask;
    }

    StdVideoVP9InterpolationFilter vp9_interpolation_filter(uint32_t filter) {
        switch (filter) {
            case STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP:
            case STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SMOOTH:
            case STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SHARP:
            case STD_VIDEO_VP9_INTERPOLATION_FILTER_BILINEAR:
            case STD_VIDEO_VP9_INTERPOLATION_FILTER_SWITCHABLE: return static_cast<StdVideoVP9InterpolationFilter>(filter);
            default: return STD_VIDEO_VP9_INTERPOLATION_FILTER_SWITCHABLE;
        }
    }

    StdVideoVP9ColorSpace vp9_color_space(uint8_t color_space) {
        if (color_space <= STD_VIDEO_VP9_COLOR_SPACE_RGB) {
            return static_cast<StdVideoVP9ColorSpace>(color_space);
        }
        return STD_VIDEO_VP9_COLOR_SPACE_UNKNOWN;
    }

} // namespace

VAStatus vkvv_vulkan_decode_vp9(void* runtime_ptr, void* session_ptr, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvVP9DecodeInput* input,
                                char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<VP9VideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr || drv == nullptr || vctx == nullptr || target == nullptr || input == nullptr || input->pic == nullptr ||
        input->slice == nullptr) {
        std::snprintf(reason, reason_size, "missing VP9 decode state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (profile != session->va_profile || input->pic->profile != session->bitstream_profile || input->pic->bit_depth != session->bit_depth) {
        std::snprintf(reason, reason_size, "VP9 Vulkan profile mismatch: context=%d session=%d va_profile=%u va_depth=%u expected_profile=%u expected_depth=%u", profile,
                      session->va_profile, input->pic->profile, input->pic->bit_depth, session->bitstream_profile, session->bit_depth);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (target->rt_format != session->va_rt_format || target->fourcc != session->va_fourcc) {
        std::snprintf(reason, reason_size, "VP9 target surface format mismatch: rt=0x%x fourcc=0x%x expected_rt=0x%x expected_fourcc=0x%x", target->rt_format, target->fourcc,
                      session->va_rt_format, session->va_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (session->video.session == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "missing VP9 video session");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (input->bitstream == nullptr || input->bitstream_size == 0 || !input->header.valid) {
        std::snprintf(reason, reason_size, "missing VP9 bitstream/header data");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if ((session->decode_flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) == 0) {
        std::snprintf(reason, reason_size, "VP9 decode requires coincident DPB/output support in this prototype");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    const VkExtent2D coded_extent = {
        round_up_16(input->pic->frame_width),
        round_up_16(input->pic->frame_height),
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
        std::snprintf(reason, reason_size, "missing VP9 target surface id");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    bool used_slots[max_vp9_dpb_slots] = {};

    struct ReferenceRecord {
        VkvvSurface*                  surface  = nullptr;
        SurfaceResource*              resource = nullptr;
        VkVideoPictureResourceInfoKHR picture{};
        VkVideoReferenceSlotInfoKHR   slot{};
    };

    std::array<ReferenceRecord, max_vp9_active_references> references{};
    std::array<bool, max_vp9_dpb_slots>                    referenced_dpb_slots{};
    uint32_t                                               reference_count                                                        = 0;
    int32_t                                                reference_name_slot_indices[VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR] = {-1, -1, -1};

    const bool                                             frame_is_inter = input->header.frame_type != 0 && input->pic->pic_fields.bits.intra_only == 0;
    const uint32_t                                         active_reference_indices[VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR] = {
        input->pic->pic_fields.bits.last_ref_frame,
        input->pic->pic_fields.bits.golden_ref_frame,
        input->pic->pic_fields.bits.alt_ref_frame,
    };

    if (frame_is_inter) {
        for (uint32_t i = 0; i < VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR; i++) {
            const uint32_t reference_index = active_reference_indices[i];
            if (reference_index >= max_vp9_reference_slots) {
                std::snprintf(reason, reason_size, "VP9 active reference index %u is invalid", reference_index);
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }

            const VASurfaceID ref_surface_id = input->pic->reference_frames[reference_index];
            auto*             ref_surface    = static_cast<VkvvSurface*>(vkvv_object_get(drv, ref_surface_id, VKVV_OBJECT_SURFACE));
            if (ref_surface == nullptr) {
                std::snprintf(reason, reason_size, "VP9 reference surface %u is missing", ref_surface_id);
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }
            VAStatus ref_status = complete_pending_surface_work_if_needed(runtime, ref_surface, "VP9 reference", reason, reason_size);
            if (ref_status != VA_STATUS_SUCCESS) {
                return ref_status;
            }

            int ref_dpb_slot = vp9_dpb_slot_for_reference_index(session, reference_index);
            if (ref_dpb_slot < 0 || session->reference_slots[reference_index].surface_id != ref_surface_id) {
                ref_dpb_slot = vp9_dpb_slot_for_surface(session, ref_surface_id);
                if (ref_dpb_slot >= 0) {
                    vp9_set_reference_slot(session, reference_index, ref_surface_id, ref_dpb_slot);
                }
            }
            if (!ref_surface->decoded || ref_surface->vulkan == nullptr || ref_dpb_slot < 0) {
                std::snprintf(reason, reason_size, "VP9 reference surface %u is not decoded yet", ref_surface_id);
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }
            if (static_cast<uint32_t>(ref_dpb_slot) >= max_vp9_dpb_slots) {
                std::snprintf(reason, reason_size, "VP9 reference surface %u has invalid DPB slot", ref_surface_id);
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }

            reference_name_slot_indices[i] = ref_dpb_slot;
            used_slots[ref_dpb_slot]       = true;
            if (referenced_dpb_slots[ref_dpb_slot]) {
                continue;
            }

            auto*            resource          = static_cast<SurfaceResource*>(ref_surface->vulkan);
            ReferenceRecord& record            = references[reference_count++];
            record.surface                     = ref_surface;
            record.resource                    = resource;
            record.picture                     = make_picture_resource(resource, resource->extent);
            record.slot.sType                  = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            record.slot.slotIndex              = ref_dpb_slot;
            record.slot.pPictureResource       = &record.picture;
            referenced_dpb_slots[ref_dpb_slot] = true;
        }
    }

    const bool current_is_reference = input->header.refresh_frame_flags != 0;
    int        target_dpb_slot      = current_is_reference ? vp9_dpb_slot_for_surface(session, target_surface_id) : -1;
    if (current_is_reference && (target_dpb_slot < 0 || target_dpb_slot >= static_cast<int>(max_vp9_dpb_slots) || used_slots[target_dpb_slot])) {
        target_dpb_slot = allocate_vp9_dpb_slot(session, used_slots);
        if (target_dpb_slot < 0) {
            std::snprintf(reason, reason_size, "no free VP9 DPB slot for current picture");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    if (current_is_reference) {
        used_slots[target_dpb_slot] = true;
    }

    if (!session->video.initialized && !reset_vp9_session(runtime, session, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VAStatus capacity_status = ensure_command_slot_capacity(runtime, "VP9 decode", reason, reason_size);
    if (capacity_status != VA_STATUS_SUCCESS) {
        return capacity_status;
    }

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    UploadBuffer* upload = &session->uploads[runtime->active_command_slot];
    if (!ensure_bitstream_upload_buffer(runtime, session->profile_spec, input->bitstream, input->bitstream_size, session->bitstream_size_alignment,
                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, upload, "VP9 bitstream", reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        record_vk_result(runtime, result, "vkResetFences", "VP9 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        record_vk_result(runtime, result, "vkResetCommandBuffer", "VP9 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        record_vk_result(runtime, result, "vkBeginCommandBuffer", "VP9 decode", reason, reason_size);
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

    VkVideoReferenceSlotInfoKHR        setup_slot{};
    VkVideoPictureResourceInfoKHR      setup_picture{};
    const VkVideoReferenceSlotInfoKHR* setup_slot_ptr = nullptr;
    if (current_is_reference) {
        setup_picture               = make_picture_resource(target_resource, coded_extent);
        setup_slot.sType            = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        setup_slot.slotIndex        = target_dpb_slot;
        setup_slot.pPictureResource = &setup_picture;
        setup_slot_ptr              = &setup_slot;
    }

    std::array<VkVideoReferenceSlotInfoKHR, max_vp9_dpb_slots> begin_reference_slots{};
    uint32_t                                                   begin_reference_count = 0;
    for (uint32_t i = 0; i < reference_count; i++) {
        begin_reference_slots[begin_reference_count++] = references[i].slot;
    }
    if (setup_slot_ptr != nullptr && begin_reference_count < begin_reference_slots.size()) {
        begin_reference_slots[begin_reference_count]           = *setup_slot_ptr;
        begin_reference_slots[begin_reference_count].slotIndex = -1;
        begin_reference_count++;
    }

    VkVideoBeginCodingInfoKHR video_begin{};
    video_begin.sType              = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession       = session->video.session;
    video_begin.referenceSlotCount = begin_reference_count;
    video_begin.pReferenceSlots    = begin_reference_count > 0 ? begin_reference_slots.data() : nullptr;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    StdVideoVP9ColorConfig color{};
    color.flags.color_range = input->header.color_range;
    color.BitDepth          = input->header.bit_depth;
    color.subsampling_x     = input->header.subsampling_x;
    color.subsampling_y     = input->header.subsampling_y;
    color.color_space       = vp9_color_space(input->header.color_space);

    StdVideoVP9LoopFilter loop_filter{};
    loop_filter.flags.loop_filter_delta_enabled = input->header.loop_filter_delta_enabled;
    loop_filter.flags.loop_filter_delta_update  = input->header.loop_filter_delta_update;
    loop_filter.loop_filter_level               = input->pic->filter_level;
    loop_filter.loop_filter_sharpness           = input->pic->sharpness_level;
    std::memcpy(loop_filter.loop_filter_ref_deltas, input->header.loop_filter_ref_deltas, sizeof(loop_filter.loop_filter_ref_deltas));
    std::memcpy(loop_filter.loop_filter_mode_deltas, input->header.loop_filter_mode_deltas, sizeof(loop_filter.loop_filter_mode_deltas));

    StdVideoVP9Segmentation segmentation{};
    segmentation.flags.segmentation_update_map          = input->pic->pic_fields.bits.segmentation_update_map;
    segmentation.flags.segmentation_temporal_update     = input->pic->pic_fields.bits.segmentation_temporal_update;
    segmentation.flags.segmentation_update_data         = input->header.segmentation_update_data;
    segmentation.flags.segmentation_abs_or_delta_update = input->header.segmentation_abs_or_delta_update;
    std::memcpy(segmentation.segmentation_tree_probs, input->pic->mb_segment_tree_probs, sizeof(segmentation.segmentation_tree_probs));
    std::memcpy(segmentation.segmentation_pred_prob, input->pic->segment_pred_probs, sizeof(segmentation.segmentation_pred_prob));
    std::memcpy(segmentation.FeatureEnabled, input->header.segment_feature_enabled, sizeof(segmentation.FeatureEnabled));
    std::memcpy(segmentation.FeatureData, input->header.segment_feature_data, sizeof(segmentation.FeatureData));

    StdVideoDecodeVP9PictureInfo std_picture{};
    std_picture.flags.error_resilient_mode         = input->pic->pic_fields.bits.error_resilient_mode;
    std_picture.flags.intra_only                   = input->pic->pic_fields.bits.intra_only;
    std_picture.flags.allow_high_precision_mv      = input->pic->pic_fields.bits.allow_high_precision_mv;
    std_picture.flags.refresh_frame_context        = input->pic->pic_fields.bits.refresh_frame_context;
    std_picture.flags.frame_parallel_decoding_mode = input->pic->pic_fields.bits.frame_parallel_decoding_mode;
    std_picture.flags.segmentation_enabled         = input->pic->pic_fields.bits.segmentation_enabled;
    std_picture.flags.show_frame                   = input->header.show_frame;
    std_picture.flags.UsePrevFrameMvs              = input->header.use_prev_frame_mvs;
    std_picture.profile                            = static_cast<StdVideoVP9Profile>(session->profile_spec.std_profile);
    std_picture.frame_type                         = input->header.frame_type ? STD_VIDEO_VP9_FRAME_TYPE_NON_KEY : STD_VIDEO_VP9_FRAME_TYPE_KEY;
    std_picture.frame_context_idx                  = input->pic->pic_fields.bits.frame_context_idx;
    std_picture.reset_frame_context                = input->pic->pic_fields.bits.reset_frame_context;
    std_picture.refresh_frame_flags                = input->header.refresh_frame_flags;
    std_picture.ref_frame_sign_bias_mask           = vp9_ref_frame_sign_bias_mask(input->pic);
    std_picture.interpolation_filter               = vp9_interpolation_filter(input->header.interpolation_filter);
    std_picture.base_q_idx                         = input->header.base_q_idx;
    std_picture.delta_q_y_dc                       = input->header.delta_q_y_dc;
    std_picture.delta_q_uv_dc                      = input->header.delta_q_uv_dc;
    std_picture.delta_q_uv_ac                      = input->header.delta_q_uv_ac;
    std_picture.tile_cols_log2                     = input->pic->log2_tile_columns;
    std_picture.tile_rows_log2                     = input->pic->log2_tile_rows;
    std_picture.pColorConfig                       = &color;
    std_picture.pLoopFilter                        = &loop_filter;
    std_picture.pSegmentation                      = &segmentation;

    VkVideoDecodeVP9PictureInfoKHR vp9_picture{};
    vp9_picture.sType           = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_KHR;
    vp9_picture.pStdPictureInfo = &std_picture;
    std::memcpy(vp9_picture.referenceNameSlotIndices, reference_name_slot_indices, sizeof(vp9_picture.referenceNameSlotIndices));
    vp9_picture.uncompressedHeaderOffset = 0;
    vp9_picture.compressedHeaderOffset   = input->header.frame_header_length_in_bytes;
    vp9_picture.tilesOffset              = input->header.frame_header_length_in_bytes + input->header.first_partition_size;

    VkVideoPictureResourceInfoKHR                                      dst_picture = make_picture_resource(target_resource, coded_extent);

    std::array<VkVideoReferenceSlotInfoKHR, max_vp9_active_references> decode_reference_slots{};
    for (uint32_t i = 0; i < reference_count; i++) {
        decode_reference_slots[i] = references[i].slot;
    }

    VkVideoDecodeInfoKHR decode_info{};
    decode_info.sType               = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext               = &vp9_picture;
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
        if (result == VK_ERROR_DEVICE_LOST) {
            record_vk_result(runtime, result, "vkEndCommandBuffer", "VP9 decode", reason, reason_size);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        std::snprintf(reason, reason_size, "vkEndCommandBuffer for VP9 decode failed: %d frame=%u refs=%u setup=%u slot=%d refresh=0x%02x q=%u offsets=%u/%u/%u bytes=%zu", result,
                      input->header.frame_type, reference_count, setup_slot_ptr != nullptr, target_dpb_slot, input->header.refresh_frame_flags, input->header.base_q_idx,
                      vp9_picture.uncompressedHeaderOffset, vp9_picture.compressedHeaderOffset, vp9_picture.tilesOffset, input->bitstream_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VkDeviceSize upload_allocation_size = upload->allocation_size;
    const bool         submitted              = submit_command_buffer(runtime, reason, reason_size, "VP9 decode");
    if (!submitted) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (current_is_reference) {
        for (uint32_t i = 0; i < max_vp9_reference_slots; i++) {
            if ((input->header.refresh_frame_flags & (1u << i)) != 0) {
                vp9_set_reference_slot(session, i, target_surface_id, target_dpb_slot);
            }
        }
    }

    track_pending_decode(runtime, target, VK_NULL_HANDLE, upload_allocation_size, input->header.show_frame != 0, "VP9 decode");
    std::snprintf(reason, reason_size, "submitted async VP9 Vulkan decode: %ux%u bytes=%zu refs=%u slot=%d refresh=0x%02x decode_mem=%llu upload_mem=%llu session_mem=%llu",
                  coded_extent.width, coded_extent.height, input->bitstream_size, reference_count, target_dpb_slot, input->header.refresh_frame_flags,
                  static_cast<unsigned long long>(target_resource->allocation_size), static_cast<unsigned long long>(upload_allocation_size),
                  static_cast<unsigned long long>(session->video.memory_bytes));
    return VA_STATUS_SUCCESS;
}
