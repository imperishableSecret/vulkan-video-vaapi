#include "internal.h"
#include "api.h"
#include "telemetry.h"
#include "vulkan/export/internal.h"
#include "vulkan/runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

    uint32_t av1_order_hint_bits_for_trace(const VkvvAV1SequenceHeader& sequence) {
        if (!sequence.enable_order_hint || sequence.order_hint_bits_minus_1 < 0) {
            return 0;
        }
        return static_cast<uint32_t>(sequence.order_hint_bits_minus_1) + 1U;
    }

    uint64_t av1_order_hint_modulus_for_trace(uint32_t order_hint_bits) {
        if (order_hint_bits == 0 || order_hint_bits >= 32) {
            return 0;
        }
        return 1ULL << order_hint_bits;
    }

    const char* av1_order_hint_classification(bool order_hint_enabled, bool visible_output, bool has_previous_visible, bool keyframe_reset, bool wrap_candidate,
                                             bool order_decreased, bool same_order_hint) {
        if (!order_hint_enabled) {
            return "order-hint-disabled";
        }
        if (!visible_output) {
            return "non-visible";
        }
        if (!has_previous_visible) {
            return "first-visible";
        }
        if (keyframe_reset) {
            return "keyframe-reset";
        }
        if (wrap_candidate) {
            return "order-wrap";
        }
        if (order_decreased) {
            return "order-decrease";
        }
        if (same_order_hint) {
            return "same-order";
        }
        return "forward";
    }

    void trace_av1_order_hint_state(const VkvvDriver* drv, const VkvvContext* vctx, const AV1VideoSession* session, const VkvvAV1DecodeInput* input,
                                    VASurfaceID target_surface_id, uint64_t frame_seq, bool refresh_export, uint32_t reference_count, int target_dpb_slot) {
        if (session == nullptr || input == nullptr || input->pic == nullptr) {
            return;
        }
        const uint32_t order_hint_bits    = av1_order_hint_bits_for_trace(input->sequence);
        const uint64_t order_hint_modulus = av1_order_hint_modulus_for_trace(order_hint_bits);
        const bool     order_hint_enabled = order_hint_bits != 0;
        const bool     visible_output     = refresh_export && (input->header.show_frame || input->header.show_existing_frame);
        const bool     has_previous       = session->has_last_visible_order_hint;
        const bool     order_decreased    = has_previous && input->pic->order_hint < session->last_visible_order_hint;
        const bool     same_order_hint    = has_previous && input->pic->order_hint == session->last_visible_order_hint;
        const bool     keyframe_reset     = input->pic->pic_info_fields.bits.frame_type == STD_VIDEO_AV1_FRAME_TYPE_KEY && input->header.refresh_frame_flags == 0xff &&
            reference_count == 0;
        const bool wrap_candidate = order_decreased && order_hint_enabled && order_hint_modulus != 0 &&
            session->last_visible_order_hint >= (order_hint_modulus * 3ULL) / 4ULL && input->pic->order_hint <= order_hint_modulus / 4ULL;
        const char* classification =
            av1_order_hint_classification(order_hint_enabled, visible_output, has_previous, keyframe_reset, wrap_candidate, order_decreased, same_order_hint);
        VKVV_TRACE("av1-order-hint-state",
                   "frame_seq=%llu driver=%llu ctx_stream=%llu target=%u order_hint=%u order_hint_enabled=%u order_hint_bits=%u order_hint_modulus=%llu "
                   "previous_visible_valid=%u previous_visible_order_hint=%u previous_visible_frame_seq=%llu previous_visible_surface=%u visible_output=%u "
                   "order_decreased=%u same_order_hint=%u wrap_candidate=%u keyframe_reset=%u classification=%s frame_type=%u show_frame=%u show_existing_frame=%u "
                   "showable_frame=%u refresh_export=%u refresh=0x%02x refs=%u primary_ref=%u target_slot=%d",
                   static_cast<unsigned long long>(frame_seq), drv != nullptr ? static_cast<unsigned long long>(drv->driver_instance_id) : 0ULL,
                   vctx != nullptr ? static_cast<unsigned long long>(vctx->stream_id) : 0ULL, target_surface_id, input->pic->order_hint, order_hint_enabled ? 1U : 0U,
                   order_hint_bits, static_cast<unsigned long long>(order_hint_modulus), has_previous ? 1U : 0U, session->last_visible_order_hint,
                   static_cast<unsigned long long>(session->last_visible_order_frame_seq), session->last_visible_order_surface, visible_output ? 1U : 0U,
                   order_decreased ? 1U : 0U, same_order_hint ? 1U : 0U, wrap_candidate ? 1U : 0U, keyframe_reset ? 1U : 0U, classification,
                   input->pic->pic_info_fields.bits.frame_type, input->header.show_frame ? 1U : 0U, input->header.show_existing_frame ? 1U : 0U,
                   input->header.showable_frame ? 1U : 0U, refresh_export ? 1U : 0U, input->header.refresh_frame_flags, reference_count, input->pic->primary_ref_frame,
                   target_dpb_slot);
    }

    void remember_av1_visible_order_hint(AV1VideoSession* session, const VkvvAV1DecodeInput* input, VASurfaceID target_surface_id, uint64_t frame_seq,
                                         bool refresh_export) {
        if (session == nullptr || input == nullptr || input->pic == nullptr || !refresh_export || !(input->header.show_frame || input->header.show_existing_frame)) {
            return;
        }
        session->has_last_visible_order_hint  = true;
        session->last_visible_order_hint      = input->pic->order_hint;
        session->last_visible_order_frame_seq = frame_seq;
        session->last_visible_order_surface   = target_surface_id;
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

    void trace_av1_display_decision(const VkvvDriver* drv, const VkvvContext* vctx, VASurfaceID target_surface_id, VASurfaceID last_display_surface,
                                    const VkvvAV1DecodeInput* input, const SurfaceResource* resource, bool refresh_export, const char* display_action) {
        const bool     show_frame          = input != nullptr && input->header.show_frame;
        const bool     show_existing_frame = input != nullptr && input->header.show_existing_frame;
        const bool     showable_frame      = input != nullptr && input->header.showable_frame;
        const uint32_t refresh_flags       = input != nullptr ? input->header.refresh_frame_flags : 0;
        const int      frame_to_show       = input != nullptr ? input->header.frame_to_show_map_idx : -1;
        const bool     shadow_stale        = surface_resource_decode_shadow_stale(resource);
        VKVV_TRACE("av1-display-decision",
                   "driver=%llu ctx_stream=%llu surface=%u show_frame=%u show_existing_frame=%u showable_frame=%u refresh_frame_flags=0x%02x frame_to_show_map_idx=%d "
                   "content_gen=%llu shadow_gen=%llu shadow_stale=%u refresh_export=%u exported=%u shadow_exported=%u predecode=%u seeded=%u last_display_surface=%u "
                   "last_display_gen=%llu display_action=%s",
                   drv != nullptr ? static_cast<unsigned long long>(drv->driver_instance_id) : 0ULL, vctx != nullptr ? static_cast<unsigned long long>(vctx->stream_id) : 0ULL,
                   target_surface_id, show_frame ? 1U : 0U, show_existing_frame ? 1U : 0U, showable_frame ? 1U : 0U, refresh_flags, frame_to_show,
                   resource != nullptr ? static_cast<unsigned long long>(resource->content_generation) : 0ULL,
                   resource != nullptr ? static_cast<unsigned long long>(resource->export_resource.content_generation) : 0ULL, shadow_stale ? 1U : 0U, refresh_export ? 1U : 0U,
                   resource != nullptr && resource->exported ? 1U : 0U, resource != nullptr && resource->export_resource.exported ? 1U : 0U,
                   resource != nullptr && resource->export_resource.predecode_exported ? 1U : 0U, resource != nullptr && resource->export_resource.predecode_seeded ? 1U : 0U,
                   last_display_surface, resource != nullptr ? static_cast<unsigned long long>(resource->last_display_refresh_generation) : 0ULL,
                   display_action != nullptr ? display_action : "unknown");
    }

    void set_av1_visible_output_trace(SurfaceResource* resource, const VkvvAV1DecodeInput* input, bool refresh_export) {
        clear_surface_av1_visible_output_trace(resource);
        if (resource == nullptr || input == nullptr || !refresh_export) {
            return;
        }
        resource->av1_visible_output_trace_valid    = true;
        resource->av1_visible_show_frame            = input->header.show_frame;
        resource->av1_visible_show_existing_frame   = input->header.show_existing_frame;
        resource->av1_visible_refresh_frame_flags   = input->header.refresh_frame_flags;
        resource->av1_visible_frame_to_show_map_idx = input->header.frame_to_show_map_idx;

        CodecVisibleOutputTrace visible{};
        visible.valid                     = true;
        visible.visible_output            = input->header.show_frame || input->header.show_existing_frame;
        visible.show_existing             = input->header.show_existing_frame;
        visible.frame_sequence            = resource->av1_frame_sequence;
        visible.display_order             = input->pic->order_hint;
        visible.frame_type                = input->pic->pic_info_fields.bits.frame_type;
        visible.refresh_flags             = input->header.refresh_frame_flags;
        visible.displayed_reference_index = input->header.frame_to_show_map_idx;
        visible.tile_or_slice_source      = resource->av1_tile_source;
        set_surface_visible_output_trace(resource, visible);
    }

    bool av1_env_flag_enabled(const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0;
    }

    bool av1_trace_tiles_enabled() {
        return vkvv_trace_deep_enabled() || av1_env_flag_enabled("VKVV_AV1_TRACE_TILES");
    }

    bool av1_trace_dpb_enabled() {
        return vkvv_trace_deep_enabled() || av1_env_flag_enabled("VKVV_AV1_TRACE_DPB");
    }

    uint32_t av1_fingerprint_level() {
        const char* value = std::getenv("VKVV_AV1_FINGERPRINT_LEVEL");
        if (value == nullptr || value[0] == '\0') {
            return 0;
        }
        const long parsed = std::strtol(value, nullptr, 10);
        return static_cast<uint32_t>(std::clamp<long>(parsed, 0, 4));
    }

    uint64_t fnv1a64(uint64_t hash, uint64_t value) {
        constexpr uint64_t prime = 1099511628211ULL;
        for (uint32_t i = 0; i < 8; i++) {
            hash ^= (value >> (i * 8)) & 0xffU;
            hash *= prime;
        }
        return hash;
    }

    uint64_t fnv1a64_bytes(uint64_t hash, const uint8_t* bytes, size_t size) {
        constexpr uint64_t prime = 1099511628211ULL;
        if (bytes == nullptr) {
            return hash;
        }
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= prime;
        }
        return hash;
    }

    uint32_t av1_size_to_u32(size_t size) {
        return size > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(size);
    }

    uint32_t av1_tile_sum_size_for_trace(const VkvvAV1DecodeInput* input) {
        uint64_t sum = 0;
        if (input == nullptr || input->tiles == nullptr) {
            return 0;
        }
        for (size_t i = 0; i < input->tile_count; i++) {
            sum += input->tiles[i].size;
        }
        return sum > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(sum);
    }

    uint64_t av1_bitstream_hash_for_trace(const VkvvAV1DecodeInput* input) {
        if (input == nullptr || input->bitstream == nullptr || input->bitstream_size == 0) {
            return 0;
        }
        uint64_t hash = 1469598103934665603ULL;
        hash          = fnv1a64(hash, input->bitstream_size);
        return fnv1a64_bytes(hash, input->bitstream, input->bitstream_size);
    }

    uint64_t av1_tile_bytes_hash_for_trace(const VkvvAV1DecodeInput* input) {
        if (input == nullptr || input->tiles == nullptr || input->tile_count == 0 || input->bitstream == nullptr) {
            return 0;
        }
        uint64_t hash = 1469598103934665603ULL;
        hash          = fnv1a64(hash, input->tile_count);
        for (size_t i = 0; i < input->tile_count; i++) {
            const VkvvAV1Tile& tile = input->tiles[i];
            const uint64_t     end  = static_cast<uint64_t>(tile.offset) + tile.size;
            hash                    = fnv1a64(hash, tile.tile_index);
            hash                    = fnv1a64(hash, tile.offset);
            hash                    = fnv1a64(hash, tile.size);
            if (tile.size != 0 && tile.offset < input->bitstream_size && end <= input->bitstream_size) {
                hash = fnv1a64_bytes(hash, input->bitstream + tile.offset, tile.size);
            }
        }
        return hash;
    }

    uint32_t av1_segmentation_feature_mask_or(const VADecPictureParameterBufferAV1* pic) {
        uint32_t mask = 0;
        if (pic == nullptr) {
            return mask;
        }
        for (uint32_t i = 0; i < VKVV_AV1_SEGMENT_COUNT; i++) {
            mask |= pic->seg_info.feature_mask[i];
        }
        return mask;
    }

    uint64_t av1_segmentation_feature_data_hash(const VADecPictureParameterBufferAV1* pic) {
        if (pic == nullptr) {
            return 0;
        }
        uint64_t hash = 1469598103934665603ULL;
        for (uint32_t segment = 0; segment < VKVV_AV1_SEGMENT_COUNT; segment++) {
            hash = fnv1a64(hash, pic->seg_info.feature_mask[segment]);
            for (uint32_t feature = 0; feature < VKVV_AV1_SEGMENT_FEATURE_COUNT; feature++) {
                hash = fnv1a64(hash, static_cast<uint16_t>(pic->seg_info.feature_data[segment][feature]));
            }
        }
        return hash;
    }

    void fill_av1_pending_parameter_trace(Av1PendingDecodeTrace* trace, const VkvvAV1DecodeInput* input, const AV1PictureStdData* std_data, uint32_t tile_sum_size,
                                          uint64_t tile_bytes_hash, uint64_t bitstream_hash) {
        if (trace == nullptr || input == nullptr || input->pic == nullptr || std_data == nullptr) {
            return;
        }
        const VADecPictureParameterBufferAV1* pic = input->pic;
        trace->current_frame_id                   = input->header.current_frame_id;
        trace->primary_ref_frame                  = pic->primary_ref_frame;
        trace->bitstream_size                     = av1_size_to_u32(input->bitstream_size);
        trace->tile_count                         = av1_size_to_u32(input->tile_count);
        trace->tile_sum_size                      = tile_sum_size;
        trace->tile_bytes_hash                    = tile_bytes_hash;
        trace->bitstream_hash                     = bitstream_hash;
        trace->tile_source                        = input->tile_source != nullptr ? input->tile_source : "unknown";
        trace->tile_selection_reason              = input->tile_selection_reason != nullptr ? input->tile_selection_reason : "unknown";
        trace->parser_used                        = input->parser_used;
        trace->parser_status                      = input->parser_status;
        trace->selected_obu_type                  = input->selected_obu_type;
        trace->tile_group_count                   = input->tile_group_count;
        trace->va_tile_count                      = av1_size_to_u32(input->va_tile_count);
        trace->parsed_tile_count                  = av1_size_to_u32(input->parsed_tile_count);
        trace->tile_ranges_equivalent             = input->tile_ranges_equivalent;
        trace->base_q_idx                         = pic->base_qindex;
        trace->delta_q_y_dc                       = pic->y_dc_delta_q;
        trace->delta_q_u_dc                       = pic->u_dc_delta_q;
        trace->delta_q_u_ac                       = pic->u_ac_delta_q;
        trace->delta_q_v_dc                       = pic->v_dc_delta_q;
        trace->delta_q_v_ac                       = pic->v_ac_delta_q;
        trace->using_qmatrix                      = pic->qmatrix_fields.bits.using_qmatrix != 0;
        trace->diff_uv_delta                      = std_data->quantization.flags.diff_uv_delta != 0;
        trace->qm_y                               = pic->qmatrix_fields.bits.qm_y;
        trace->qm_u                               = pic->qmatrix_fields.bits.qm_u;
        trace->qm_v                               = pic->qmatrix_fields.bits.qm_v;
        trace->error_resilient_mode               = pic->pic_info_fields.bits.error_resilient_mode != 0;
        trace->disable_cdf_update                 = pic->pic_info_fields.bits.disable_cdf_update != 0;
        trace->disable_frame_end_update_cdf       = pic->pic_info_fields.bits.disable_frame_end_update_cdf != 0;
        trace->allow_intrabc                      = pic->pic_info_fields.bits.allow_intrabc != 0;
        trace->allow_warped_motion                = pic->pic_info_fields.bits.allow_warped_motion != 0;
        trace->allow_high_precision_mv            = pic->pic_info_fields.bits.allow_high_precision_mv != 0;
        trace->is_motion_mode_switchable          = pic->pic_info_fields.bits.is_motion_mode_switchable != 0;
        trace->use_ref_frame_mvs                  = pic->pic_info_fields.bits.use_ref_frame_mvs != 0;
        trace->reference_select                   = pic->mode_control_fields.bits.reference_select != 0;
        trace->skip_mode_present                  = pic->mode_control_fields.bits.skip_mode_present != 0;
        trace->skip_mode_frame[0]                 = input->header.skip_mode_frame[0];
        trace->skip_mode_frame[1]                 = input->header.skip_mode_frame[1];
        trace->interpolation_filter               = pic->interp_filter;
        trace->std_interpolation_filter           = std_data->picture.interpolation_filter;
        trace->tx_mode                            = pic->mode_control_fields.bits.tx_mode;
        trace->std_tx_mode                        = std_data->picture.TxMode;
        trace->segmentation_enabled               = pic->seg_info.segment_info_fields.bits.enabled != 0;
        trace->segmentation_update_map            = pic->seg_info.segment_info_fields.bits.update_map != 0;
        trace->segmentation_temporal_update       = pic->seg_info.segment_info_fields.bits.temporal_update != 0;
        trace->segmentation_update_data           = pic->seg_info.segment_info_fields.bits.update_data != 0;
        trace->segmentation_feature_mask_or       = av1_segmentation_feature_mask_or(pic);
        trace->segmentation_feature_data_hash     = av1_segmentation_feature_data_hash(pic);
        trace->loop_filter_delta_enabled          = pic->loop_filter_info_fields.bits.mode_ref_delta_enabled != 0;
        trace->loop_filter_delta_update           = pic->loop_filter_info_fields.bits.mode_ref_delta_update != 0;
        trace->loop_filter_level[0]               = pic->filter_level[0];
        trace->loop_filter_level[1]               = pic->filter_level[1];
        trace->loop_filter_level[2]               = pic->filter_level_u;
        trace->loop_filter_level[3]               = pic->filter_level_v;
        trace->cdef_enabled                       = input->sequence.enable_cdef;
        trace->cdef_damping_minus_3               = pic->cdef_damping_minus_3;
        trace->cdef_bits                          = pic->cdef_bits;
        trace->restoration_enabled                = input->sequence.enable_restoration;
        trace->uses_lr                            = std_data->picture.flags.UsesLr != 0;
        trace->uses_chroma_lr                     = std_data->picture.flags.usesChromaLr != 0;
        trace->restoration_type[0]                = std_data->restoration.FrameRestorationType[0];
        trace->restoration_type[1]                = std_data->restoration.FrameRestorationType[1];
        trace->restoration_type[2]                = std_data->restoration.FrameRestorationType[2];
        trace->restoration_size[0]                = std_data->restoration.LoopRestorationSize[0];
        trace->restoration_size[1]                = std_data->restoration.LoopRestorationSize[1];
        trace->restoration_size[2]                = std_data->restoration.LoopRestorationSize[2];
    }

    void trace_av1_picture_params(const VkvvDriver* drv, const VkvvContext* vctx, VASurfaceID target_surface_id, uint64_t frame_seq, const VkvvAV1DecodeInput* input,
                                  const AV1PictureStdData* std_data, uint32_t reference_count, int target_dpb_slot, bool has_setup_slot, bool refresh_export,
                                  uint32_t tile_sum_size, uint64_t tile_bytes_hash, uint64_t bitstream_hash) {
        if (!vkvv_trace_enabled() || input == nullptr || input->pic == nullptr || std_data == nullptr) {
            return;
        }
        const VADecPictureParameterBufferAV1* pic = input->pic;
        VKVV_TRACE("av1-picture-params",
                   "frame_seq=%llu driver=%llu ctx_stream=%llu target=%u order_hint=%u frame_type=%u current_frame_id=%u show_frame=%u showable_frame=%u "
                   "show_existing_frame=%u refresh_frame_flags=0x%02x refresh_export=%u primary_ref_frame=%u reference_count=%u target_dpb_slot=%d setup_slot=%d "
                   "bitstream_size=%zu tile_count=%zu tile_sum_size=%u tile_hash=0x%llx bitstream_hash=0x%llx tile_source=%s selection_reason=%s parser_used=%u "
                   "parser_status=%d selected_obu_type=%u tile_group_count=%u va_tile_count=%zu parsed_tile_count=%zu tile_ranges_equivalent=%u "
                   "base_q_idx=%u delta_q_y_dc=%d delta_q_u_dc=%d delta_q_u_ac=%d delta_q_v_dc=%d delta_q_v_ac=%d using_qmatrix=%u diff_uv_delta=%u "
                   "qm_y=%u qm_u=%u qm_v=%u error_resilient_mode=%u disable_cdf_update=%u disable_frame_end_update_cdf=%u allow_intrabc=%u allow_warped_motion=%u "
                   "allow_high_precision_mv=%u is_motion_mode_switchable=%u use_ref_frame_mvs=%u reference_select=%u skip_mode_present=%u skip_mode_frame0=%u "
                   "skip_mode_frame1=%u interpolation_filter=%u std_interpolation_filter=%u tx_mode=%u std_tx_mode=%u segmentation_enabled=%u segmentation_update_map=%u "
                   "segmentation_temporal_update=%u segmentation_update_data=%u segmentation_feature_mask_or=0x%x segmentation_feature_data_hash=0x%llx "
                   "loop_filter_delta_enabled=%u loop_filter_delta_update=%u loop_filter_level0=%u loop_filter_level1=%u loop_filter_level2=%u loop_filter_level3=%u "
                   "cdef_enabled=%u cdef_damping_minus_3=%u cdef_bits=%u restoration_enabled=%u uses_lr=%u uses_chroma_lr=%u restoration_type_y=%u "
                   "restoration_type_cb=%u restoration_type_cr=%u restoration_size_y=%u restoration_size_cb=%u restoration_size_cr=%u",
                   static_cast<unsigned long long>(frame_seq), drv != nullptr ? static_cast<unsigned long long>(drv->driver_instance_id) : 0ULL,
                   vctx != nullptr ? static_cast<unsigned long long>(vctx->stream_id) : 0ULL, target_surface_id, pic->order_hint, pic->pic_info_fields.bits.frame_type,
                   input->header.current_frame_id, input->header.show_frame ? 1U : 0U, input->header.showable_frame ? 1U : 0U,
                   input->header.show_existing_frame ? 1U : 0U, input->header.refresh_frame_flags, refresh_export ? 1U : 0U, pic->primary_ref_frame, reference_count,
                   target_dpb_slot, has_setup_slot ? target_dpb_slot : -1, input->bitstream_size, input->tile_count, tile_sum_size,
                   static_cast<unsigned long long>(tile_bytes_hash), static_cast<unsigned long long>(bitstream_hash),
                   input->tile_source != nullptr ? input->tile_source : "unknown", input->tile_selection_reason != nullptr ? input->tile_selection_reason : "unknown",
                   input->parser_used ? 1U : 0U, input->parser_status, input->selected_obu_type, input->tile_group_count, input->va_tile_count, input->parsed_tile_count,
                   input->tile_ranges_equivalent ? 1U : 0U, pic->base_qindex, pic->y_dc_delta_q, pic->u_dc_delta_q, pic->u_ac_delta_q, pic->v_dc_delta_q, pic->v_ac_delta_q,
                   pic->qmatrix_fields.bits.using_qmatrix ? 1U : 0U, std_data->quantization.flags.diff_uv_delta ? 1U : 0U, pic->qmatrix_fields.bits.qm_y,
                   pic->qmatrix_fields.bits.qm_u, pic->qmatrix_fields.bits.qm_v, pic->pic_info_fields.bits.error_resilient_mode ? 1U : 0U,
                   pic->pic_info_fields.bits.disable_cdf_update ? 1U : 0U, pic->pic_info_fields.bits.disable_frame_end_update_cdf ? 1U : 0U,
                   pic->pic_info_fields.bits.allow_intrabc ? 1U : 0U, pic->pic_info_fields.bits.allow_warped_motion ? 1U : 0U,
                   pic->pic_info_fields.bits.allow_high_precision_mv ? 1U : 0U, pic->pic_info_fields.bits.is_motion_mode_switchable ? 1U : 0U,
                   pic->pic_info_fields.bits.use_ref_frame_mvs ? 1U : 0U, pic->mode_control_fields.bits.reference_select ? 1U : 0U,
                   pic->mode_control_fields.bits.skip_mode_present ? 1U : 0U, input->header.skip_mode_frame[0], input->header.skip_mode_frame[1], pic->interp_filter,
                   std_data->picture.interpolation_filter, pic->mode_control_fields.bits.tx_mode, std_data->picture.TxMode,
                   pic->seg_info.segment_info_fields.bits.enabled ? 1U : 0U, pic->seg_info.segment_info_fields.bits.update_map ? 1U : 0U,
                   pic->seg_info.segment_info_fields.bits.temporal_update ? 1U : 0U, pic->seg_info.segment_info_fields.bits.update_data ? 1U : 0U,
                   av1_segmentation_feature_mask_or(pic), static_cast<unsigned long long>(av1_segmentation_feature_data_hash(pic)),
                   pic->loop_filter_info_fields.bits.mode_ref_delta_enabled ? 1U : 0U, pic->loop_filter_info_fields.bits.mode_ref_delta_update ? 1U : 0U,
                   pic->filter_level[0], pic->filter_level[1], pic->filter_level_u, pic->filter_level_v, input->sequence.enable_cdef ? 1U : 0U,
                   pic->cdef_damping_minus_3, pic->cdef_bits, input->sequence.enable_restoration ? 1U : 0U, std_data->picture.flags.UsesLr ? 1U : 0U,
                   std_data->picture.flags.usesChromaLr ? 1U : 0U, std_data->restoration.FrameRestorationType[0], std_data->restoration.FrameRestorationType[1],
                   std_data->restoration.FrameRestorationType[2], std_data->restoration.LoopRestorationSize[0], std_data->restoration.LoopRestorationSize[1],
                   std_data->restoration.LoopRestorationSize[2]);
    }

    uint64_t av1_metadata_fingerprint(const SurfaceResource* resource, const VkvvAV1DecodeInput* input, uint64_t frame_seq, int target_dpb_slot) {
        uint64_t hash = 1469598103934665603ULL;
        hash          = fnv1a64(hash, frame_seq);
        hash          = fnv1a64(hash, resource != nullptr ? resource->surface_id : VA_INVALID_ID);
        hash          = fnv1a64(hash, resource != nullptr ? resource->content_generation + 1 : 0);
        hash          = fnv1a64(hash, input != nullptr ? input->pic->order_hint : 0);
        hash          = fnv1a64(hash, input != nullptr ? input->header.refresh_frame_flags : 0);
        hash          = fnv1a64(hash, static_cast<uint64_t>(target_dpb_slot));
        hash          = fnv1a64(hash, resource != nullptr ? vkvv_trace_handle(resource->image) : 0);
        hash          = fnv1a64(hash, resource != nullptr ? vkvv_trace_handle(resource->export_resource.image) : 0);
        return hash;
    }

    std::string av1_bytes_hex(const uint8_t* data, size_t size, uint32_t offset, uint32_t count) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string           text;
        if (data == nullptr || size == 0 || offset >= size || count == 0) {
            return "-";
        }
        const uint32_t available = static_cast<uint32_t>(std::min<size_t>(count, size - offset));
        text.reserve(available * 2);
        for (uint32_t i = 0; i < available; i++) {
            const uint8_t byte = data[offset + i];
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 0xf]);
        }
        return text;
    }

    const VkvvAV1Tile* av1_find_tile_by_index(const VkvvAV1Tile* tiles, size_t count, uint32_t tile_index) {
        if (tiles == nullptr) {
            return nullptr;
        }
        for (size_t i = 0; i < count; i++) {
            if (tiles[i].tile_index == tile_index) {
                return &tiles[i];
            }
        }
        return nullptr;
    }

    void trace_av1_tile_submit_map(const VkvvDriver* drv, const VkvvContext* vctx, VASurfaceID target_surface_id, uint64_t frame_seq, const VkvvAV1DecodeInput* input) {
        if (input == nullptr || input->pic == nullptr || input->tiles == nullptr || input->tile_count == 0 || !av1_trace_tiles_enabled()) {
            return;
        }

        std::vector<const VkvvAV1Tile*> sorted;
        sorted.reserve(input->tile_count);
        uint64_t sum_sizes   = 0;
        uint32_t min_offset  = std::numeric_limits<uint32_t>::max();
        uint64_t max_end     = 0;
        bool     inside      = true;
        bool     overlap     = false;
        bool     gaps        = false;
        bool     zero_size   = false;
        for (size_t i = 0; i < input->tile_count; i++) {
            const VkvvAV1Tile& tile = input->tiles[i];
            sorted.push_back(&tile);
            const uint64_t end = static_cast<uint64_t>(tile.offset) + tile.size;
            sum_sizes += tile.size;
            min_offset = std::min(min_offset, tile.offset);
            max_end    = std::max(max_end, end);
            if (tile.size == 0 || tile.offset >= input->bitstream_size || end > input->bitstream_size) {
                inside = false;
            }
            zero_size |= tile.size == 0;
        }
        std::sort(sorted.begin(), sorted.end(), [](const VkvvAV1Tile* a, const VkvvAV1Tile* b) { return a->offset < b->offset; });
        uint64_t previous_end = 0;
        for (const VkvvAV1Tile* tile : sorted) {
            if (previous_end != 0) {
                overlap |= tile->offset < previous_end;
                gaps |= tile->offset > previous_end;
            }
            previous_end = std::max(previous_end, static_cast<uint64_t>(tile->offset) + tile->size);
        }
        const bool implausibly_small = input->bitstream_size > 256 && sum_sizes < input->bitstream_size / 64;
        const bool suspicious        = !inside || overlap || zero_size || implausibly_small;
        VKVV_TRACE("av1-tile-submit-map",
                   "scope=frame frame_seq=%llu driver=%llu stream=%llu surface=%u codec=0x%x profile=%u bit_depth=%u coded_width=%u coded_height=%u visible_width=%u "
                   "visible_height=%u show_frame=%u show_existing_frame=%u showable_frame=%u refresh_frame_flags=0x%02x order_hint=%u frame_type=%u bitstream_size=%zu "
                   "frame_header_offset=%u tile_count=%zu tile_cols=%u tile_rows=%u tile_source=%s selection_reason=%s parser_used=%u parser_status=%d selected_obu_type=%u "
                   "tile_group_count=%u va_tile_count=%zu parsed_tile_count=%zu sum_final_tile_sizes=%llu min_final_tile_offset=%u max_final_tile_end=%llu "
                   "ranges_inside_bitstream=%u ranges_overlap=%u ranges_have_gaps=%u tile_ranges_valid=%u suspicious=%u implausibly_small=%u",
                   static_cast<unsigned long long>(frame_seq), drv != nullptr ? static_cast<unsigned long long>(drv->driver_instance_id) : 0ULL,
                   vctx != nullptr ? static_cast<unsigned long long>(vctx->stream_id) : 0ULL, target_surface_id, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, input->pic->profile,
                   input->bit_depth, round_up_16(input->frame_width), round_up_16(input->frame_height), input->frame_width, input->frame_height,
                   input->header.show_frame ? 1U : 0U, input->header.show_existing_frame ? 1U : 0U, input->header.showable_frame ? 1U : 0U,
                   input->header.refresh_frame_flags, input->pic->order_hint, input->pic->pic_info_fields.bits.frame_type, input->bitstream_size, input->header.frame_header_offset,
                   input->tile_count, input->pic->tile_cols, input->pic->tile_rows, input->tile_source != nullptr ? input->tile_source : "unknown",
                   input->tile_selection_reason != nullptr ? input->tile_selection_reason : "unknown", input->parser_used ? 1U : 0U, input->parser_status,
                   input->selected_obu_type, input->tile_group_count, input->va_tile_count, input->parsed_tile_count, static_cast<unsigned long long>(sum_sizes),
                   min_offset == std::numeric_limits<uint32_t>::max() ? 0U : min_offset, static_cast<unsigned long long>(max_end), inside ? 1U : 0U, overlap ? 1U : 0U,
                   gaps ? 1U : 0U, !suspicious ? 1U : 0U, suspicious ? 1U : 0U, implausibly_small ? 1U : 0U);

        previous_end = 0;
        for (size_t i = 0; i < input->tile_count; i++) {
            const VkvvAV1Tile& tile     = input->tiles[i];
            const uint64_t     tile_end = static_cast<uint64_t>(tile.offset) + tile.size;
            const bool         tile_inside = tile.size != 0 && tile.offset < input->bitstream_size && tile_end <= input->bitstream_size;
            const bool         tile_overlap = i > 0 && tile.offset < previous_end;
            const auto*        va_tile      = av1_find_tile_by_index(input->va_tiles, input->va_tile_count, tile.tile_index);
            const auto*        parsed_tile  = av1_find_tile_by_index(input->parsed_tiles, input->parsed_tile_count, tile.tile_index);
            const std::string  first_bytes  = av1_bytes_hex(input->bitstream, input->bitstream_size, tile.offset, 16);
            const uint32_t     last_offset  = tile.size > 16 ? tile.offset + tile.size - 16 : tile.offset;
            const std::string  last_bytes   = av1_bytes_hex(input->bitstream, input->bitstream_size, last_offset, 16);
            VKVV_TRACE("av1-tile-submit-map",
                       "scope=tile frame_seq=%llu surface=%u tile_index=%u tile_row=%u tile_col=%u va_slice_index=%u va_offset=%u va_size=%u va_data_offset=%u "
                       "va_data_size=%u parsed_obu_index=%u parsed_obu_offset=%u parsed_obu_size=%u parsed_payload_offset=%u parsed_entry_offset=%u parsed_entry_size=%u "
                       "final_vk_offset=%u final_vk_size=%u final_range_end=%llu inside_bitstream=%u overlap_previous=%u first_16_bytes_hex=%s last_16_bytes_hex=%s",
                       static_cast<unsigned long long>(frame_seq), target_surface_id, tile.tile_index, tile.param.tile_row, tile.param.tile_column,
                       va_tile != nullptr ? va_tile->va_slice_index : tile.va_slice_index, va_tile != nullptr ? va_tile->offset : tile.va_offset,
                       va_tile != nullptr ? va_tile->size : tile.va_size, va_tile != nullptr ? va_tile->va_data_offset : tile.va_data_offset,
                       va_tile != nullptr ? va_tile->va_data_size : tile.va_data_size, parsed_tile != nullptr ? parsed_tile->parsed_obu_index : tile.parsed_obu_index,
                       parsed_tile != nullptr ? parsed_tile->parsed_obu_offset : tile.parsed_obu_offset,
                       parsed_tile != nullptr ? parsed_tile->parsed_obu_size : tile.parsed_obu_size,
                       parsed_tile != nullptr ? parsed_tile->parsed_payload_offset : tile.parsed_payload_offset,
                       parsed_tile != nullptr ? parsed_tile->parsed_entry_offset : tile.parsed_entry_offset,
                       parsed_tile != nullptr ? parsed_tile->parsed_entry_size : tile.parsed_entry_size, tile.offset, tile.size, static_cast<unsigned long long>(tile_end),
                       tile_inside ? 1U : 0U, tile_overlap ? 1U : 0U, first_bytes.c_str(), last_bytes.c_str());
            if (va_tile != nullptr || parsed_tile != nullptr) {
                const uint32_t va_offset     = va_tile != nullptr ? va_tile->offset : 0;
                const uint32_t va_size       = va_tile != nullptr ? va_tile->size : 0;
                const uint32_t parsed_offset = parsed_tile != nullptr ? parsed_tile->offset : 0;
                const uint32_t parsed_size   = parsed_tile != nullptr ? parsed_tile->size : 0;
                VKVV_TRACE("av1-tile-source-compare",
                           "frame_seq=%llu surface=%u tile_index=%u va_offset=%u va_size=%u parsed_offset=%u parsed_size=%u offset_delta=%d size_delta=%d same_offset=%u "
                           "same_size=%u tile_source=%s selection_reason=%s",
                           static_cast<unsigned long long>(frame_seq), target_surface_id, tile.tile_index, va_offset, va_size, parsed_offset, parsed_size,
                           static_cast<int32_t>(parsed_offset) - static_cast<int32_t>(va_offset), static_cast<int32_t>(parsed_size) - static_cast<int32_t>(va_size),
                           va_tile != nullptr && parsed_tile != nullptr && va_offset == parsed_offset ? 1U : 0U,
                           va_tile != nullptr && parsed_tile != nullptr && va_size == parsed_size ? 1U : 0U, input->tile_source != nullptr ? input->tile_source : "unknown",
                           input->tile_selection_reason != nullptr ? input->tile_selection_reason : "unknown");
            }
            previous_end = std::max(previous_end, tile_end);
        }
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

    void trace_av1_dpb_map(const char* event, const VkvvDriver* drv, const VkvvContext* vctx, VASurfaceID target_surface_id, uint64_t frame_seq,
                           const AV1VideoSession* session, const VkvvAV1DecodeInput* input,
                           const int32_t reference_name_slot_indices[VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR], int target_dpb_slot, bool has_setup_slot,
                           bool current_updates_reference_map, bool references_valid, uint32_t reference_count) {
        if (event == nullptr || session == nullptr || input == nullptr || input->pic == nullptr || !av1_trace_dpb_enabled()) {
            return;
        }

        VKVV_TRACE(event,
                   "scope=frame frame_seq=%llu driver=%llu stream=%llu surface=%u session_generation=0 sequence_generation=0 order_hint=%u frame_id=%u show_frame=%u "
                   "show_existing_frame=%u showable_frame=%u refresh_frame_flags=0x%02x frame_type=%u target_dpb_slot=%d setup_slot=%d scratch_slot=%u stores_as_reference=%u "
                   "references_valid=%u reference_count=%u",
                   static_cast<unsigned long long>(frame_seq), drv != nullptr ? static_cast<unsigned long long>(drv->driver_instance_id) : 0ULL,
                   vctx != nullptr ? static_cast<unsigned long long>(vctx->stream_id) : 0ULL, target_surface_id, input->pic->order_hint, input->header.current_frame_id,
                   input->header.show_frame ? 1U : 0U, input->header.show_existing_frame ? 1U : 0U, input->header.showable_frame ? 1U : 0U,
                   input->header.refresh_frame_flags, input->pic->pic_info_fields.bits.frame_type, target_dpb_slot, has_setup_slot ? target_dpb_slot : -1,
                   has_setup_slot && !current_updates_reference_map ? 1U : 0U, current_updates_reference_map ? 1U : 0U, references_valid ? 1U : 0U, reference_count);

        for (uint32_t i = 0; i < max_av1_reference_slots; i++) {
            const AV1ReferenceSlot& entry = session->reference_slots[i];
            VKVV_TRACE(event,
                       "scope=map frame_seq=%llu map_index=%u surface=%u vulkan_dpb_slot=%d order_hint=%u frame_id=%u showable=%u content_generation=%llu "
                       "session_generation=0 sequence_generation=0 valid=%u",
                       static_cast<unsigned long long>(frame_seq), i, entry.surface_id, entry.slot, entry.info.OrderHint,
                       entry.has_metadata ? entry.metadata.frame_id : 0U, entry.has_metadata && entry.metadata.showable ? 1U : 0U,
                       entry.has_metadata ? static_cast<unsigned long long>(entry.metadata.content_generation) : 0ULL,
                       entry.surface_id != VA_INVALID_ID && entry.slot >= 0 ? 1U : 0U);
        }

        if (reference_name_slot_indices == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
            const uint8_t ref_index   = av1_va_ref_frame_index(input->pic, i);
            const bool    valid_index = ref_index < max_av1_reference_slots;
            const auto*   slot        = valid_index ? av1_reference_slot_for_index(session, ref_index) : nullptr;
            VKVV_TRACE(event,
                       "scope=active frame_seq=%llu ref_name=%u ref_name_index=%u ref_frame_map_index=%u vulkan_dpb_slot=%d surface=%u surface_stream=%llu surface_codec=0x%x "
                       "surface_content_generation=%llu surface_decode_generation=%llu surface_session_generation=0 order_hint=%u frame_id=%u valid=%u reason_if_invalid=%s",
                       static_cast<unsigned long long>(frame_seq), i, i, valid_index ? ref_index : 0U, reference_name_slot_indices[i],
                       slot != nullptr ? slot->surface_id : VA_INVALID_ID, slot != nullptr && slot->has_metadata ? static_cast<unsigned long long>(slot->metadata.stream_id) : 0ULL,
                       slot != nullptr && slot->has_metadata ? slot->metadata.codec_operation : 0U,
                       slot != nullptr && slot->has_metadata ? static_cast<unsigned long long>(slot->metadata.content_generation) : 0ULL,
                       slot != nullptr && slot->has_metadata ? static_cast<unsigned long long>(slot->metadata.content_generation) : 0ULL,
                       slot != nullptr ? slot->info.OrderHint : 0U, slot != nullptr && slot->has_metadata ? slot->metadata.frame_id : 0U,
                       slot != nullptr && reference_name_slot_indices[i] >= 0 ? 1U : 0U,
                       !valid_index ? "invalid-ref-index" : slot == nullptr ? "missing-slot" : reference_name_slot_indices[i] < 0 ? "missing-dpb-slot" : "none");
        }
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
    const VASurfaceID target_surface_id = vctx->render_target;
    if (target_surface_id == VA_INVALID_ID) {
        std::snprintf(reason, reason_size, "missing AV1 target surface id");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (av1_target_surface_needs_detach(session, input, target_surface_id)) {
        VAStatus detach_status = complete_pending_surface_work_if_needed(runtime, target, "AV1 target reference detach", reason, reason_size);
        if (detach_status != VA_STATUS_SUCCESS) {
            return detach_status;
        }
        av1_detach_target_dpb_resource(runtime, session, target, target_surface_id);
    }
    if (!ensure_surface_resource(runtime, target, decode_key, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    bool used_slots[max_av1_dpb_slots] = {};

    struct ReferenceRecord {
        VkvvSurface*                   surface  = nullptr;
        SurfaceResource*               resource = nullptr;
        uint32_t                       reference_name = 0;
        uint32_t                       ref_frame_map_index = 0;
        uint32_t                       frame_id = 0;
        bool                           showable = false;
        bool                           displayed = false;
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
    const uint64_t                                         frame_seq                                                              = ++session->frame_sequence;

    if (trace_deep_enabled) {
        const std::string ref_map              = av1_ref_frame_map_string(input->pic);
        const std::string ref_slots_before     = av1_reference_slots_string(session);
        const std::string surface_slots_before = av1_surface_slots_string(session);
        VKVV_TRACE("av1-frame-enter",
                   "frame_seq=%llu driver=%llu ctx_stream=%llu target=%u current_frame=%u order_hint=%u frame_type=%u show=%u hdr_existing=%u hdr_show=%u hdr_showable=%u refresh_export=%u "
                   "refresh=0x%02x primary_ref=%u depth=%u fourcc=0x%x bitstream=%zu header=%u tiles=%zu "
                   "ref_map=\"%s\" ref_slots=\"%s\" surface_slots=\"%s\"",
                   static_cast<unsigned long long>(frame_seq), static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id),
                   target_surface_id, input->pic->current_frame, input->pic->order_hint, input->pic->pic_info_fields.bits.frame_type, input->pic->pic_info_fields.bits.show_frame,
                   input->header.show_existing_frame ? 1U : 0U, input->header.show_frame ? 1U : 0U, input->header.showable_frame ? 1U : 0U, refresh_export ? 1U : 0U,
                   input->header.refresh_frame_flags,
                   input->pic->primary_ref_frame, input->bit_depth, input->fourcc, input->bitstream_size, input->header.frame_header_offset, input->tile_count, ref_map.c_str(),
                   ref_slots_before.c_str(), surface_slots_before.c_str());
    }

    if (input->header.show_existing_frame) {
        if (av1_env_flag_enabled("VKVV_AV1_DISABLE_SHOW_EXISTING_FASTPATH")) {
            VKVV_TRACE("av1-show-existing-disabled",
                       "frame_seq=%llu driver=%llu ctx_stream=%llu target=%u map_idx=%d disable_show_existing_fastpath=1",
                       static_cast<unsigned long long>(frame_seq), static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id),
                       target_surface_id, input->header.frame_to_show_map_idx);
            std::snprintf(reason, reason_size, "AV1 show-existing fastpath disabled by VKVV_AV1_DISABLE_SHOW_EXISTING_FASTPATH");
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
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

        auto* ref_resource = av1_retained_dpb_resource_for_slot(session, show_slot->slot);
        if (ref_resource == nullptr) {
            ref_resource = static_cast<SurfaceResource*>(ref_surface->vulkan);
        }
        show_slot = validate_av1_show_existing_reference(session, input, ref_surface, ref_resource, drv, vctx, decode_key, reason, reason_size);
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

        const uint32_t fingerprint_level = av1_fingerprint_level();
        const uint64_t show_fingerprint = fingerprint_level > 0 ? av1_metadata_fingerprint(target_resource, input, frame_seq, show_slot->slot) : 0;
        const uint64_t previous_decode_fingerprint = target_resource->av1_decode_fingerprint;
        target_resource->av1_frame_sequence     = frame_seq;
        target_resource->av1_order_hint         = input->pic->order_hint;
        target_resource->av1_frame_type         = input->pic->pic_info_fields.bits.frame_type;
        target_resource->av1_tile_count         = 0;
        target_resource->av1_tile_sum_size      = 0;
        target_resource->av1_tile_ranges_valid  = true;
        target_resource->av1_target_dpb_slot    = show_slot->slot;
        target_resource->av1_setup_slot         = show_slot->slot;
        target_resource->av1_references_valid   = true;
        target_resource->av1_reference_count    = 1;
        target_resource->av1_decode_fingerprint = show_fingerprint;
        target_resource->av1_tile_source        = "show-existing";
        if (trace_deep_enabled) {
            trace_av1_order_hint_state(drv, vctx, session, input, target_surface_id, frame_seq, true, 1, show_slot->slot);
        }
        if (fingerprint_level > 0) {
            VKVV_TRACE("av1-decode-fingerprint",
                       "frame_seq=%llu surface=%u content_generation=%llu order_hint=%u show_frame=%u refresh_frame_flags=0x%02x decode_image_handle=0x%llx "
                       "decode_image_layout=%d decode_crc_valid=1 decode_y_tl_crc=0 decode_y_center_crc=0 decode_y_br_crc=0 decode_uv_center_crc=0 decode_combined_crc=0x%llx "
                       "same_as_previous_decode=%u same_as_previous_visible_decode=%u fingerprint_level=%u fingerprint_kind=metadata",
                       static_cast<unsigned long long>(frame_seq), target_surface_id, static_cast<unsigned long long>(target_resource->content_generation), input->pic->order_hint,
                       input->header.show_frame ? 1U : 0U, input->header.refresh_frame_flags, vkvv_trace_handle(target_resource->image), target_resource->layout,
                       static_cast<unsigned long long>(show_fingerprint), previous_decode_fingerprint == show_fingerprint ? 1U : 0U,
                       target_resource->av1_previous_visible_fingerprint == show_fingerprint ? 1U : 0U, fingerprint_level);
        }

        set_av1_visible_output_trace(target_resource, input, true);
        VAStatus export_status = vkvv_vulkan_refresh_surface_export(runtime, target, true, reason, reason_size);
        if (export_status != VA_STATUS_SUCCESS) {
            return export_status;
        }

        trace_av1_display_decision(drv, vctx, target_surface_id, show_slot->surface_id, input, target_resource, true, "show-existing-refresh");
        VKVV_TRACE("av1-show-existing", "driver=%llu ctx_stream=%llu target=%u source=%u map_idx=%d slot=%d display_frame_id=%u target_gen=%llu source_gen=%llu refresh_export=1",
                   static_cast<unsigned long long>(drv->driver_instance_id), static_cast<unsigned long long>(vctx->stream_id), target_surface_id, show_slot->surface_id,
                   input->header.frame_to_show_map_idx, show_slot->slot, input->header.display_frame_id, static_cast<unsigned long long>(target_resource->content_generation),
                   static_cast<unsigned long long>(ref_resource->content_generation));
        remember_av1_visible_order_hint(session, input, target_surface_id, frame_seq, true);
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
            const auto*             ref_resource   = named_slot != nullptr ? av1_retained_dpb_resource_for_slot(session, named_slot->slot) : nullptr;
            if (ref_resource == nullptr) {
                ref_resource = ref_surface != nullptr ? static_cast<const SurfaceResource*>(ref_surface->vulkan) : nullptr;
            }
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
            auto*                   resource     = av1_retained_dpb_resource_for_slot(session, ref_dpb_slot);
            if (resource == nullptr) {
                resource = static_cast<SurfaceResource*>(ref_surface->vulkan);
            }
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
            record.reference_name              = i;
            record.ref_frame_map_index         = reference_index;
            record.frame_id                    = stored_slot->has_metadata ? stored_slot->metadata.frame_id : 0;
            record.showable                    = stored_slot->has_metadata && stored_slot->metadata.showable;
            record.displayed                   = stored_slot->has_metadata && stored_slot->metadata.displayed;
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
            av1_clear_surface_slot(session, target_surface_id);
            av1_release_unreferenced_retained_dpb_resources(runtime, session);
        }
    }
    if (trace_deep_enabled) {
        trace_av1_order_hint_state(drv, vctx, session, input, target_surface_id, frame_seq, refresh_export, reference_count, target_dpb_slot);
    }
    trace_av1_dpb_map("av1-dpb-map-before-submit", drv, vctx, target_surface_id, frame_seq, session, input, reference_name_slot_indices, target_dpb_slot, has_setup_slot,
                      current_updates_reference_map, true, reference_count);

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
    const bool     trace_enabled     = vkvv_trace_enabled();
    const uint32_t tile_sum_size     = av1_tile_sum_size_for_trace(input);
    const uint64_t tile_bytes_hash   = trace_enabled ? av1_tile_bytes_hash_for_trace(input) : 0;
    const uint64_t bitstream_hash    = trace_enabled ? av1_bitstream_hash_for_trace(input) : 0;
    trace_av1_tile_submit_map(drv, vctx, target_surface_id, frame_seq, input);
    trace_av1_picture_params(drv, vctx, target_surface_id, frame_seq, input, &std_data, reference_count, target_dpb_slot, has_setup_slot, refresh_export, tile_sum_size,
                             tile_bytes_hash, bitstream_hash);
    if (trace_enabled) {
        for (uint32_t i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; i++) {
            const uint8_t              ref_index      = av1_va_ref_frame_index(input->pic, i);
            const int32_t              ref_slot       = reference_name_slot_indices[i];
            const VASurfaceID          ref_surface_id = ref_index < max_av1_reference_slots ? input->pic->ref_frame_map[ref_index] : VA_INVALID_ID;
            const ReferenceRecord*     record         = nullptr;
            const AV1ReferenceSlot*    named_slot     = ref_index < max_av1_reference_slots ? av1_reference_slot_for_index(session, ref_index) : nullptr;
            for (uint32_t j = 0; j < reference_count; j++) {
                if (references[j].slot.slotIndex == ref_slot) {
                    record = &references[j];
                    break;
                }
            }
            const SurfaceResource* resource = record != nullptr ? record->resource : nullptr;
            VKVV_TRACE("av1-reference-params",
                       "frame_seq=%llu target=%u reference_name=%u reference_ref_idx=%u reference_slot=%d reference_surface=%u named_surface=%u valid=%u order_hint=%u "
                       "reference_frame_type=%u reference_frame_id=%u reference_showable=%u reference_displayed=%u reference_content_gen=%llu "
                       "current_resource_content_gen=%llu matches_target_surface=%u",
                       static_cast<unsigned long long>(frame_seq), target_surface_id, i, ref_index, ref_slot, ref_surface_id,
                       named_slot != nullptr ? named_slot->surface_id : VA_INVALID_ID, record != nullptr && resource != nullptr ? 1U : 0U,
                       record != nullptr ? record->std_ref.OrderHint : named_slot != nullptr ? named_slot->info.OrderHint : 0U,
                       record != nullptr ? record->std_ref.frame_type : named_slot != nullptr ? named_slot->info.frame_type : 0U,
                       record != nullptr ? record->frame_id : named_slot != nullptr && named_slot->has_metadata ? named_slot->metadata.frame_id : 0U,
                       record != nullptr && record->showable ? 1U : named_slot != nullptr && named_slot->has_metadata && named_slot->metadata.showable ? 1U : 0U,
                       record != nullptr && record->displayed ? 1U : named_slot != nullptr && named_slot->has_metadata && named_slot->metadata.displayed ? 1U : 0U,
                       record != nullptr && resource != nullptr ? static_cast<unsigned long long>(record->resource->content_generation) : 0ULL,
                       resource != nullptr ? static_cast<unsigned long long>(resource->content_generation) : 0ULL, ref_surface_id == target_surface_id ? 1U : 0U);
        }
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
    trace_av1_dpb_map("av1-dpb-map-after-submit", drv, vctx, target_surface_id, frame_seq, session, input, reference_name_slot_indices, target_dpb_slot, has_setup_slot,
                      current_updates_reference_map, true, reference_count);

    const AV1ReferenceMetadata current_metadata = build_current_reference_metadata(drv, vctx, target, target_resource, decode_key, input, refresh_export);
    if (current_updates_reference_map) {
        av1_release_retained_dpb_resource(runtime, session, target_dpb_slot);
        av1_update_reference_slots_from_refresh(session, input, target_surface_id, target_dpb_slot, setup_std_ref, &current_metadata);
        av1_release_unreferenced_retained_dpb_resources(runtime, session);
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
    trace_av1_dpb_map("av1-dpb-map-after-refresh", drv, vctx, target_surface_id, frame_seq, session, input, reference_name_slot_indices, target_dpb_slot, has_setup_slot,
                      current_updates_reference_map, true, reference_count);

    const uint32_t fingerprint_level = av1_fingerprint_level();
    const uint64_t decode_fingerprint = fingerprint_level > 0 ? av1_metadata_fingerprint(target_resource, input, frame_seq, target_dpb_slot) : 0;
    const uint64_t previous_decode_fingerprint = target_resource->av1_decode_fingerprint;
    target_resource->av1_frame_sequence       = frame_seq;
    target_resource->av1_order_hint           = input->pic->order_hint;
    target_resource->av1_frame_type           = input->pic->pic_info_fields.bits.frame_type;
    target_resource->av1_tile_count           = static_cast<uint32_t>(input->tile_count);
    target_resource->av1_tile_sum_size        = tile_sum_size;
    target_resource->av1_tile_ranges_valid    = true;
    target_resource->av1_target_dpb_slot      = target_dpb_slot;
    target_resource->av1_setup_slot           = has_setup_slot ? target_dpb_slot : -1;
    target_resource->av1_references_valid     = true;
    target_resource->av1_reference_count      = reference_count;
    target_resource->av1_decode_fingerprint   = decode_fingerprint;
    target_resource->av1_tile_source          = input->tile_source != nullptr ? input->tile_source : "unknown";
    Av1PendingDecodeTrace av1_pending_trace{};
    av1_pending_trace.valid                 = true;
    av1_pending_trace.frame_sequence        = frame_seq;
    av1_pending_trace.order_hint            = input->pic->order_hint;
    av1_pending_trace.frame_type            = input->pic->pic_info_fields.bits.frame_type;
    av1_pending_trace.show_frame            = input->header.show_frame;
    av1_pending_trace.showable_frame        = input->header.showable_frame;
    av1_pending_trace.show_existing_frame   = input->header.show_existing_frame;
    av1_pending_trace.refresh_frame_flags   = input->header.refresh_frame_flags;
    av1_pending_trace.frame_to_show_map_idx = input->header.frame_to_show_map_idx;
    av1_pending_trace.target_dpb_slot       = target_dpb_slot;
    av1_pending_trace.setup_slot            = has_setup_slot ? target_dpb_slot : -1;
    av1_pending_trace.reference_count       = reference_count;
    PendingDecodeTrace pending_trace{};
    pending_trace.valid                  = true;
    pending_trace.reference_count        = reference_count;
    pending_trace.traced_reference_count = std::min<uint32_t>(reference_count, av1_pending_reference_trace_capacity);
    fill_av1_pending_parameter_trace(&av1_pending_trace, input, &std_data, tile_sum_size, tile_bytes_hash, bitstream_hash);
    const uint32_t traced_reference_count   = std::min<uint32_t>(reference_count, av1_pending_reference_trace_capacity);
    for (uint32_t i = 0; i < traced_reference_count; i++) {
        av1_pending_trace.references[i].resource           = references[i].resource;
        av1_pending_trace.references[i].surface_id         = references[i].surface != nullptr ? references[i].surface->id : VA_INVALID_ID;
        av1_pending_trace.references[i].dpb_slot           = references[i].slot.slotIndex;
        av1_pending_trace.references[i].reference_name     = references[i].reference_name;
        av1_pending_trace.references[i].ref_frame_map_index = references[i].ref_frame_map_index;
        av1_pending_trace.references[i].order_hint         = references[i].std_ref.OrderHint;
        av1_pending_trace.references[i].frame_type         = references[i].std_ref.frame_type;
        av1_pending_trace.references[i].frame_id           = references[i].frame_id;
        av1_pending_trace.references[i].content_generation = references[i].resource != nullptr ? references[i].resource->content_generation : 0;
        av1_pending_trace.references[i].showable           = references[i].showable;
        av1_pending_trace.references[i].displayed          = references[i].displayed;
        av1_pending_trace.references[i].valid              = references[i].resource != nullptr;
    }
    if (fingerprint_level > 0) {
        VKVV_TRACE("av1-decode-fingerprint",
                   "frame_seq=%llu surface=%u content_generation=%llu order_hint=%u show_frame=%u refresh_frame_flags=0x%02x decode_image_handle=0x%llx "
                   "decode_image_layout=%d decode_crc_valid=1 decode_y_tl_crc=0 decode_y_center_crc=0 decode_y_br_crc=0 decode_uv_center_crc=0 decode_combined_crc=0x%llx "
                   "same_as_previous_decode=%u same_as_previous_visible_decode=%u fingerprint_level=%u fingerprint_kind=metadata",
                   static_cast<unsigned long long>(frame_seq), target_surface_id, static_cast<unsigned long long>(target_resource->content_generation + 1), input->pic->order_hint,
                   input->header.show_frame ? 1U : 0U, input->header.refresh_frame_flags, vkvv_trace_handle(target_resource->image), target_resource->layout,
                   static_cast<unsigned long long>(decode_fingerprint), previous_decode_fingerprint == decode_fingerprint ? 1U : 0U,
                   target_resource->av1_previous_visible_fingerprint == decode_fingerprint ? 1U : 0U, fingerprint_level);
    }

    set_av1_visible_output_trace(target_resource, input, refresh_export);
    pending_trace.visible   = target_resource->visible_output_trace;
    pending_trace.av1_trace = av1_pending_trace;
    track_pending_decode(runtime, target, parameters, upload_allocation_size, refresh_export, "AV1 decode", &pending_trace);
    trace_av1_display_decision(drv, vctx, target_surface_id, refresh_export ? target_surface_id : VA_INVALID_ID, input, target_resource, refresh_export,
                               refresh_export ? "decode-display-queued" : "decode-reference-only");
    remember_av1_visible_order_hint(session, input, target_surface_id, frame_seq, refresh_export);
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
