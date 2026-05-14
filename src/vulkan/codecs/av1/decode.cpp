#include "internal.h"
#include "api.h"
#include "telemetry.h"
#include "vulkan/export/internal.h"
#include "vulkan/runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

using namespace vkvv;

namespace {

    bool av1_frame_is_intra(const VADecPictureParameterBufferAV1* pic) {
        return pic->pic_info_fields.bits.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY || pic->pic_info_fields.bits.frame_type == STD_VIDEO_AV1_FRAME_TYPE_INTRA_ONLY;
    }

    bool av1_frame_is_switch(const VkvvAV1DecodeInput* input) {
        return input != nullptr && input->pic != nullptr && input->pic->pic_info_fields.bits.frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH;
    }

    uint8_t av1_va_ref_frame_index(const VADecPictureParameterBufferAV1* pic, uint32_t index) {
        if (pic == nullptr || index >= VKVV_AV1_ACTIVE_REFERENCE_COUNT) {
            return max_av1_reference_slots;
        }
        return pic->ref_frame_idx[index];
    }

    uint32_t align_power2(uint32_t value, uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    uint32_t av1_frame_width(const VADecPictureParameterBufferAV1* pic) {
        uint32_t width = static_cast<uint32_t>(pic->frame_width_minus1) + 1;
        if (pic->pic_info_fields.bits.use_superres && pic->superres_scale_denominator != 8) {
            width = static_cast<uint32_t>(((static_cast<uint64_t>(width) * 8U) + (pic->superres_scale_denominator / 2U)) / pic->superres_scale_denominator);
        }
        return width;
    }

    uint32_t av1_superblock_count(uint32_t pixels, bool use_128x128_superblock) {
        constexpr uint32_t mi_size_log2  = 2;
        const uint32_t     mib_size_log2 = use_128x128_superblock ? 5U : 4U;
        const uint32_t     mi_count      = align_power2(align_power2(pixels, 8) >> mi_size_log2, 1U << mib_size_log2);
        return mi_count >> mib_size_log2;
    }

    uint32_t ceil_log2(uint32_t value) {
        uint32_t log2 = 0;
        while ((1U << log2) < value) {
            log2++;
        }
        return log2;
    }

    void fill_uniform_tile_sizes(uint16_t* sizes_minus1, uint32_t tile_count, uint32_t superblock_count) {
        const uint32_t tile_log2 = ceil_log2(tile_count);
        const uint32_t tile_size = (superblock_count + (1U << tile_log2) - 1U) >> tile_log2;
        uint32_t       remaining = superblock_count;
        for (uint32_t i = 0; i < tile_count; i++) {
            const uint32_t size = (i == tile_count - 1U) ? remaining : std::min(tile_size, remaining);
            sizes_minus1[i]     = static_cast<uint16_t>(size > 0 ? size - 1U : 0);
            remaining -= std::min(size, remaining);
        }
    }

    void fill_explicit_tile_sizes(uint16_t* sizes_minus1, const uint16_t* va_sizes_minus1, uint32_t tile_count, uint32_t superblock_count) {
        uint32_t consumed = 0;
        for (uint32_t i = 0; i < tile_count; i++) {
            uint32_t size = 1;
            if (i + 1 < tile_count) {
                size = static_cast<uint32_t>(va_sizes_minus1[i]) + 1U;
            } else if (superblock_count > consumed) {
                size = superblock_count - consumed;
            }
            sizes_minus1[i] = static_cast<uint16_t>(size > 0 ? size - 1U : 0);
            consumed += size;
        }
    }

    StdVideoAV1FrameType av1_effective_frame_type(uint8_t frame_type) {
        if (frame_type == STD_VIDEO_AV1_FRAME_TYPE_SWITCH) {
            return STD_VIDEO_AV1_FRAME_TYPE_INTER;
        }
        if (frame_type <= STD_VIDEO_AV1_FRAME_TYPE_SWITCH) {
            return static_cast<StdVideoAV1FrameType>(frame_type);
        }
        return STD_VIDEO_AV1_FRAME_TYPE_KEY;
    }

    StdVideoAV1InterpolationFilter av1_interpolation_filter(uint8_t filter) {
        if (filter <= STD_VIDEO_AV1_INTERPOLATION_FILTER_SWITCHABLE) {
            return static_cast<StdVideoAV1InterpolationFilter>(filter);
        }
        return STD_VIDEO_AV1_INTERPOLATION_FILTER_SWITCHABLE;
    }

    StdVideoAV1TxMode av1_tx_mode(uint8_t mode) {
        if (mode <= STD_VIDEO_AV1_TX_MODE_SELECT) {
            return static_cast<StdVideoAV1TxMode>(mode);
        }
        return STD_VIDEO_AV1_TX_MODE_SELECT;
    }

    StdVideoAV1FrameRestorationType av1_restoration_type(uint32_t type) {
        if (type <= STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_SWITCHABLE) {
            return static_cast<StdVideoAV1FrameRestorationType>(type);
        }
        return STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE;
    }

    bool av1_restoration_size_code(uint32_t type, uint8_t lr_unit_shift, bool chroma, bool lr_uv_shift, uint16_t* code, char* reason, size_t reason_size) {
        if (code == nullptr) {
            std::snprintf(reason, reason_size, "missing AV1 restoration size output");
            return false;
        }
        if (type == 0) {
            *code = 0;
            return true;
        }
        uint32_t shift = 6U + lr_unit_shift;
        if (chroma && lr_uv_shift && shift > 0) {
            shift--;
        }
        const uint32_t size = 1U << shift;
        switch (size) {
            case 32:
            case 64:
            case 128:
            case 256: *code = static_cast<uint16_t>(shift - 5U); return true;
            default:
                std::snprintf(reason, reason_size, "invalid AV1 loop restoration size: type=%u size=%u shift=%u chroma=%u uv_shift=%u", type, size, lr_unit_shift, chroma ? 1U : 0U,
                              lr_uv_shift ? 1U : 0U);
                return false;
        }
    }

    uint8_t av1_ref_frame_sign_bias_mask(const VkvvAV1FrameHeader& header) {
        uint8_t mask = 0;
        for (uint32_t i = 0; i < VKVV_AV1_REFERENCE_COUNT; i++) {
            if (header.ref_frame_sign_bias[i]) {
                mask |= static_cast<uint8_t>(1U << i);
            }
        }
        return mask;
    }

    void add_av1_copy_barrier(std::vector<VkImageMemoryBarrier2>* barriers, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        if (barriers == nullptr || old_layout == new_layout) {
            return;
        }

        VkImageMemoryBarrier2 barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask                    = src_stage;
        barrier.srcAccessMask                   = src_access;
        barrier.dstStageMask                    = dst_stage;
        barrier.dstAccessMask                   = dst_access;
        barrier.oldLayout                       = old_layout;
        barrier.newLayout                       = new_layout;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barriers->push_back(barrier);
    }

    bool copy_av1_show_existing_surface(VulkanRuntime* runtime, SurfaceResource* source, SurfaceResource* target, char* reason, size_t reason_size) {
        if (runtime == nullptr || source == nullptr || target == nullptr || source->image == VK_NULL_HANDLE || target->image == VK_NULL_HANDLE) {
            std::snprintf(reason, reason_size, "missing AV1 show-existing copy resources");
            return false;
        }
        if (source == target) {
            return true;
        }
        if (source->format != target->format || source->va_fourcc != target->va_fourcc || source->coded_extent.width < target->coded_extent.width ||
            source->coded_extent.height < target->coded_extent.height) {
            std::snprintf(reason, reason_size, "AV1 show-existing copy resource mismatch: source_format=%d target_format=%d source=%ux%u target=%ux%u", source->format,
                          target->format, source->coded_extent.width, source->coded_extent.height, target->coded_extent.width, target->coded_extent.height);
            return false;
        }

        const ExportFormatInfo* format = export_format_for_surface(nullptr, target, reason, reason_size);
        if (format == nullptr) {
            return false;
        }

        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "AV1 show-existing copy", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "AV1 show-existing copy", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "AV1 show-existing copy", reason, reason_size)) {
            return false;
        }

        std::vector<VkImageMemoryBarrier2> barriers;
        add_av1_copy_barrier(&barriers, source->image, source->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_av1_copy_barrier(&barriers, target->image, target->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        target->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        VkImageCopy regions[2]{};
        for (uint32_t i = 0; i < format->layer_count; i++) {
            regions[i].srcSubresource.aspectMask = format->layers[i].aspect;
            regions[i].srcSubresource.layerCount = 1;
            regions[i].dstSubresource.aspectMask = format->layers[i].aspect;
            regions[i].dstSubresource.layerCount = 1;
            regions[i].extent                    = export_layer_extent(target->coded_extent, format->layers[i]);
        }
        vkCmdCopyImage(runtime->command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, format->layer_count,
                       regions);

        barriers.clear();
        add_av1_copy_barrier(&barriers, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                             VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
        add_av1_copy_barrier(&barriers, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                             VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        source->layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        target->layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;

        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "AV1 show-existing copy", reason, reason_size)) {
            return false;
        }
        return submit_command_buffer_and_wait(runtime, reason, reason_size, "AV1 show-existing copy");
    }

    void build_av1_tile_info(const VkvvAV1DecodeInput* input, AV1PictureStdData* std_data) {
        const VADecPictureParameterBufferAV1* pic         = input->pic;
        const uint32_t                        tile_cols   = std::min<uint32_t>(pic->tile_cols, STD_VIDEO_AV1_MAX_TILE_COLS);
        const uint32_t                        tile_rows   = std::min<uint32_t>(pic->tile_rows, STD_VIDEO_AV1_MAX_TILE_ROWS);
        const bool                            use_128x128 = pic->seq_info_fields.fields.use_128x128_superblock != 0;
        const uint32_t                        mib_size    = use_128x128 ? 32U : 16U;
        const uint32_t                        sb_cols     = av1_superblock_count(av1_frame_width(pic), use_128x128);
        const uint32_t                        sb_rows     = av1_superblock_count(static_cast<uint32_t>(pic->frame_height_minus1) + 1U, use_128x128);

        if (pic->pic_info_fields.bits.uniform_tile_spacing_flag) {
            fill_uniform_tile_sizes(std_data->width_in_sbs_minus1.data(), tile_cols, sb_cols);
            fill_uniform_tile_sizes(std_data->height_in_sbs_minus1.data(), tile_rows, sb_rows);
        } else {
            fill_explicit_tile_sizes(std_data->width_in_sbs_minus1.data(), pic->width_in_sbs_minus_1, tile_cols, sb_cols);
            fill_explicit_tile_sizes(std_data->height_in_sbs_minus1.data(), pic->height_in_sbs_minus_1, tile_rows, sb_rows);
        }

        uint32_t mi_col = 0;
        for (uint32_t i = 0; i < tile_cols; i++) {
            std_data->mi_col_starts[i] = static_cast<uint16_t>(std::min<uint32_t>(mi_col, UINT16_MAX));
            mi_col += (static_cast<uint32_t>(std_data->width_in_sbs_minus1[i]) + 1U) * mib_size;
        }
        std_data->mi_col_starts[tile_cols] = static_cast<uint16_t>(std::min<uint32_t>(mi_col, UINT16_MAX));

        uint32_t mi_row = 0;
        for (uint32_t i = 0; i < tile_rows; i++) {
            std_data->mi_row_starts[i] = static_cast<uint16_t>(std::min<uint32_t>(mi_row, UINT16_MAX));
            mi_row += (static_cast<uint32_t>(std_data->height_in_sbs_minus1[i]) + 1U) * mib_size;
        }
        std_data->mi_row_starts[tile_rows] = static_cast<uint16_t>(std::min<uint32_t>(mi_row, UINT16_MAX));

        std_data->tile_info                                 = {};
        std_data->tile_info.flags.uniform_tile_spacing_flag = pic->pic_info_fields.bits.uniform_tile_spacing_flag;
        std_data->tile_info.TileCols                        = static_cast<uint8_t>(tile_cols);
        std_data->tile_info.TileRows                        = static_cast<uint8_t>(tile_rows);
        std_data->tile_info.context_update_tile_id          = static_cast<uint16_t>(pic->context_update_tile_id);
        std_data->tile_info.tile_size_bytes_minus_1         = input->header.tile_size_bytes_minus_1;
        std_data->tile_info.pMiColStarts                    = std_data->mi_col_starts.data();
        std_data->tile_info.pMiRowStarts                    = std_data->mi_row_starts.data();
        std_data->tile_info.pWidthInSbsMinus1               = std_data->width_in_sbs_minus1.data();
        std_data->tile_info.pHeightInSbsMinus1              = std_data->height_in_sbs_minus1.data();
    }

    bool validate_av1_switch_frame_impl(const VkvvAV1DecodeInput* input, char* reason, size_t reason_size) {
        if (!av1_frame_is_switch(input)) {
            return true;
        }
        if (!input->header.error_resilient_mode || input->pic->pic_info_fields.bits.error_resilient_mode == 0) {
            std::snprintf(reason, reason_size, "invalid AV1 switch frame: error_resilient_mode must be set");
            return false;
        }
        if (!input->header.frame_size_override_flag) {
            std::snprintf(reason, reason_size, "invalid AV1 switch frame: frame_size_override_flag must be set");
            return false;
        }
        if (input->header.refresh_frame_flags != 0xff) {
            std::snprintf(reason, reason_size, "invalid AV1 switch frame: refresh_frame_flags=0x%02x expected=0xff", input->header.refresh_frame_flags);
            return false;
        }
        if (input->pic->primary_ref_frame != STD_VIDEO_AV1_PRIMARY_REF_NONE) {
            std::snprintf(reason, reason_size, "invalid AV1 switch frame: primary_ref_frame=%u expected=%u", input->pic->primary_ref_frame, STD_VIDEO_AV1_PRIMARY_REF_NONE);
            return false;
        }
        return true;
    }

    bool build_av1_picture_std_data_impl(AV1VideoSession* session, const VkvvAV1DecodeInput* input, AV1PictureStdData* std_data, char* reason, size_t reason_size) {
        if (session == nullptr || input == nullptr || input->pic == nullptr || std_data == nullptr) {
            std::snprintf(reason, reason_size, "missing AV1 std-picture input");
            return false;
        }
        if (!validate_av1_switch_frame_impl(input, reason, reason_size)) {
            return false;
        }
        const VADecPictureParameterBufferAV1* pic    = input->pic;
        const VkvvAV1FrameHeader&             header = input->header;

        build_av1_tile_info(input, std_data);

        std_data->quantization                     = {};
        std_data->quantization.flags.using_qmatrix = pic->qmatrix_fields.bits.using_qmatrix;
        std_data->quantization.flags.diff_uv_delta = pic->u_dc_delta_q != pic->v_dc_delta_q || pic->u_ac_delta_q != pic->v_ac_delta_q;
        std_data->quantization.base_q_idx          = pic->base_qindex;
        std_data->quantization.DeltaQYDc           = pic->y_dc_delta_q;
        std_data->quantization.DeltaQUDc           = pic->u_dc_delta_q;
        std_data->quantization.DeltaQUAc           = pic->u_ac_delta_q;
        std_data->quantization.DeltaQVDc           = pic->v_dc_delta_q;
        std_data->quantization.DeltaQVAc           = pic->v_ac_delta_q;
        std_data->quantization.qm_y                = pic->qmatrix_fields.bits.qm_y;
        std_data->quantization.qm_u                = pic->qmatrix_fields.bits.qm_u;
        std_data->quantization.qm_v                = pic->qmatrix_fields.bits.qm_v;

        std_data->segmentation = {};
        std::memcpy(std_data->segmentation.FeatureEnabled, pic->seg_info.feature_mask, sizeof(std_data->segmentation.FeatureEnabled));
        std::memcpy(std_data->segmentation.FeatureData, pic->seg_info.feature_data, sizeof(std_data->segmentation.FeatureData));

        std_data->loop_filter                                 = {};
        std_data->loop_filter.flags.loop_filter_delta_enabled = pic->loop_filter_info_fields.bits.mode_ref_delta_enabled;
        std_data->loop_filter.flags.loop_filter_delta_update  = pic->loop_filter_info_fields.bits.mode_ref_delta_update;
        std_data->loop_filter.loop_filter_level[0]            = pic->filter_level[0];
        std_data->loop_filter.loop_filter_level[1]            = pic->filter_level[1];
        std_data->loop_filter.loop_filter_level[2]            = pic->filter_level_u;
        std_data->loop_filter.loop_filter_level[3]            = pic->filter_level_v;
        std_data->loop_filter.loop_filter_sharpness           = pic->loop_filter_info_fields.bits.sharpness_level;
        if (pic->primary_ref_frame == STD_VIDEO_AV1_PRIMARY_REF_NONE) {
            session->loop_filter_ref_deltas  = {1, 0, 0, 0, -1, 0, -1, -1};
            session->loop_filter_mode_deltas = {0, 0};
        }
        if (pic->loop_filter_info_fields.bits.mode_ref_delta_update) {
            std_data->loop_filter.update_ref_delta  = 0xff;
            std_data->loop_filter.update_mode_delta = 0x03;
            for (uint32_t i = 0; i < STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME; i++) {
                session->loop_filter_ref_deltas[i] = pic->ref_deltas[i];
            }
            for (uint32_t i = 0; i < STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS; i++) {
                session->loop_filter_mode_deltas[i] = pic->mode_deltas[i];
            }
        } else {
            std_data->loop_filter.update_ref_delta  = 0;
            std_data->loop_filter.update_mode_delta = 0;
        }
        std::memcpy(std_data->loop_filter.loop_filter_ref_deltas, session->loop_filter_ref_deltas.data(), sizeof(std_data->loop_filter.loop_filter_ref_deltas));
        std::memcpy(std_data->loop_filter.loop_filter_mode_deltas, session->loop_filter_mode_deltas.data(), sizeof(std_data->loop_filter.loop_filter_mode_deltas));

        std_data->cdef                      = {};
        std_data->cdef.cdef_damping_minus_3 = pic->cdef_damping_minus_3;
        std_data->cdef.cdef_bits            = pic->cdef_bits;
        for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_CDEF_FILTER_STRENGTHS; i++) {
            std_data->cdef.cdef_y_pri_strength[i]  = (pic->cdef_y_strengths[i] >> 2) & 0x0f;
            std_data->cdef.cdef_y_sec_strength[i]  = pic->cdef_y_strengths[i] & 0x03;
            std_data->cdef.cdef_uv_pri_strength[i] = (pic->cdef_uv_strengths[i] >> 2) & 0x0f;
            std_data->cdef.cdef_uv_sec_strength[i] = pic->cdef_uv_strengths[i] & 0x03;
        }

        std_data->restoration                         = {};
        std_data->restoration.FrameRestorationType[0] = av1_restoration_type(pic->loop_restoration_fields.bits.yframe_restoration_type);
        std_data->restoration.FrameRestorationType[1] = av1_restoration_type(pic->loop_restoration_fields.bits.cbframe_restoration_type);
        std_data->restoration.FrameRestorationType[2] = av1_restoration_type(pic->loop_restoration_fields.bits.crframe_restoration_type);
        if (!av1_restoration_size_code(pic->loop_restoration_fields.bits.yframe_restoration_type, pic->loop_restoration_fields.bits.lr_unit_shift, false, false,
                                       &std_data->restoration.LoopRestorationSize[0], reason, reason_size) ||
            !av1_restoration_size_code(pic->loop_restoration_fields.bits.cbframe_restoration_type, pic->loop_restoration_fields.bits.lr_unit_shift, true,
                                       pic->loop_restoration_fields.bits.lr_uv_shift, &std_data->restoration.LoopRestorationSize[1], reason, reason_size) ||
            !av1_restoration_size_code(pic->loop_restoration_fields.bits.crframe_restoration_type, pic->loop_restoration_fields.bits.lr_unit_shift, true,
                                       pic->loop_restoration_fields.bits.lr_uv_shift, &std_data->restoration.LoopRestorationSize[2], reason, reason_size)) {
            return false;
        }

        std_data->global_motion                                                  = {};
        std_data->global_motion.GmType[STD_VIDEO_AV1_REFERENCE_NAME_INTRA_FRAME] = VAAV1TransformationIdentity;
        for (uint32_t i = 0; i < VKVV_AV1_ACTIVE_REFERENCE_COUNT; i++) {
            const uint32_t ref_name                  = i + 1;
            std_data->global_motion.GmType[ref_name] = pic->wm[i].invalid ? static_cast<uint8_t>(VAAV1TransformationIdentity) : static_cast<uint8_t>(pic->wm[i].wmtype);
            for (uint32_t j = 0; j < STD_VIDEO_AV1_GLOBAL_MOTION_PARAMS; j++) {
                std_data->global_motion.gm_params[ref_name][j] = pic->wm[i].wmmat[j];
            }
        }

        std_data->picture                                        = {};
        std_data->picture.flags.error_resilient_mode             = pic->pic_info_fields.bits.error_resilient_mode;
        std_data->picture.flags.disable_cdf_update               = pic->pic_info_fields.bits.disable_cdf_update;
        std_data->picture.flags.use_superres                     = pic->pic_info_fields.bits.use_superres;
        std_data->picture.flags.render_and_frame_size_different  = header.render_and_frame_size_different;
        std_data->picture.flags.allow_screen_content_tools       = pic->pic_info_fields.bits.allow_screen_content_tools;
        std_data->picture.flags.is_filter_switchable             = header.is_filter_switchable;
        std_data->picture.flags.force_integer_mv                 = pic->pic_info_fields.bits.force_integer_mv;
        std_data->picture.flags.frame_size_override_flag         = header.frame_size_override_flag;
        std_data->picture.flags.buffer_removal_time_present_flag = header.buffer_removal_time_present_flag;
        std_data->picture.flags.allow_intrabc                    = pic->pic_info_fields.bits.allow_intrabc;
        std_data->picture.flags.frame_refs_short_signaling       = header.frame_refs_short_signaling;
        std_data->picture.flags.allow_high_precision_mv          = pic->pic_info_fields.bits.allow_high_precision_mv;
        std_data->picture.flags.is_motion_mode_switchable        = pic->pic_info_fields.bits.is_motion_mode_switchable;
        std_data->picture.flags.use_ref_frame_mvs                = pic->pic_info_fields.bits.use_ref_frame_mvs;
        std_data->picture.flags.disable_frame_end_update_cdf     = pic->pic_info_fields.bits.disable_frame_end_update_cdf;
        std_data->picture.flags.allow_warped_motion              = pic->pic_info_fields.bits.allow_warped_motion;
        std_data->picture.flags.reduced_tx_set                   = pic->mode_control_fields.bits.reduced_tx_set_used;
        std_data->picture.flags.reference_select                 = pic->mode_control_fields.bits.reference_select;
        std_data->picture.flags.skip_mode_present                = pic->mode_control_fields.bits.skip_mode_present;
        std_data->picture.flags.delta_q_present                  = pic->mode_control_fields.bits.delta_q_present_flag;
        std_data->picture.flags.delta_lf_present                 = pic->mode_control_fields.bits.delta_lf_present_flag;
        std_data->picture.flags.delta_lf_multi                   = pic->mode_control_fields.bits.delta_lf_multi;
        std_data->picture.flags.segmentation_enabled             = pic->seg_info.segment_info_fields.bits.enabled;
        std_data->picture.flags.segmentation_update_map          = pic->seg_info.segment_info_fields.bits.update_map;
        std_data->picture.flags.segmentation_temporal_update     = pic->seg_info.segment_info_fields.bits.temporal_update;
        std_data->picture.flags.segmentation_update_data         = pic->seg_info.segment_info_fields.bits.update_data;
        std_data->picture.flags.UsesLr = pic->loop_restoration_fields.bits.yframe_restoration_type != 0 || pic->loop_restoration_fields.bits.cbframe_restoration_type != 0 ||
            pic->loop_restoration_fields.bits.crframe_restoration_type != 0;
        std_data->picture.flags.usesChromaLr   = pic->loop_restoration_fields.bits.cbframe_restoration_type != 0 || pic->loop_restoration_fields.bits.crframe_restoration_type != 0;
        std_data->picture.flags.apply_grain    = false;
        std_data->picture.frame_type           = av1_effective_frame_type(pic->pic_info_fields.bits.frame_type);
        std_data->picture.current_frame_id     = header.current_frame_id;
        std_data->picture.OrderHint            = static_cast<uint8_t>(pic->order_hint);
        std_data->picture.primary_ref_frame    = pic->primary_ref_frame;
        std_data->picture.refresh_frame_flags  = header.refresh_frame_flags;
        std_data->picture.interpolation_filter = av1_interpolation_filter(pic->interp_filter);
        std_data->picture.TxMode               = av1_tx_mode(pic->mode_control_fields.bits.tx_mode);
        std_data->picture.delta_q_res          = pic->mode_control_fields.bits.log2_delta_q_res;
        std_data->picture.delta_lf_res         = pic->mode_control_fields.bits.log2_delta_lf_res;
        std::memcpy(std_data->picture.SkipModeFrame, header.skip_mode_frame, sizeof(std_data->picture.SkipModeFrame));
        std_data->picture.coded_denom = pic->pic_info_fields.bits.use_superres ? pic->superres_scale_denominator : 8;
        std::memcpy(std_data->picture.OrderHints, header.order_hints, sizeof(std_data->picture.OrderHints));
        for (uint32_t i = 0; i < VKVV_AV1_ACTIVE_REFERENCE_COUNT; i++) {
            std_data->picture.expectedFrameId[i + 1] = header.expected_frame_id[i];
        }
        std_data->picture.pTileInfo        = &std_data->tile_info;
        std_data->picture.pQuantization    = &std_data->quantization;
        std_data->picture.pSegmentation    = &std_data->segmentation;
        std_data->picture.pLoopFilter      = &std_data->loop_filter;
        std_data->picture.pCDEF            = &std_data->cdef;
        std_data->picture.pLoopRestoration = &std_data->restoration;
        std_data->picture.pGlobalMotion    = &std_data->global_motion;
        return true;
    }

    StdVideoDecodeAV1ReferenceInfo build_av1_current_reference_info_impl(const VkvvAV1DecodeInput* input) {
        StdVideoDecodeAV1ReferenceInfo info{};
        info.flags.disable_frame_end_update_cdf = input->pic->pic_info_fields.bits.disable_frame_end_update_cdf;
        info.flags.segmentation_enabled         = input->pic->seg_info.segment_info_fields.bits.enabled;
        info.frame_type                         = static_cast<uint8_t>(av1_effective_frame_type(input->pic->pic_info_fields.bits.frame_type));
        info.RefFrameSignBias                   = av1_ref_frame_sign_bias_mask(input->header);
        info.OrderHint                          = static_cast<uint8_t>(input->pic->order_hint);
        std::memcpy(info.SavedOrderHints, input->header.order_hints, sizeof(info.SavedOrderHints));
        return info;
    }

    AV1ReferenceMetadata build_current_reference_metadata(const VkvvDriver* drv, const VkvvContext* vctx, const VkvvSurface* target, const SurfaceResource* resource,
                                                          const DecodeImageKey& decode_key, const VkvvAV1DecodeInput* input, bool displayed) {
        AV1ReferenceMetadata metadata{};
        if (drv == nullptr || vctx == nullptr || target == nullptr || resource == nullptr || input == nullptr) {
            return metadata;
        }
        metadata.driver_instance_id = drv->driver_instance_id;
        metadata.stream_id          = vctx->stream_id;
        metadata.codec_operation    = decode_key.codec_operation;
        metadata.surface_id         = target->id;
        metadata.content_generation = resource->content_generation + 1;
        metadata.decode_key         = decode_key;
        metadata.coded_extent       = resource->coded_extent;
        metadata.va_rt_format       = target->rt_format;
        metadata.va_fourcc          = target->fourcc;
        metadata.bit_depth          = input->bit_depth;
        metadata.frame_id           = input->header.current_frame_id;
        metadata.showable           = input->header.showable_frame;
        metadata.displayed          = displayed;
        return metadata;
    }

    bool validate_av1_decode_input_bounds(const VkvvAV1DecodeInput* input, char* reason, size_t reason_size) {
        if (input == nullptr || input->bitstream == nullptr || input->bitstream_size == 0 || input->tiles == nullptr || input->tile_count == 0) {
            std::snprintf(reason, reason_size, "missing AV1 decode bitstream bounds");
            return false;
        }
        if (input->tile_count > VKVV_AV1_MAX_TILES) {
            std::snprintf(reason, reason_size, "AV1 decode has too many tiles: tiles=%zu max=%u", input->tile_count, VKVV_AV1_MAX_TILES);
            return false;
        }
        if (input->bitstream_size > std::numeric_limits<uint32_t>::max()) {
            std::snprintf(reason, reason_size, "AV1 decode bitstream is too large: bytes=%zu", input->bitstream_size);
            return false;
        }
        if (input->header.frame_header_offset >= input->bitstream_size) {
            std::snprintf(reason, reason_size, "AV1 frame header offset exceeds bitstream: header=%u bytes=%zu", input->header.frame_header_offset, input->bitstream_size);
            return false;
        }
        for (size_t i = 0; i < input->tile_count; i++) {
            const uint64_t tile_end = static_cast<uint64_t>(input->tiles[i].offset) + input->tiles[i].size;
            if (input->tiles[i].size == 0 || input->tiles[i].offset >= input->bitstream_size || tile_end > input->bitstream_size) {
                std::snprintf(reason, reason_size, "AV1 tile exceeds decode bitstream: tile=%zu offset=%u size=%u bytes=%zu header=%u", i, input->tiles[i].offset,
                              input->tiles[i].size, input->bitstream_size, input->header.frame_header_offset);
                return false;
            }
        }
        return true;
    }

    uint32_t used_slot_mask(const bool used_slots[max_av1_dpb_slots]) {
        uint32_t mask = 0;
        if (used_slots == nullptr) {
            return mask;
        }
        for (uint32_t i = 0; i < max_av1_dpb_slots; i++) {
            if (used_slots[i]) {
                mask |= 1U << i;
            }
        }
        return mask;
    }

    std::string av1_reference_slots_string(const AV1VideoSession* session) {
        std::string text;
        if (session == nullptr) {
            return text;
        }
        char item[96] = {};
        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            const AV1ReferenceSlot& entry = session->reference_slots[i];
            if (entry.surface_id == VA_INVALID_ID || entry.slot < 0) {
                continue;
            }
            std::snprintf(item, sizeof(item), "%s%u:%u/%d/oh%u", text.empty() ? "" : ",", i, entry.surface_id, entry.slot, entry.info.OrderHint);
            text += item;
        }
        return text.empty() ? "-" : text;
    }

    std::string av1_surface_slots_string(const AV1VideoSession* session) {
        std::string text;
        if (session == nullptr) {
            return "-";
        }
        char item[96] = {};
        for (const AV1ReferenceSlot& entry : session->surface_slots) {
            if (entry.surface_id == VA_INVALID_ID || entry.slot < 0) {
                continue;
            }
            std::snprintf(item, sizeof(item), "%s%u/%d/oh%u", text.empty() ? "" : ",", entry.surface_id, entry.slot, entry.info.OrderHint);
            text += item;
        }
        return text.empty() ? "-" : text;
    }

    std::string av1_ref_frame_map_string(const VADecPictureParameterBufferAV1* pic) {
        std::string text;
        if (pic == nullptr) {
            return "-";
        }
        char item[64] = {};
        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            std::snprintf(item, sizeof(item), "%s%u:%u", text.empty() ? "" : ",", i, pic->ref_frame_map[i]);
            text += item;
        }
        return text.empty() ? "-" : text;
    }

    std::string av1_active_refs_string(const VkvvAV1DecodeInput* input, const int32_t reference_name_slot_indices[VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR]) {
        std::string text;
        if (input == nullptr || input->pic == nullptr || reference_name_slot_indices == nullptr) {
            return "-";
        }
        char item[112] = {};
        for (uint32_t i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
            const uint8_t     ref_index   = av1_va_ref_frame_index(input->pic, i);
            const VASurfaceID ref_surface = ref_index < max_av1_reference_slots ? input->pic->ref_frame_map[ref_index] : VA_INVALID_ID;
            std::snprintf(item, sizeof(item), "%s%u:idx%u/surf%u/slot%d", text.empty() ? "" : ",", i, ref_index, ref_surface, reference_name_slot_indices[i]);
            text += item;
        }
        return text.empty() ? "-" : text;
    }

} // namespace

namespace vkvv {

    bool validate_av1_switch_frame(const VkvvAV1DecodeInput* input, char* reason, size_t reason_size) {
        return validate_av1_switch_frame_impl(input, reason, reason_size);
    }

    bool build_av1_picture_std_data(AV1VideoSession* session, const VkvvAV1DecodeInput* input, AV1PictureStdData* std_data, char* reason, size_t reason_size) {
        return build_av1_picture_std_data_impl(session, input, std_data, reason, reason_size);
    }

    StdVideoDecodeAV1ReferenceInfo build_av1_current_reference_info(const VkvvAV1DecodeInput* input) {
        return build_av1_current_reference_info_impl(input);
    }

} // namespace vkvv

VAStatus vkvv_vulkan_decode_av1(void* runtime_ptr, void* session_ptr, VkvvDriver* drv, VkvvContext* vctx, VkvvSurface* target, VAProfile profile, const VkvvAV1DecodeInput* input,
                                char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    auto* session = static_cast<AV1VideoSession*>(session_ptr);
    if (runtime == nullptr || session == nullptr || drv == nullptr || vctx == nullptr || target == nullptr || input == nullptr || input->pic == nullptr) {
        std::snprintf(reason, reason_size, "missing AV1 decode state");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (profile != session->va_profile || input->pic->profile != session->bitstream_profile || input->bit_depth != session->bit_depth) {
        std::snprintf(reason, reason_size, "AV1 Vulkan profile mismatch: context=%d session=%d va_profile=%u depth=%u expected_profile=%u expected_depth=%u", profile,
                      session->va_profile, input->pic->profile, input->bit_depth, session->bitstream_profile, session->bit_depth);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (target->rt_format != session->va_rt_format || target->fourcc != session->va_fourcc) {
        std::snprintf(reason, reason_size, "AV1 target surface format mismatch: rt=0x%x fourcc=0x%x expected_rt=0x%x expected_fourcc=0x%x", target->rt_format, target->fourcc,
                      session->va_rt_format, session->va_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (input->rt_format != session->va_rt_format || input->fourcc != session->va_fourcc) {
        std::snprintf(reason, reason_size, "AV1 bitstream/output format mismatch: rt=0x%x fourcc=0x%x expected_rt=0x%x expected_fourcc=0x%x", input->rt_format, input->fourcc,
                      session->va_rt_format, session->va_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    if (session->video.session == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "missing AV1 video session");
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!input->header.valid) {
        std::snprintf(reason, reason_size, "missing AV1 frame header");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (!input->header.show_existing_frame) {
        if (input->bitstream == nullptr || input->bitstream_size == 0 || input->tiles == nullptr || input->tile_count == 0) {
            std::snprintf(reason, reason_size, "missing AV1 bitstream/header/tile data");
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (!validate_av1_decode_input_bounds(input, reason, reason_size)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (!validate_av1_switch_frame(input, reason, reason_size)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
    }
    if ((session->decode_flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) == 0) {
        std::snprintf(reason, reason_size, "AV1 decode requires coincident DPB/output support");
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    const VkExtent2D coded_extent = {
        round_up_16(input->frame_width),
        round_up_16(input->frame_height),
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
        std::snprintf(reason, reason_size, "missing AV1 target surface id");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    bool used_slots[max_av1_dpb_slots] = {};

    struct ReferenceRecord {
        VkvvSurface*                   surface  = nullptr;
        SurfaceResource*               resource = nullptr;
        VkVideoPictureResourceInfoKHR  picture{};
        StdVideoDecodeAV1ReferenceInfo std_ref{};
        VkVideoDecodeAV1DpbSlotInfoKHR av1_slot{};
        VkVideoReferenceSlotInfoKHR    slot{};
    };

    std::array<ReferenceRecord, max_av1_active_references> references{};
    std::array<bool, max_av1_dpb_slots>                    referenced_dpb_slots{};
    uint32_t                                               reference_count                                                        = 0;
    int32_t                                                reference_name_slot_indices[VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR] = {-1, -1, -1, -1, -1, -1, -1};
    const bool                                             trace_deep_enabled                                                     = vkvv_trace_deep_enabled();
    const bool                                             refresh_export                                                         = av1_decode_needs_export_refresh(input);

    if (trace_deep_enabled) {
        const std::string ref_map              = av1_ref_frame_map_string(input->pic);
        const std::string ref_slots_before     = av1_reference_slots_string(session);
        const std::string surface_slots_before = av1_surface_slots_string(session);
        VKVV_TRACE("av1-frame-enter",
                   "driver=%llu ctx_stream=%llu target=%u current_frame=%u order_hint=%u frame_type=%u show=%u hdr_existing=%u hdr_show=%u hdr_showable=%u refresh_export=%u "
                   "refresh=0x%02x primary_ref=%u depth=%u fourcc=0x%x bitstream=%zu header=%u tiles=%zu "
                   "ref_map=\"%s\" ref_slots=\"%s\" surface_slots=\"%s\"",
                   static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id, input->pic->current_frame,
                   input->pic->order_hint, input->pic->pic_info_fields.bits.frame_type, input->pic->pic_info_fields.bits.show_frame, input->header.show_existing_frame ? 1U : 0U,
                   input->header.show_frame ? 1U : 0U, input->header.showable_frame ? 1U : 0U, refresh_export ? 1U : 0U, input->header.refresh_frame_flags,
                   input->pic->primary_ref_frame, input->bit_depth, input->fourcc, input->bitstream_size, input->header.frame_header_offset, input->tile_count, ref_map.c_str(),
                   ref_slots_before.c_str(), surface_slots_before.c_str());
    }

    if (input->header.show_existing_frame) {
        if (input->tile_count != 0 || input->bitstream_size != 0) {
            std::snprintf(reason, reason_size, "AV1 show-existing frame must not carry decode tiles: tiles=%zu bytes=%zu", input->tile_count, input->bitstream_size);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (input->header.frame_to_show_map_idx < 0 || input->header.frame_to_show_map_idx >= static_cast<int8_t>(max_av1_reference_slots)) {
            std::snprintf(reason, reason_size, "invalid AV1 show-existing reference index: map_idx=%d", input->header.frame_to_show_map_idx);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        const auto* show_slot = av1_reference_slot_for_index(session, static_cast<uint32_t>(input->header.frame_to_show_map_idx));
        if (show_slot == nullptr) {
            std::snprintf(reason, reason_size, "missing AV1 show-existing reference slot: map_idx=%d", input->header.frame_to_show_map_idx);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        auto* ref_surface = static_cast<VkvvSurface*>(vkvv_object_get(drv, show_slot->surface_id, VKVV_OBJECT_SURFACE));
        if (ref_surface == nullptr) {
            std::snprintf(reason, reason_size, "AV1 show-existing reference surface %u is missing: map_idx=%d", show_slot->surface_id, input->header.frame_to_show_map_idx);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        VAStatus ref_status = complete_pending_surface_work_if_needed(runtime, ref_surface, "AV1 show-existing reference", reason, reason_size);
        if (ref_status != VA_STATUS_SUCCESS) {
            return ref_status;
        }

        auto* ref_resource = static_cast<SurfaceResource*>(ref_surface->vulkan);
        show_slot          = validate_av1_show_existing_reference(session, input, ref_surface, ref_resource, drv, vctx, decode_key, reason, reason_size);
        if (show_slot == nullptr) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        auto* target_resource = static_cast<SurfaceResource*>(target->vulkan);
        if (!copy_av1_show_existing_surface(runtime, ref_resource, target_resource, reason, reason_size)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        target->decoded     = true;
        target->work_state  = VKVV_SURFACE_WORK_READY;
        target->sync_status = VA_STATUS_SUCCESS;
        if (target_resource != ref_resource) {
            target_resource->content_generation++;
        }

        VAStatus export_status = vkvv_vulkan_refresh_surface_export(runtime, target, true, reason, reason_size);
        if (export_status != VA_STATUS_SUCCESS) {
            return export_status;
        }

        VKVV_TRACE("av1-show-existing", "driver=%llu ctx_stream=%llu target=%u source=%u map_idx=%d slot=%d display_frame_id=%u target_gen=%llu source_gen=%llu refresh_export=1",
                   static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id, show_slot->surface_id,
                   input->header.frame_to_show_map_idx, show_slot->slot, input->header.display_frame_id, static_cast<unsigned long long>(target_resource->content_generation),
                   static_cast<unsigned long long>(ref_resource->content_generation));
        VKVV_SUCCESS_REASON(reason, reason_size, "presented AV1 show-existing frame: target=%u source=%u map_idx=%d slot=%d depth=%u fourcc=0x%x", target_surface_id,
                            show_slot->surface_id, input->header.frame_to_show_map_idx, show_slot->slot, input->bit_depth, input->fourcc);
        return VA_STATUS_SUCCESS;
    }

    if (!av1_frame_is_intra(input->pic)) {
        for (uint32_t i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
            const uint8_t reference_index = av1_va_ref_frame_index(input->pic, i);
            if (static_cast<uint32_t>(reference_index) >= max_av1_reference_slots) {
                std::snprintf(reason, reason_size, "AV1 active reference index %d is invalid", reference_index);
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }

            const VASurfaceID       ref_surface_id = input->pic->ref_frame_map[reference_index];
            auto*                   ref_surface    = static_cast<VkvvSurface*>(vkvv_object_get(drv, ref_surface_id, VKVV_OBJECT_SURFACE));
            const AV1ReferenceSlot* named_slot     = av1_reference_slot_for_index(session, static_cast<uint32_t>(reference_index));
            const AV1ReferenceSlot* surface_slot   = av1_surface_slot_for_surface(session, ref_surface_id);
            const auto*             ref_resource   = ref_surface != nullptr ? static_cast<const SurfaceResource*>(ref_surface->vulkan) : nullptr;
            if (trace_deep_enabled) {
                VKVV_TRACE("av1-ref-resolve",
                           "driver=%llu ctx_stream=%llu target=%u ref_name=%u ref_idx=%u ref_surface=%u named_surface=%u named_slot=%d named_oh=%u surface_slot=%d surface_oh=%u "
                           "decoded=%u pending=%u ref_stream=%llu ref_codec=0x%x content_gen=%llu shadow_gen=%llu predecode=%u seeded=%u",
                           static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id, i, reference_index,
                           ref_surface_id, named_slot != nullptr ? named_slot->surface_id : VA_INVALID_ID, named_slot != nullptr ? named_slot->slot : -1,
                           named_slot != nullptr ? named_slot->info.OrderHint : 0, surface_slot != nullptr ? surface_slot->slot : -1,
                           surface_slot != nullptr ? surface_slot->info.OrderHint : 0, ref_surface != nullptr && ref_surface->decoded ? 1U : 0U,
                           ref_surface != nullptr && ref_surface->work_state == VKVV_SURFACE_WORK_RENDERING ? 1U : 0U,
                           ref_surface != nullptr ? static_cast<unsigned long long>(ref_surface->stream_id) : 0ULL, ref_surface != nullptr ? ref_surface->codec_operation : 0U,
                           ref_resource != nullptr ? static_cast<unsigned long long>(ref_resource->content_generation) : 0ULL,
                           ref_resource != nullptr ? static_cast<unsigned long long>(ref_resource->export_resource.content_generation) : 0ULL,
                           ref_resource != nullptr && ref_resource->export_resource.predecode_exported ? 1U : 0U,
                           ref_resource != nullptr && ref_resource->export_resource.predecode_seeded ? 1U : 0U);
            }
            if (ref_surface == nullptr) {
                std::snprintf(reason, reason_size, "AV1 reference surface %u is missing: ref_name=%u ref_idx=%d", ref_surface_id, i, reference_index);
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }
            VAStatus ref_status = complete_pending_surface_work_if_needed(runtime, ref_surface, "AV1 reference", reason, reason_size);
            if (ref_status != VA_STATUS_SUCCESS) {
                return ref_status;
            }

            const AV1ReferenceSlot* stored_slot  = av1_reconcile_reference_slot(session, static_cast<uint32_t>(reference_index), ref_surface_id);
            const int               ref_dpb_slot = stored_slot != nullptr ? stored_slot->slot : -1;
            auto*                   resource     = static_cast<SurfaceResource*>(ref_surface->vulkan);
            if (!validate_av1_reference_slot(session, stored_slot, ref_surface, resource, drv, vctx, decode_key, reason, reason_size)) {
                const size_t used = std::strlen(reason);
                if (used < reason_size) {
                    std::snprintf(reason + used, reason_size - used, ": ref_name=%u ref_idx=%d stored_surface=%u target=%u current=%u", i, reference_index,
                                  stored_slot != nullptr ? stored_slot->surface_id : VA_INVALID_ID, vctx->render_target, input->pic->current_frame);
                }
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }

            reference_name_slot_indices[i] = ref_dpb_slot;
            used_slots[ref_dpb_slot]       = true;
            if (referenced_dpb_slots[ref_dpb_slot]) {
                continue;
            }

            ReferenceRecord& record            = references[reference_count++];
            record.surface                     = ref_surface;
            record.resource                    = resource;
            record.picture                     = make_picture_resource(resource, resource->extent);
            record.std_ref                     = stored_slot->info;
            record.av1_slot.sType              = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR;
            record.av1_slot.pStdReferenceInfo  = &record.std_ref;
            record.slot.sType                  = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
            record.slot.pNext                  = &record.av1_slot;
            record.slot.slotIndex              = ref_dpb_slot;
            record.slot.pPictureResource       = &record.picture;
            referenced_dpb_slots[ref_dpb_slot] = true;
        }
    }

    av1_mark_retained_reference_slots(session, input, used_slots);

    const bool current_updates_reference_map = input->header.refresh_frame_flags != 0;
    const bool needs_setup_slot              = session->max_dpb_slots != 0;
    int        target_dpb_slot               = needs_setup_slot ? av1_select_current_setup_slot(session, target_surface_id, used_slots, current_updates_reference_map) : -1;
    if (needs_setup_slot && target_dpb_slot < 0) {
        std::snprintf(reason, reason_size, "%s",
                      current_updates_reference_map ? "no free AV1 setup DPB slot for current picture" : "no free AV1 scratch DPB slot for current picture");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    const bool has_setup_slot = target_dpb_slot >= 0;
    if (trace_deep_enabled) {
        const std::string ref_slots_before       = av1_reference_slots_string(session);
        const std::string surface_slots_before   = av1_surface_slots_string(session);
        const std::string active_refs            = av1_active_refs_string(input, reference_name_slot_indices);
        const auto*       target_resource_before = static_cast<const SurfaceResource*>(target->vulkan);
        VKVV_TRACE(
            "av1-decode-plan",
            "driver=%llu ctx_stream=%llu target=%u surface_stream=%llu surface_codec=0x%x frame_type=%u show=%u hdr_existing=%u hdr_show=%u hdr_showable=%u "
            "refresh_export=%u current_frame=%u order_hint=%u primary_ref=%u "
            "refresh=0x%02x depth=%u fourcc=0x%x "
            "refs=%u current_ref=%u setup=%u target_slot=%d used_mask=0x%03x content_gen=%llu shadow_gen=%llu exported=%u last_display_gen=%llu predecode=%u active_refs=\"%s\" "
            "ref_slots=\"%s\" surface_slots=\"%s\"",
            static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id,
            static_cast<unsigned long long>(target->stream_id), target->codec_operation, input->pic->pic_info_fields.bits.frame_type, input->pic->pic_info_fields.bits.show_frame,
            input->header.show_existing_frame ? 1U : 0U, input->header.show_frame ? 1U : 0U, input->header.showable_frame ? 1U : 0U, refresh_export ? 1U : 0U,
            input->pic->current_frame, input->pic->order_hint, input->pic->primary_ref_frame, input->header.refresh_frame_flags, input->bit_depth, input->fourcc, reference_count,
            current_updates_reference_map ? 1U : 0U, has_setup_slot ? 1U : 0U, target_dpb_slot, used_slot_mask(used_slots),
            target_resource_before != nullptr ? static_cast<unsigned long long>(target_resource_before->content_generation) : 0ULL,
            target_resource_before != nullptr ? static_cast<unsigned long long>(target_resource_before->export_resource.content_generation) : 0ULL,
            target_resource_before != nullptr && target_resource_before->export_resource.exported ? 1U : 0U,
            target_resource_before != nullptr ? static_cast<unsigned long long>(target_resource_before->last_display_refresh_generation) : 0ULL,
            target_resource_before != nullptr && target_resource_before->export_resource.predecode_exported ? 1U : 0U, active_refs.c_str(), ref_slots_before.c_str(),
            surface_slots_before.c_str());
    }
    if (target_dpb_slot >= 0) {
        used_slots[target_dpb_slot] = true;
        if (!current_updates_reference_map) {
            av1_clear_reference_slot(session, target_dpb_slot);
        }
    }

    AV1SessionStdParameters session_params{};
    build_av1_session_parameters(input, &session_params);
    VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
    if (!create_av1_session_parameters(runtime, session, &session_params, &parameters, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!session->video.initialized && !reset_av1_session(runtime, session, parameters, reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VAStatus capacity_status = ensure_command_slot_capacity(runtime, "AV1 decode", reason, reason_size);
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
                                        VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, upload, "AV1 bitstream", reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkResetFences", "AV1 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkResetCommandBuffer", "AV1 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        record_vk_result(runtime, result, "vkBeginCommandBuffer", "AV1 decode", reason, reason_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    auto*                              target_resource = static_cast<SurfaceResource*>(target->vulkan);
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(reference_count + 1);
    for (uint32_t i = 0; i < reference_count; i++) {
        add_image_layout_barrier(&barriers, references[i].resource, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR);
    }
    add_image_layout_barrier(&barriers, target_resource, av1_target_layout(has_setup_slot), av1_target_access(has_setup_slot));
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers    = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }

    StdVideoDecodeAV1ReferenceInfo     setup_std_ref{};
    VkVideoDecodeAV1DpbSlotInfoKHR     setup_av1_slot{};
    VkVideoReferenceSlotInfoKHR        setup_slot{};
    VkVideoPictureResourceInfoKHR      setup_picture{};
    const VkVideoReferenceSlotInfoKHR* setup_slot_ptr = nullptr;
    if (has_setup_slot) {
        setup_std_ref                    = build_av1_current_reference_info(input);
        setup_picture                    = make_picture_resource(target_resource, coded_extent);
        setup_av1_slot.sType             = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR;
        setup_av1_slot.pStdReferenceInfo = &setup_std_ref;
        setup_slot.sType                 = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
        setup_slot.pNext                 = &setup_av1_slot;
        setup_slot.slotIndex             = target_dpb_slot;
        setup_slot.pPictureResource      = &setup_picture;
        setup_slot_ptr                   = &setup_slot;
    }

    std::array<VkVideoReferenceSlotInfoKHR, max_av1_dpb_slots> begin_reference_slots{};
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
    video_begin.sType                  = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession           = session->video.session;
    video_begin.videoSessionParameters = parameters;
    video_begin.referenceSlotCount     = begin_reference_count;
    video_begin.pReferenceSlots        = begin_reference_count > 0 ? begin_reference_slots.data() : nullptr;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    AV1PictureStdData std_data{};
    if (!build_av1_picture_std_data(session, input, &std_data, reason, reason_size)) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    std::array<uint32_t, VKVV_AV1_MAX_TILES> tile_offsets{};
    std::array<uint32_t, VKVV_AV1_MAX_TILES> tile_sizes{};
    for (size_t i = 0; i < input->tile_count; i++) {
        tile_offsets[i] = input->tiles[i].offset;
        tile_sizes[i]   = input->tiles[i].size;
    }

    VkVideoDecodeAV1PictureInfoKHR av1_picture{};
    av1_picture.sType           = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR;
    av1_picture.pStdPictureInfo = &std_data.picture;
    std::memcpy(av1_picture.referenceNameSlotIndices, reference_name_slot_indices, sizeof(av1_picture.referenceNameSlotIndices));
    av1_picture.frameHeaderOffset = input->header.frame_header_offset;
    av1_picture.tileCount         = static_cast<uint32_t>(input->tile_count);
    av1_picture.pTileOffsets      = tile_offsets.data();
    av1_picture.pTileSizes        = tile_sizes.data();

    VkVideoPictureResourceInfoKHR                                      dst_picture = make_picture_resource(target_resource, coded_extent);

    std::array<VkVideoReferenceSlotInfoKHR, max_av1_active_references> decode_reference_slots{};
    for (uint32_t i = 0; i < reference_count; i++) {
        decode_reference_slots[i] = references[i].slot;
    }

    VkVideoDecodeInfoKHR decode_info{};
    decode_info.sType               = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
    decode_info.pNext               = &av1_picture;
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
            record_vk_result(runtime, result, "vkEndCommandBuffer", "AV1 decode", reason, reason_size);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        std::snprintf(reason, reason_size, "vkEndCommandBuffer for AV1 decode failed: %d frame=%u refs=%u setup=%u slot=%d refresh=0x%02x tiles=%u header=%u tile0=%u/%u bytes=%zu",
                      result, input->pic->pic_info_fields.bits.frame_type, reference_count, setup_slot_ptr != nullptr, target_dpb_slot, input->header.refresh_frame_flags,
                      av1_picture.tileCount, av1_picture.frameHeaderOffset, input->tile_count > 0 ? tile_offsets[0] : 0, input->tile_count > 0 ? tile_sizes[0] : 0,
                      input->bitstream_size);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VkDeviceSize upload_allocation_size = upload->allocation_size;
    const bool         submitted              = submit_command_buffer(runtime, reason, reason_size, "AV1 decode");
    if (!submitted) {
        runtime->destroy_video_session_parameters(runtime->device, parameters, nullptr);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const AV1ReferenceMetadata current_metadata = build_current_reference_metadata(drv, vctx, target, target_resource, decode_key, input, refresh_export);
    if (current_updates_reference_map) {
        av1_update_reference_slots_from_refresh(session, input, target_surface_id, target_dpb_slot, setup_std_ref, &current_metadata);
        if (trace_deep_enabled) {
            const std::string ref_slots_after     = av1_reference_slots_string(session);
            const std::string surface_slots_after = av1_surface_slots_string(session);
            VKVV_TRACE("av1-ref-update", "driver=%llu ctx_stream=%llu target=%u refresh=0x%02x target_slot=%d order_hint=%u ref_slots=\"%s\" surface_slots=\"%s\"",
                       static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id,
                       input->header.refresh_frame_flags, target_dpb_slot, input->pic->order_hint, ref_slots_after.c_str(), surface_slots_after.c_str());
        }
    } else {
        if (trace_deep_enabled) {
            const std::string surface_slots_after = av1_surface_slots_string(session);
            VKVV_TRACE("av1-ref-update-skip", "driver=%llu ctx_stream=%llu target=%u refresh=0x%02x scratch_slot=%d order_hint=%u surface_slots=\"%s\"",
                       static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id,
                       input->header.refresh_frame_flags, target_dpb_slot, input->pic->order_hint, surface_slots_after.c_str());
        }
    }

    track_pending_decode(runtime, target, parameters, upload_allocation_size, refresh_export, "AV1 decode");
    VKVV_TRACE("av1-submit",
               "driver=%llu ctx_stream=%llu target=%u slot=%d refresh=0x%02x refresh_export=%u hdr_existing=%u hdr_show=%u hdr_showable=%u depth=%u fourcc=0x%x refs=%u bytes=%zu "
               "upload_mem=%llu session_mem=%llu",
               static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id, target_dpb_slot,
               input->header.refresh_frame_flags, refresh_export ? 1U : 0U, input->header.show_existing_frame ? 1U : 0U, input->header.show_frame ? 1U : 0U,
               input->header.showable_frame ? 1U : 0U, input->bit_depth, input->fourcc, reference_count, input->bitstream_size,
               static_cast<unsigned long long>(upload_allocation_size), static_cast<unsigned long long>(session->video.memory_bytes));
    VKVV_SUCCESS_REASON(reason, reason_size,
                        "submitted async AV1 Vulkan decode: %ux%u depth=%u fourcc=0x%x tiles=%zu bytes=%zu refs=%u setup=%u slot=%d refresh=0x%02x header=%u tile0=%u/%u "
                        "decode_mem=%llu upload_mem=%llu session_mem=%llu",
                        coded_extent.width, coded_extent.height, input->bit_depth, input->fourcc, input->tile_count, input->bitstream_size, reference_count,
                        setup_slot_ptr != nullptr ? 1U : 0U, target_dpb_slot, input->header.refresh_frame_flags, av1_picture.frameHeaderOffset,
                        input->tile_count > 0 ? tile_offsets[0] : 0, input->tile_count > 0 ? tile_sizes[0] : 0, static_cast<unsigned long long>(target_resource->allocation_size),
                        static_cast<unsigned long long>(upload_allocation_size), static_cast<unsigned long long>(session->video.memory_bytes));
    return VA_STATUS_SUCCESS;
}
