#include "vulkan/export/internal.h"
#include "telemetry.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include <drm_fourcc.h>

namespace vkvv {

    void add_raw_image_barrier(std::vector<VkImageMemoryBarrier2>* barriers, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                               VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

    VkvvPixelProofMode export_pixel_proof_mode();
    bool               export_pixel_proof_enabled();
    bool               export_seed_pixel_proof_required();
    bool               export_visible_pixel_proof_required();

    bool     readback_image_luma_sample(VulkanRuntime* runtime, VkImage image, VkImageLayout* layout, const ExportFormatInfo* format, VkExtent2D extent, const char* label,
                                        uint64_t* crc, VkDeviceSize* sample_bytes, char* reason, size_t reason_size);
    uint64_t pixel_reference_crc(const ExportFormatInfo* format, VkExtent2D extent, bool black);
    bool     pixel_proof_valid_for_source(VkvvExportPixelSource source, bool crc_valid, bool is_black, bool is_zero);
    VkvvPixelProofState pixel_proof_state_from_sample(bool sample_ok, uint64_t crc, uint64_t black_crc, uint64_t zero_crc, bool is_black, bool is_zero);
    bool                pixel_identity_valid_from_state(VkvvPixelProofState state);
    VkvvPixelColorState pixel_color_state_from_sample(bool sample_ok, bool is_black, bool is_zero);
    void                trace_pixel_proof_computed(VASurfaceID surface_id, uint64_t fd_dev, uint64_t fd_ino, uint64_t generation, VkvvExportPixelSource pixel_source, uint64_t crc,
                                                   uint64_t black_crc, uint64_t zero_crc, bool is_black, bool is_zero, VkDeviceSize sample_bytes, VkvvPixelProofState state);

    struct SeedPixelProofState {
        bool                valid                 = false;
        bool                identity_valid        = false;
        bool                content_valid         = false;
        bool                source_is_placeholder = false;
        bool                is_black              = false;
        bool                is_zero               = false;
        uint64_t            crc                   = 0;
        VkDeviceSize        sample_bytes          = 0;
        VkvvPixelProofState state                 = VkvvPixelProofState::Unknown;
        VkvvPixelColorState color_state           = VkvvPixelColorState::Unknown;
    };
    SeedPixelProofState predecode_seed_source_pixel_proof_state(const SurfaceResource* source);

    void                trace_export_present_state(const SurfaceResource* owner, const ExportResource* resource, const char* action, bool refresh_export, bool display_visible) {
        if (owner == nullptr || resource == nullptr) {
            return;
        }
        const char* mutation_action = action != nullptr && std::strcmp(action, "nondisplay-present-pinned-skip") == 0 ? "skipped-client-shadow" : "none";
        VKVV_TRACE("export-present-state",
                   "action=%s surface=%u codec=0x%x stream=%llu fd_dev=%llu fd_ino=%llu content_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu "
                   "decode_shadow_gen=%llu fd_exported=%u fd_content_gen=%llu may_be_sampled_by_client=%u detached_from_surface=%u present_gen=%llu presentable=%u "
                   "present_pinned=%u published_visible=%u decode_shadow_private_active=%u predecode=%u seeded=%u "
                   "predecode_quarantined=%u predecode_generation=%llu placeholder=%u export_role=%s export_intent=%s raw_export_flags=0x%x mem_type=0x%x had_va_begin=%u "
                   "had_decode_submit=%u refresh_export=%u display_visible=%u present_source=%s mutation_action=%s "
                   "client_visible_shadow_mutated=0 client_visible_shadow=%u private_only=%u external_release_required=%u external_release_done=%u external_release_mode=%s "
                   "external_released_generation=%llu",
                   action != nullptr ? action : "unknown", owner->surface_id, owner->codec_operation, static_cast<unsigned long long>(owner->stream_id),
                   static_cast<unsigned long long>(resource->fd_dev), static_cast<unsigned long long>(resource->fd_ino), static_cast<unsigned long long>(owner->content_generation),
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(owner->private_decode_shadow.content_generation),
                   static_cast<unsigned long long>(resource->decode_shadow_generation), resource->exported_fd.fd_exported ? 1U : 0U,
                   static_cast<unsigned long long>(resource->exported_fd.fd_content_generation), export_resource_fd_may_be_sampled_by_client(resource) ? 1U : 0U,
                   resource->exported_fd.detached_from_surface ? 1U : 0U, static_cast<unsigned long long>(resource->present_generation), resource->presentable ? 1U : 0U,
                   resource->present_pinned ? 1U : 0U, resource->published_visible ? 1U : 0U, resource->decode_shadow_private_active ? 1U : 0U,
                   resource->predecode_exported ? 1U : 0U, resource->predecode_seeded ? 1U : 0U, resource->predecode_quarantined ? 1U : 0U,
                   static_cast<unsigned long long>(resource->predecode_generation), resource->black_placeholder ? 1U : 0U, vkvv_export_role_name(resource->export_role),
                   vkvv_export_intent_name(resource->export_intent), resource->raw_export_flags, resource->export_mem_type, resource->predecode_had_va_begin ? 1U : 0U,
                   resource->predecode_had_decode_submit ? 1U : 0U, refresh_export ? 1U : 0U, display_visible ? 1U : 0U, vkvv_export_present_source_name(resource->present_source),
                   mutation_action, resource->client_visible_shadow ? 1U : 0U, resource->private_nondisplay_shadow ? 1U : 0U,
                   resource->external_sync.external_release_required ? 1U : 0U, resource->external_sync.external_release_done ? 1U : 0U,
                   vkvv_external_release_mode_name(resource->external_sync.release_mode), static_cast<unsigned long long>(resource->external_sync.released_generation));
        if (resource->content_generation == 0 && resource->presentable) {
            VKVV_TRACE("invalid-presentable-undecoded-surface",
                       "surface=%u codec=0x%x stream=%llu content_gen=%llu shadow_gen=%llu present_gen=%llu presentable=1 present_pinned=%u action=%s", owner->surface_id,
                       owner->codec_operation, static_cast<unsigned long long>(owner->stream_id), static_cast<unsigned long long>(owner->content_generation),
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->present_generation),
                       resource->present_pinned ? 1U : 0U, action != nullptr ? action : "unknown");
        }
        if (resource->present_generation > owner->content_generation) {
            VKVV_TRACE("invalid-present-generation",
                       "surface=%u codec=0x%x stream=%llu content_gen=%llu shadow_gen=%llu present_gen=%llu presentable=%u present_pinned=%u action=%s", owner->surface_id,
                       owner->codec_operation, static_cast<unsigned long long>(owner->stream_id), static_cast<unsigned long long>(owner->content_generation),
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->present_generation),
                       resource->presentable ? 1U : 0U, resource->present_pinned ? 1U : 0U, action != nullptr ? action : "unknown");
        }
    }

    void trace_predecode_target_present_state(const ExportResource* resource, const char* action) {
        if (resource == nullptr) {
            return;
        }
        VKVV_TRACE("export-present-state",
                   "action=%s surface=%u codec=0x%x stream=%llu fd_dev=%llu fd_ino=%llu content_gen=0 present_shadow_gen=%llu private_shadow_gen=0 "
                   "decode_shadow_gen=%llu fd_exported=%u fd_content_gen=%llu may_be_sampled_by_client=%u detached_from_surface=%u present_gen=%llu presentable=%u "
                   "present_pinned=%u published_visible=%u decode_shadow_private_active=%u predecode=%u seeded=%u predecode_quarantined=%u predecode_generation=%llu "
                   "placeholder=%u export_role=%s export_intent=%s raw_export_flags=0x%x mem_type=0x%x had_va_begin=%u had_decode_submit=%u refresh_export=1 display_visible=0 "
                   "present_source=%s mutation_action=none client_visible_shadow_mutated=0 client_visible_shadow=%u private_only=0 external_release_required=%u "
                   "external_release_done=%u external_release_mode=%s external_released_generation=%llu",
                   action != nullptr ? action : "predecode-target-seeded", resource->owner_surface_id, resource->codec_operation,
                   static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->fd_dev),
                   static_cast<unsigned long long>(resource->fd_ino), static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(resource->decode_shadow_generation), resource->exported_fd.fd_exported ? 1U : 0U,
                   static_cast<unsigned long long>(resource->exported_fd.fd_content_generation), export_resource_fd_may_be_sampled_by_client(resource) ? 1U : 0U,
                   resource->exported_fd.detached_from_surface ? 1U : 0U, static_cast<unsigned long long>(resource->present_generation), resource->presentable ? 1U : 0U,
                   resource->present_pinned ? 1U : 0U, resource->published_visible ? 1U : 0U, resource->decode_shadow_private_active ? 1U : 0U,
                   resource->predecode_exported ? 1U : 0U, resource->predecode_seeded ? 1U : 0U, resource->predecode_quarantined ? 1U : 0U,
                   static_cast<unsigned long long>(resource->predecode_generation), resource->black_placeholder ? 1U : 0U, vkvv_export_role_name(resource->export_role),
                   vkvv_export_intent_name(resource->export_intent), resource->raw_export_flags, resource->export_mem_type, resource->predecode_had_va_begin ? 1U : 0U,
                   resource->predecode_had_decode_submit ? 1U : 0U, vkvv_export_present_source_name(resource->present_source), resource->client_visible_shadow ? 1U : 0U,
                   resource->external_sync.external_release_required ? 1U : 0U, resource->external_sync.external_release_done ? 1U : 0U,
                   vkvv_external_release_mode_name(resource->external_sync.release_mode), static_cast<unsigned long long>(resource->external_sync.released_generation));
    }

    void trace_exported_fd_freshness_check(const SurfaceResource* owner, const ExportResource* resource, bool refresh_export, bool display_visible, const char* action) {
        if (owner == nullptr || resource == nullptr) {
            return;
        }
        const bool may_sample = export_resource_fd_may_be_sampled_by_client(resource);
        const bool stale      = may_sample && owner->content_generation > resource->exported_fd.fd_content_generation;
        VKVV_TRACE("exported-fd-freshness-check",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu fd_content_gen=%llu last_written_content_gen=%llu "
                   "may_be_sampled_by_client=%u detached_from_surface=%u refresh_export=%u display_visible=%u action=%s",
                   owner->surface_id, static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                   static_cast<unsigned long long>(resource->exported_fd.fd_dev), static_cast<unsigned long long>(resource->exported_fd.fd_ino),
                   static_cast<unsigned long long>(owner->content_generation), static_cast<unsigned long long>(resource->exported_fd.fd_content_generation),
                   static_cast<unsigned long long>(resource->exported_fd.last_written_content_generation), may_sample ? 1U : 0U,
                   resource->exported_fd.detached_from_surface ? 1U : 0U, refresh_export ? 1U : 0U, display_visible ? 1U : 0U, action != nullptr ? action : "unknown");
        if (stale) {
            VKVV_TRACE("invalid-stale-exported-fd", "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu fd_content_gen=%llu action=%s", owner->surface_id,
                       static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                       static_cast<unsigned long long>(owner->content_generation), static_cast<unsigned long long>(resource->exported_fd.fd_content_generation),
                       action != nullptr ? action : "unknown");
        }
    }

    namespace {

        struct ScopedUploadBuffer {
            VulkanRuntime* runtime = nullptr;
            UploadBuffer   buffer{};

            explicit ScopedUploadBuffer(VulkanRuntime* owner) : runtime(owner) {}
            ScopedUploadBuffer(const ScopedUploadBuffer&)            = delete;
            ScopedUploadBuffer& operator=(const ScopedUploadBuffer&) = delete;
            ~ScopedUploadBuffer() {
                destroy_upload_buffer(runtime, &buffer);
            }
        };

        uint32_t export_plane_bytes_per_pixel(uint32_t drm_format) {
            switch (drm_format) {
                case DRM_FORMAT_R8: return 1;
                case DRM_FORMAT_GR88:
                case DRM_FORMAT_R16: return 2;
                case DRM_FORMAT_GR1616: return 4;
                default: return 0;
            }
        }

        void store_u16_le(uint8_t* dst, uint16_t value) {
            dst[0] = static_cast<uint8_t>(value & 0xffu);
            dst[1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
        }

        bool fill_export_black_plane(uint8_t* dst, VkExtent3D extent, uint32_t drm_format, char* reason, size_t reason_size) {
            const size_t samples = static_cast<size_t>(extent.width) * extent.height;
            switch (drm_format) {
                case DRM_FORMAT_R8: std::memset(dst, 16, samples); return true;
                case DRM_FORMAT_GR88:
                    for (size_t i = 0; i < samples; i++) {
                        dst[(i * 2) + 0] = 128;
                        dst[(i * 2) + 1] = 128;
                    }
                    return true;
                case DRM_FORMAT_R16:
                    for (size_t i = 0; i < samples; i++) {
                        store_u16_le(dst + (i * 2), static_cast<uint16_t>(64u << 6u));
                    }
                    return true;
                case DRM_FORMAT_GR1616:
                    for (size_t i = 0; i < samples; i++) {
                        store_u16_le(dst + (i * 4) + 0, static_cast<uint16_t>(512u << 6u));
                        store_u16_le(dst + (i * 4) + 2, static_cast<uint16_t>(512u << 6u));
                    }
                    return true;
                default: std::snprintf(reason, reason_size, "unsupported export shadow plane format 0x%x", drm_format); return false;
            }
        }

        bool create_staging_transfer_buffer(VulkanRuntime* runtime, VkDeviceSize size, VkBufferUsageFlags usage, UploadBuffer* staging, const char* label, char* reason,
                                            size_t reason_size) {
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size        = size;
            buffer_info.usage       = usage;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &staging->buffer);
            if (!record_vk_result(runtime, result, "vkCreateBuffer", label, reason, reason_size)) {
                return false;
            }

            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(runtime->device, staging->buffer, &requirements);
            staging->size            = size;
            staging->capacity        = size;
            staging->allocation_size = requirements.size;

            uint32_t memory_type_index = 0;
            staging->coherent          = true;
            if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &memory_type_index)) {
                staging->coherent = false;
                if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index)) {
                    destroy_upload_buffer(runtime, staging);
                    std::snprintf(reason, reason_size, "no host-visible memory type for %s", label != nullptr ? label : "transfer staging");
                    return false;
                }
            }

            VkMemoryAllocateInfo allocate_info{};
            allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate_info.allocationSize  = requirements.size;
            allocate_info.memoryTypeIndex = memory_type_index;
            result                        = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &staging->memory);
            if (!record_vk_result(runtime, result, "vkAllocateMemory", label, reason, reason_size)) {
                destroy_upload_buffer(runtime, staging);
                return false;
            }

            result = vkBindBufferMemory(runtime->device, staging->buffer, staging->memory, 0);
            if (!record_vk_result(runtime, result, "vkBindBufferMemory", label, reason, reason_size)) {
                destroy_upload_buffer(runtime, staging);
                return false;
            }
            return true;
        }

        bool predecode_seed_target_thumbnail_like(const ExportResource* target) {
            return target != nullptr && target->extent.width <= 960 && target->extent.height <= 540 && target->content_generation == 0;
        }

        bool initialize_export_resource_black(VulkanRuntime* runtime, ExportResource* resource, const ExportFormatInfo* format, char* reason, size_t reason_size) {
            if (resource == nullptr || resource->image == VK_NULL_HANDLE || format == nullptr) {
                std::snprintf(reason, reason_size, "missing export shadow init resource");
                return false;
            }

            VkBufferImageCopy regions[2]{};
            VkExtent3D        plane_extents[2]{};
            uint32_t          plane_formats[2]{};
            VkDeviceSize      staging_size = 0;
            for (uint32_t i = 0; i < format->layer_count; i++) {
                const uint32_t bytes_per_pixel = export_plane_bytes_per_pixel(format->layers[i].drm_format);
                if (bytes_per_pixel == 0) {
                    std::snprintf(reason, reason_size, "unsupported export shadow plane format 0x%x", format->layers[i].drm_format);
                    return false;
                }
                plane_extents[i]                       = export_layer_extent(resource->extent, format->layers[i]);
                plane_formats[i]                       = format->layers[i].drm_format;
                staging_size                           = align_up(staging_size, 16);
                regions[i].bufferOffset                = staging_size;
                regions[i].bufferRowLength             = 0;
                regions[i].bufferImageHeight           = 0;
                regions[i].imageSubresource.aspectMask = format->layers[i].aspect;
                regions[i].imageSubresource.layerCount = 1;
                regions[i].imageExtent                 = plane_extents[i];
                staging_size += static_cast<VkDeviceSize>(plane_extents[i].width) * plane_extents[i].height * bytes_per_pixel;
            }

            ScopedUploadBuffer staging(runtime);
            if (!create_staging_transfer_buffer(runtime, staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging.buffer, "export shadow init", reason, reason_size)) {
                return false;
            }

            VkResult result = vkMapMemory(runtime->device, staging.buffer.memory, 0, staging.buffer.size, 0, &staging.buffer.mapped);
            if (!record_vk_result(runtime, result, "vkMapMemory", "export shadow init", reason, reason_size)) {
                return false;
            }
            auto* mapped = static_cast<uint8_t*>(staging.buffer.mapped);
            std::memset(mapped, 0, static_cast<size_t>(staging.buffer.size));
            for (uint32_t i = 0; i < format->layer_count; i++) {
                if (!fill_export_black_plane(mapped + regions[i].bufferOffset, plane_extents[i], plane_formats[i], reason, reason_size)) {
                    return false;
                }
            }
            if (!staging.buffer.coherent) {
                VkMappedMemoryRange range{};
                range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = staging.buffer.memory;
                range.offset = 0;
                range.size   = VK_WHOLE_SIZE;
                vkFlushMappedMemoryRanges(runtime->device, 1, &range);
            }
            vkUnmapMemory(runtime->device, staging.buffer.memory);
            staging.buffer.mapped = nullptr;

            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            if (!ensure_command_resources(runtime, reason, reason_size)) {
                return false;
            }

            result = vkResetFences(runtime->device, 1, &runtime->fence);
            if (!record_vk_result(runtime, result, "vkResetFences", "export shadow init", reason, reason_size)) {
                return false;
            }
            result = vkResetCommandBuffer(runtime->command_buffer, 0);
            if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "export shadow init", reason, reason_size)) {
                return false;
            }

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
            if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "export shadow init", reason, reason_size)) {
                return false;
            }

            std::vector<VkImageMemoryBarrier2> barriers;
            add_raw_image_barrier(&barriers, resource->image, resource->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            if (!barriers.empty()) {
                VkDependencyInfo dependency{};
                dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
                dependency.pImageMemoryBarriers    = barriers.data();
                vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
            }
            resource->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            vkCmdCopyBufferToImage(runtime->command_buffer, staging.buffer.buffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, format->layer_count, regions);

            barriers.clear();
            add_raw_image_barrier(&barriers, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
            if (!barriers.empty()) {
                VkDependencyInfo dependency{};
                dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
                dependency.pImageMemoryBarriers    = barriers.data();
                vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
            }
            resource->layout = VK_IMAGE_LAYOUT_GENERAL;

            result = vkEndCommandBuffer(runtime->command_buffer);
            if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "export shadow init", reason, reason_size)) {
                return false;
            }
            if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "export shadow init", CommandUse::Export)) {
                return false;
            }

            resource->content_generation     = 0;
            resource->predecode_exported     = false;
            resource->predecode_seeded       = false;
            resource->black_placeholder      = true;
            resource->seed_source_surface_id = VA_INVALID_ID;
            resource->seed_source_generation = 0;
            resource->seed_pixel_proof_valid = false;
            mark_export_predecode_nonpresentable(resource);
            return true;
        }

    } // namespace

    bool create_image_resource_with_tiling(VulkanRuntime* runtime, ExportResource* resource, const ExportFormatInfo* format, VkExtent2D extent, VkImageTiling tiling,
                                           VkImageUsageFlags usage, bool export_memory, const char* trace_role, char* reason, size_t reason_size) {
        if (format == nullptr) {
            std::snprintf(reason, reason_size, "missing surface export format");
            return false;
        }

        VkExternalMemoryImageCreateInfo external_image{};
        external_image.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        std::vector<uint64_t>                     drm_modifiers;
        VkImageDrmFormatModifierListCreateInfoEXT drm_modifier_list{};
        drm_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            if (!enumerate_drm_format_modifiers(runtime, format->vk_format, VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT, &drm_modifiers)) {
                std::snprintf(reason, reason_size, "no DRM format modifiers support transfer-dst export images");
                return false;
            }
            drm_modifier_list.drmFormatModifierCount = static_cast<uint32_t>(drm_modifiers.size());
            drm_modifier_list.pDrmFormatModifiers    = drm_modifiers.data();
            external_image.pNext                     = &drm_modifier_list;
        }

        VkImageCreateInfo image_info{};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext         = export_memory ? &external_image : nullptr;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.format        = format->vk_format;
        image_info.extent        = {extent.width, extent.height, 1};
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling        = tiling;
        image_info.usage         = usage;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
        if (!record_vk_result(runtime, result, "vkCreateImage", trace_role, reason, reason_size)) {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(runtime->device, resource->image, &requirements);

        uint32_t memory_type_index = 0;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
            !find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, 0, &memory_type_index)) {
            destroy_export_resource(runtime, resource);
            std::snprintf(reason, reason_size, "no memory type for export shadow image");
            return false;
        }

        VkMemoryDedicatedAllocateInfo dedicated_allocate{};
        dedicated_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated_allocate.image = resource->image;

        VkExportMemoryAllocateInfo export_allocate{};
        export_allocate.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_allocate.pNext       = &dedicated_allocate;
        export_allocate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.pNext           = export_memory ? static_cast<const void*>(&export_allocate) : static_cast<const void*>(&dedicated_allocate);
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;

        result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &resource->memory);
        if (!record_vk_result(runtime, result, "vkAllocateMemory", trace_role, reason, reason_size)) {
            destroy_export_resource(runtime, resource);
            return false;
        }

        result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
        if (!record_vk_result(runtime, result, "vkBindImageMemory", trace_role, reason, reason_size)) {
            destroy_export_resource(runtime, resource);
            return false;
        }

        if (tiling != VK_IMAGE_TILING_OPTIMAL) {
            for (uint32_t i = 0; i < format->layer_count; i++) {
                VkImageSubresource plane{};
                plane.aspectMask = format->layers[i].aspect;
                vkGetImageSubresourceLayout(runtime->device, resource->image, &plane, &resource->plane_layouts[i]);
            }
        }

        resource->extent             = extent;
        resource->format             = format->vk_format;
        resource->va_fourcc          = format->va_fourcc;
        resource->allocation_size    = requirements.size;
        resource->plane_count        = format->layer_count;
        resource->content_generation = 0;
        VKVV_TRACE("export-image-create", "role=%s format=%d fourcc=0x%x extent=%ux%u exportable_memory=%u export_mem=%llu planes=%u", trace_role, resource->format,
                   resource->va_fourcc, resource->extent.width, resource->extent.height, export_memory ? 1U : 0U, static_cast<unsigned long long>(resource->allocation_size),
                   resource->plane_count);
        if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
            modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
            result                    = runtime->get_image_drm_format_modifier_properties(runtime->device, resource->image, &modifier_properties);
            if (!record_vk_result(runtime, result, "vkGetImageDrmFormatModifierPropertiesEXT", trace_role, reason, reason_size)) {
                destroy_export_resource(runtime, resource);
                return false;
            }
            resource->drm_format_modifier     = modifier_properties.drmFormatModifier;
            resource->has_drm_format_modifier = true;
        } else {
            resource->drm_format_modifier     = DRM_FORMAT_MOD_LINEAR;
            resource->has_drm_format_modifier = true;
        }
        return true;
    }

    bool create_export_resource_with_tiling(VulkanRuntime* runtime, ExportResource* resource, const ExportFormatInfo* format, VkExtent2D extent, VkImageTiling tiling, char* reason,
                                            size_t reason_size) {
        return create_image_resource_with_tiling(runtime, resource, format, extent, tiling, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true,
                                                 "export shadow image", reason, reason_size);
    }

    bool ensure_export_resource(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export shadow")) {
            return false;
        }
        const ExportFormatInfo* format = export_format_for_surface(nullptr, source, reason, reason_size);
        if (format == nullptr) {
            return false;
        }

        ExportResource* resource = &source->export_resource;
        if (resource->image != VK_NULL_HANDLE && resource->driver_instance_id == source->driver_instance_id && resource->stream_id == source->stream_id &&
            resource->codec_operation == source->codec_operation && resource->format == source->format && resource->va_fourcc == source->va_fourcc &&
            resource->extent.width >= source->coded_extent.width && resource->extent.height >= source->coded_extent.height) {
            resource->driver_instance_id = source->driver_instance_id;
            resource->stream_id          = source->stream_id;
            resource->codec_operation    = source->codec_operation;
            resource->owner_surface_id   = source->surface_id;
            return true;
        }

        if (resource->image == VK_NULL_HANDLE && source->surface_id != VA_INVALID_ID) {
            bool reattached = false;
            prune_detached_exports_for_surface(runtime, source->driver_instance_id, source->surface_id, source->stream_id, source->codec_operation, source->va_fourcc,
                                               source->format, source->coded_extent);
            {
                std::lock_guard<std::mutex> lock(runtime->export_mutex);
                for (auto it = runtime->retained_exports.begin(); it != runtime->retained_exports.end();) {
                    ExportResource& retained = it->resource;
                    if (retained.private_nondisplay_shadow || !retained.exported || !retained.client_visible_shadow) {
                        const VkDeviceSize bytes = retained.allocation_size;
                        VKVV_TRACE("retained-role-mismatch-drop", "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx exported=%u client_visible=%u private_only=%u",
                                   retained.owner_surface_id, static_cast<unsigned long long>(retained.driver_instance_id), static_cast<unsigned long long>(retained.stream_id),
                                   retained.codec_operation, vkvv_trace_handle(retained.memory), retained.exported ? 1U : 0U, retained.client_visible_shadow ? 1U : 0U,
                                   retained.private_nondisplay_shadow ? 1U : 0U);
                        destroy_export_resource(runtime, &retained);
                        runtime->retained_export_memory_bytes = runtime->retained_export_memory_bytes > bytes ? runtime->retained_export_memory_bytes - bytes : 0;
                        it                                    = runtime->retained_exports.erase(it);
                        continue;
                    }
                    if (retained.driver_instance_id == source->driver_instance_id && retained.owner_surface_id == source->surface_id && retained.stream_id == source->stream_id &&
                        retained.codec_operation == source->codec_operation && retained.format == source->format && retained.va_fourcc == source->va_fourcc &&
                        retained.extent.width == source->coded_extent.width && retained.extent.height == source->coded_extent.height) {
                        *resource                        = retained;
                        resource->driver_instance_id     = source->driver_instance_id;
                        resource->stream_id              = source->stream_id;
                        resource->codec_operation        = source->codec_operation;
                        resource->owner_surface_id       = source->surface_id;
                        resource->content_generation     = 0;
                        source->export_retained_attached = true;
                        source->export_import_attached   = false;
                        it->state                        = RetainedExportState::Attached;
                        runtime->retained_export_memory_bytes =
                            runtime->retained_export_memory_bytes > resource->allocation_size ? runtime->retained_export_memory_bytes - resource->allocation_size : 0;
                        trace_export_fd_lifetime(source, resource, "retained-attach", resource->content_generation, export_resource_fd_may_be_sampled_by_client(resource));
                        VKVV_TRACE("retained-present-shadow-attach",
                                   "surface=%u old_owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx content_gen=%llu present_gen=%llu client_visible=1 private_only=0",
                                   source->surface_id, retained.owner_surface_id, static_cast<unsigned long long>(retained.driver_instance_id),
                                   static_cast<unsigned long long>(retained.stream_id), retained.codec_operation, vkvv_trace_handle(retained.memory),
                                   static_cast<unsigned long long>(retained.content_generation), static_cast<unsigned long long>(retained.present_generation));
                        runtime->retained_exports.erase(it);
                        note_retained_export_attached_locked(runtime);
                        reattached = true;
                        break;
                    }
                    ++it;
                }
            }
            if (reattached) {
                if (!initialize_export_resource_black(runtime, resource, format, reason, reason_size)) {
                    destroy_export_resource(runtime, resource);
                    return false;
                }
                return true;
            }
        }

        detach_export_resource(runtime, source);
        clear_surface_export_attach_state(source);
        if (runtime->image_drm_format_modifier &&
            create_export_resource_with_tiling(runtime, resource, format, source->coded_extent, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, reason, reason_size)) {
            resource->driver_instance_id = source->driver_instance_id;
            resource->stream_id          = source->stream_id;
            resource->codec_operation    = source->codec_operation;
            resource->owner_surface_id   = source->surface_id;
            if (initialize_export_resource_black(runtime, resource, format, reason, reason_size)) {
                return true;
            }
        }
        destroy_export_resource(runtime, resource);
        if (!create_export_resource_with_tiling(runtime, resource, format, source->coded_extent, VK_IMAGE_TILING_LINEAR, reason, reason_size)) {
            return false;
        }
        resource->driver_instance_id = source->driver_instance_id;
        resource->stream_id          = source->stream_id;
        resource->codec_operation    = source->codec_operation;
        resource->owner_surface_id   = source->surface_id;
        if (!initialize_export_resource_black(runtime, resource, format, reason, reason_size)) {
            destroy_export_resource(runtime, resource);
            return false;
        }
        return true;
    }

    bool private_decode_shadow_matches_surface(const ExportResource& shadow, const SurfaceResource* source) {
        return source != nullptr && shadow.image != VK_NULL_HANDLE && shadow.memory != VK_NULL_HANDLE && shadow.driver_instance_id == source->driver_instance_id &&
            shadow.stream_id == source->stream_id && shadow.codec_operation == source->codec_operation && shadow.format == source->format &&
            shadow.va_fourcc == source->va_fourcc && shadow.extent.width >= source->coded_extent.width && shadow.extent.height >= source->coded_extent.height && !shadow.exported &&
            shadow.private_nondisplay_shadow && !shadow.client_visible_shadow;
    }

    bool ensure_private_decode_shadow(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, "private decode shadow")) {
            return false;
        }
        if (source == nullptr || source->image == VK_NULL_HANDLE || source->content_generation == 0) {
            std::snprintf(reason, reason_size, "missing decoded source for private decode shadow");
            return false;
        }
        const ExportFormatInfo* format = export_format_for_surface(nullptr, source, reason, reason_size);
        if (format == nullptr) {
            return false;
        }

        ExportResource* shadow = &source->private_decode_shadow;
        if (private_decode_shadow_matches_surface(*shadow, source)) {
            shadow->driver_instance_id = source->driver_instance_id;
            shadow->stream_id          = source->stream_id;
            shadow->codec_operation    = source->codec_operation;
            shadow->owner_surface_id   = source->surface_id;
            return true;
        }

        if (shadow->image != VK_NULL_HANDLE || shadow->memory != VK_NULL_HANDLE) {
            destroy_export_resource(runtime, shadow);
        }

        constexpr VkImageUsageFlags private_shadow_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!create_image_resource_with_tiling(runtime, shadow, format, source->coded_extent, VK_IMAGE_TILING_OPTIMAL, private_shadow_usage, false, "private decode shadow image",
                                               reason, reason_size)) {
            destroy_export_resource(runtime, shadow);
            if (!create_image_resource_with_tiling(runtime, shadow, format, source->coded_extent, VK_IMAGE_TILING_LINEAR, private_shadow_usage, false,
                                                   "private decode shadow image", reason, reason_size)) {
                return false;
            }
        }

        shadow->driver_instance_id                           = source->driver_instance_id;
        shadow->stream_id                                    = source->stream_id;
        shadow->codec_operation                              = source->codec_operation;
        shadow->owner_surface_id                             = source->surface_id;
        shadow->exported                                     = false;
        shadow->client_visible_shadow                        = false;
        shadow->private_nondisplay_shadow                    = true;
        shadow->present_source                               = VkvvExportPresentSource::PrivateNondisplay;
        shadow->decode_shadow_generation                     = 0;
        shadow->decode_shadow_private_active                 = false;
        source->export_resource.decode_shadow_private_active = false;
        VKVV_TRACE("private-decode-shadow-create",
                   "surface=%u driver=%llu stream=%llu codec=0x%x format=%d fourcc=0x%x extent=%ux%u memory_size=%llu private_only=1 exported=0 image=0x%llx memory=0x%llx",
                   source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   shadow->format, shadow->va_fourcc, shadow->extent.width, shadow->extent.height, static_cast<unsigned long long>(shadow->allocation_size),
                   vkvv_trace_handle(shadow->image), vkvv_trace_handle(shadow->memory));
        return true;
    }

    bool copy_decode_to_private_shadow(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size) {
        if (runtime == nullptr || source == nullptr || source->image == VK_NULL_HANDLE || source->content_generation == 0) {
            std::snprintf(reason, reason_size, "missing private decode shadow copy source");
            return false;
        }
        if (!ensure_private_decode_shadow(runtime, source, reason, reason_size)) {
            return false;
        }

        ExportResource*         target = &source->private_decode_shadow;
        const ExportFormatInfo* format = export_format_for_surface(nullptr, source, reason, reason_size);
        if (format == nullptr) {
            return false;
        }

        const VkImage        source_image              = source->image;
        const uint64_t       source_content_generation = source->content_generation;
        const VkImage        target_image              = target->image;
        const VkDeviceMemory target_memory             = target->memory;
        const uint64_t       private_shadow_before     = target->content_generation;
        const bool           trace_enabled             = vkvv_trace_enabled();
        VKVV_TRACE("private-decode-shadow-copy-enter",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu present_gen=%llu present_shadow_gen=%llu private_shadow_gen_before=%llu "
                   "decode_shadow_gen=%llu source_image=0x%llx private_image=0x%llx private_memory=0x%llx",
                   source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(source->export_resource.present_generation),
                   static_cast<unsigned long long>(source->export_resource.content_generation), static_cast<unsigned long long>(private_shadow_before),
                   static_cast<unsigned long long>(source->export_resource.decode_shadow_generation), vkvv_trace_handle(source->image), vkvv_trace_handle(target->image),
                   vkvv_trace_handle(target->memory));

        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "private decode shadow copy", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "private decode shadow copy", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "private decode shadow copy", reason, reason_size)) {
            return false;
        }

        std::vector<VkImageMemoryBarrier2> barriers;
        add_raw_image_barrier(&barriers, source->image, source->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_raw_image_barrier(&barriers, target->image, target->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
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
            regions[i].extent                    = export_layer_extent(source->coded_extent, format->layers[i]);
        }
        vkCmdCopyImage(runtime->command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, format->layer_count,
                       regions);

        barriers.clear();
        add_raw_image_barrier(&barriers, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                              VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
        add_raw_image_barrier(&barriers, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        source->layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        target->layout = VK_IMAGE_LAYOUT_GENERAL;

        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "private decode shadow copy", reason, reason_size)) {
            return false;
        }

        const auto wait_start = trace_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "private decode shadow copy", CommandUse::Export)) {
            return false;
        }
        if (source->image != source_image || source->content_generation != source_content_generation || target->image != target_image || target->memory != target_memory) {
            std::snprintf(reason, reason_size, "private decode shadow copy target changed before publish");
            return false;
        }

        target->content_generation                           = source->content_generation;
        target->driver_instance_id                           = source->driver_instance_id;
        target->stream_id                                    = source->stream_id;
        target->codec_operation                              = source->codec_operation;
        target->owner_surface_id                             = source->surface_id;
        target->predecode_exported                           = false;
        target->predecode_seeded                             = false;
        target->black_placeholder                            = false;
        target->seed_source_surface_id                       = VA_INVALID_ID;
        target->seed_source_generation                       = 0;
        target->seed_pixel_proof_valid                       = false;
        target->client_visible_shadow                        = false;
        target->private_nondisplay_shadow                    = true;
        target->present_source                               = VkvvExportPresentSource::PrivateNondisplay;
        source->export_resource.decode_shadow_generation     = source->content_generation;
        source->export_resource.decode_shadow_private_active = true;

        uint64_t wait_ns = 0;
        if (trace_enabled) {
            wait_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count());
        }
        VKVV_TRACE("private-decode-shadow-copy-done",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu present_gen=%llu present_shadow_gen=%llu private_shadow_gen_before=%llu "
                   "private_shadow_gen_after=%llu decode_shadow_gen=%llu copy_reason=%s copy_bytes=%llu wait_ns=%llu",
                   source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(source->export_resource.present_generation),
                   static_cast<unsigned long long>(source->export_resource.content_generation), static_cast<unsigned long long>(private_shadow_before),
                   static_cast<unsigned long long>(target->content_generation), static_cast<unsigned long long>(source->export_resource.decode_shadow_generation),
                   vkvv_export_copy_reason_name(VkvvExportCopyReason::NondisplayPrivateRefresh), static_cast<unsigned long long>(target->allocation_size),
                   static_cast<unsigned long long>(wait_ns));
        return true;
    }

    bool trace_visible_pixel_proof(VulkanRuntime* runtime, SurfaceResource* source, char*, size_t) {
        if (!export_pixel_proof_enabled() || source == nullptr || source->content_generation == 0 || source->export_resource.image == VK_NULL_HANDLE) {
            return true;
        }

        char                    proof_reason[256] = {};
        const ExportFormatInfo* format            = export_format_for_surface(nullptr, source, proof_reason, sizeof(proof_reason));
        uint64_t                decode_crc        = 0;
        uint64_t                present_crc       = 0;
        VkDeviceSize            decode_bytes      = 0;
        VkDeviceSize            present_bytes     = 0;
        const bool              decode_ok         = format != nullptr &&
            readback_image_luma_sample(runtime, source->image, &source->layout, format, source->coded_extent, "decode pixel proof", &decode_crc, &decode_bytes, proof_reason,
                                       sizeof(proof_reason));
        const bool present_ok = format != nullptr &&
            readback_image_luma_sample(runtime, source->export_resource.image, &source->export_resource.layout, format, source->export_resource.extent, "present pixel proof",
                                       &present_crc, &present_bytes, proof_reason, sizeof(proof_reason));

        source->decode_pixel_proof_valid       = decode_ok;
        source->present_pixel_proof_valid      = present_ok;
        source->decode_pixel_crc               = decode_ok ? decode_crc : 0;
        source->present_pixel_crc              = present_ok ? present_crc : 0;
        source->present_pixel_matches_decode   = decode_ok && present_ok && decode_crc == present_crc;
        source->present_pixel_matches_previous = present_ok && source->previous_present_pixel_crc != 0 && source->previous_present_pixel_crc == source->present_pixel_crc;

        const uint64_t            order_hint_or_frame_num = surface_resource_uses_av1_decode(source) ? source->av1_order_hint : source->content_generation;
        const uint64_t            black_crc               = pixel_reference_crc(format, source->coded_extent, true);
        const uint64_t            zero_crc                = pixel_reference_crc(format, source->coded_extent, false);
        const bool                decode_black            = decode_ok && black_crc != 0 && source->decode_pixel_crc == black_crc;
        const bool                decode_zero             = decode_ok && zero_crc != 0 && source->decode_pixel_crc == zero_crc;
        const bool                present_black           = present_ok && black_crc != 0 && source->present_pixel_crc == black_crc;
        const bool                present_zero            = present_ok && zero_crc != 0 && source->present_pixel_crc == zero_crc;
        const VkvvPixelProofState decode_state            = pixel_proof_state_from_sample(decode_ok, source->decode_pixel_crc, black_crc, zero_crc, decode_black, decode_zero);
        const VkvvPixelProofState present_state           = pixel_proof_state_from_sample(present_ok, source->present_pixel_crc, black_crc, zero_crc, present_black, present_zero);
        const bool                decode_identity_valid   = pixel_identity_valid_from_state(decode_state);
        const bool                present_identity_valid  = pixel_identity_valid_from_state(present_state);
        trace_pixel_proof_computed(source->surface_id, source->export_resource.fd_dev, source->export_resource.fd_ino, source->content_generation,
                                   VkvvExportPixelSource::DecodedContent, decode_ok ? source->decode_pixel_crc : 0, black_crc, zero_crc, decode_black, decode_zero, decode_bytes,
                                   decode_state);
        trace_pixel_proof_computed(source->surface_id, source->export_resource.fd_dev, source->export_resource.fd_ino, source->export_resource.present_generation,
                                   VkvvExportPixelSource::DecodedContent, present_ok ? source->present_pixel_crc : 0, black_crc, zero_crc, present_black, present_zero,
                                   present_bytes, present_state);
        VKVV_TRACE("decode-pixel-proof",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu order_hint_or_frame_num=%llu decode_crc_valid=%u decode_crc=0x%llx black_crc=0x%llx "
                   "zero_crc=0x%llx is_black=%u is_zero=%u pixel_proof_valid=%u sample_bytes=%llu",
                   source->surface_id, source->codec_operation, static_cast<unsigned long long>(source->stream_id), static_cast<unsigned long long>(source->content_generation),
                   static_cast<unsigned long long>(order_hint_or_frame_num), decode_ok ? 1U : 0U, static_cast<unsigned long long>(source->decode_pixel_crc),
                   static_cast<unsigned long long>(black_crc), static_cast<unsigned long long>(zero_crc), decode_black ? 1U : 0U, decode_zero ? 1U : 0U,
                   decode_identity_valid ? 1U : 0U, static_cast<unsigned long long>(decode_bytes));
        VKVV_TRACE("present-pixel-proof",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu fd_dev=%llu fd_ino=%llu fd_content_gen=%llu present_gen=%llu present_shadow_crc_valid=%u "
                   "present_shadow_crc=0x%llx present_crc=0x%llx decode_crc=0x%llx black_crc=0x%llx zero_crc=0x%llx previous_present_crc=0x%llx matches_decode=%u "
                   "matches_previous=%u matches_previous_present=%u is_black=%u is_zero=%u pixel_proof_valid=%u sample_bytes=%llu",
                   source->surface_id, source->codec_operation, static_cast<unsigned long long>(source->stream_id), static_cast<unsigned long long>(source->content_generation),
                   static_cast<unsigned long long>(source->export_resource.fd_dev), static_cast<unsigned long long>(source->export_resource.fd_ino),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(&source->export_resource)),
                   static_cast<unsigned long long>(source->export_resource.present_generation), present_ok ? 1U : 0U, static_cast<unsigned long long>(source->present_pixel_crc),
                   static_cast<unsigned long long>(source->present_pixel_crc), static_cast<unsigned long long>(source->decode_pixel_crc),
                   static_cast<unsigned long long>(black_crc), static_cast<unsigned long long>(zero_crc), static_cast<unsigned long long>(source->previous_present_pixel_crc),
                   source->present_pixel_matches_decode ? 1U : 0U, source->present_pixel_matches_previous ? 1U : 0U, source->present_pixel_matches_previous ? 1U : 0U,
                   present_black ? 1U : 0U, present_zero ? 1U : 0U, present_identity_valid ? 1U : 0U, static_cast<unsigned long long>(present_bytes));
        if (present_ok) {
            source->previous_present_pixel_crc = source->present_pixel_crc;
        }
        if (!decode_ok || !present_ok) {
            VKVV_TRACE("pixel-proof-unavailable", "surface=%u codec=0x%x stream=%llu proof=visible reason=\"%s\"", source->surface_id, source->codec_operation,
                       static_cast<unsigned long long>(source->stream_id), proof_reason);
        }
        return true;
    }

    bool trace_private_shadow_pixel_proof(VulkanRuntime* runtime, SurfaceResource* source, char*, size_t) {
        if (!export_pixel_proof_enabled() || source == nullptr || source->content_generation == 0 || source->private_decode_shadow.image == VK_NULL_HANDLE) {
            return true;
        }

        char                    proof_reason[256] = {};
        const ExportFormatInfo* format            = export_format_for_surface(nullptr, source, proof_reason, sizeof(proof_reason));
        uint64_t                decode_crc        = 0;
        uint64_t                private_crc       = 0;
        VkDeviceSize            decode_bytes      = 0;
        VkDeviceSize            private_bytes     = 0;
        const bool              decode_ok         = format != nullptr &&
            readback_image_luma_sample(runtime, source->image, &source->layout, format, source->coded_extent, "decode pixel proof", &decode_crc, &decode_bytes, proof_reason,
                                       sizeof(proof_reason));
        const bool private_ok = format != nullptr &&
            readback_image_luma_sample(runtime, source->private_decode_shadow.image, &source->private_decode_shadow.layout, format, source->private_decode_shadow.extent,
                                       "private shadow pixel proof", &private_crc, &private_bytes, proof_reason, sizeof(proof_reason));

        source->decode_pixel_proof_valid            = decode_ok;
        source->private_shadow_pixel_proof_valid    = private_ok;
        source->decode_pixel_crc                    = decode_ok ? decode_crc : 0;
        source->private_shadow_pixel_crc            = private_ok ? private_crc : 0;
        source->private_shadow_pixel_matches_decode = decode_ok && private_ok && decode_crc == private_crc;
        const uint64_t            black_crc         = pixel_reference_crc(format, source->coded_extent, true);
        const uint64_t            zero_crc          = pixel_reference_crc(format, source->coded_extent, false);
        const bool                private_black     = private_ok && black_crc != 0 && source->private_shadow_pixel_crc == black_crc;
        const bool                private_zero      = private_ok && zero_crc != 0 && source->private_shadow_pixel_crc == zero_crc;
        const VkvvPixelProofState private_state     = pixel_proof_state_from_sample(private_ok, source->private_shadow_pixel_crc, black_crc, zero_crc, private_black, private_zero);
        trace_pixel_proof_computed(source->surface_id, source->private_decode_shadow.fd_dev, source->private_decode_shadow.fd_ino, source->private_decode_shadow.content_generation,
                                   VkvvExportPixelSource::RetainedUnknown, private_ok ? source->private_shadow_pixel_crc : 0, black_crc, zero_crc, private_black, private_zero,
                                   private_bytes, private_state);

        VKVV_TRACE("private-shadow-pixel-proof",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu decode_crc_valid=%u decode_crc=0x%llx private_shadow_crc_valid=%u private_shadow_crc=0x%llx "
                   "matches_decode=%u decode_sample_bytes=%llu private_sample_bytes=%llu",
                   source->surface_id, source->codec_operation, static_cast<unsigned long long>(source->stream_id), static_cast<unsigned long long>(source->content_generation),
                   decode_ok ? 1U : 0U, static_cast<unsigned long long>(source->decode_pixel_crc), private_ok ? 1U : 0U,
                   static_cast<unsigned long long>(source->private_shadow_pixel_crc), source->private_shadow_pixel_matches_decode ? 1U : 0U,
                   static_cast<unsigned long long>(decode_bytes), static_cast<unsigned long long>(private_bytes));
        if (!decode_ok || !private_ok) {
            VKVV_TRACE("pixel-proof-unavailable", "surface=%u codec=0x%x stream=%llu proof=private-shadow reason=\"%s\"", source->surface_id, source->codec_operation,
                       static_cast<unsigned long long>(source->stream_id), proof_reason);
        }
        return true;
    }

    bool trace_returned_fd_pixel_proof(VulkanRuntime* runtime, const SurfaceResource* owner, ExportResource* resource, const VkvvFdIdentity& fd, VkvvExportPixelSource pixel_source,
                                       VkvvReturnedFdProof* proof, char*, size_t) {
        if (proof != nullptr) {
            *proof                             = {};
            proof->returned_fd                 = fd.valid;
            proof->fd                          = -1;
            proof->fd_dev                      = fd.dev;
            proof->fd_ino                      = fd.ino;
            proof->may_be_sampled_by_client    = export_resource_fd_may_be_sampled_by_client(resource);
            proof->surface_id                  = owner != nullptr ? owner->surface_id : VA_INVALID_ID;
            proof->stream_id                   = owner != nullptr ? owner->stream_id : (resource != nullptr ? resource->stream_id : 0);
            proof->codec_operation             = owner != nullptr ? owner->codec_operation : (resource != nullptr ? resource->codec_operation : 0);
            proof->content_generation          = owner != nullptr ? owner->content_generation : (resource != nullptr ? resource->content_generation : 0);
            proof->fd_content_generation       = export_resource_fd_content_generation(resource);
            proof->pixel_source                = pixel_source;
            proof->pixel_source_is_placeholder = pixel_source == VkvvExportPixelSource::Placeholder;
            proof->decoded_pixels_valid        = pixel_source == VkvvExportPixelSource::DecodedContent && proof->fd_content_generation != 0;
            proof->seed_pixels_valid =
                pixel_source == VkvvExportPixelSource::StreamLocalSeed && proof->fd_content_generation != 0 && resource != nullptr && resource->seed_pixel_proof_valid;
            proof->placeholder_pixels  = proof->pixel_source_is_placeholder;
            proof->pixel_content_valid = !proof->pixel_source_is_placeholder && proof->fd_content_generation != 0 &&
                (pixel_source == VkvvExportPixelSource::DecodedContent || pixel_source == VkvvExportPixelSource::StreamLocalSeed);
        }
        if (owner == nullptr || resource == nullptr || resource->image == VK_NULL_HANDLE) {
            return true;
        }

        char                    proof_reason[256] = {};
        const ExportFormatInfo* format            = export_format_for_surface(nullptr, owner, proof_reason, sizeof(proof_reason));
        const uint64_t          black_crc         = pixel_reference_crc(format, resource->extent, true);
        const uint64_t          zero_crc          = pixel_reference_crc(format, resource->extent, false);
        const bool non_sampleable_predecode_seed = pixel_source == VkvvExportPixelSource::StreamLocalSeed && resource->predecode_exported && resource->predecode_quarantined &&
            !export_resource_fd_may_be_sampled_by_client(resource);
        if (non_sampleable_predecode_seed) {
            const bool                pixel_proof_valid = resource->seed_pixel_proof_valid;
            const VkvvPixelProofState returned_state = pixel_proof_valid ? resource->seed_pixel_proof_state : VkvvPixelProofState::Unavailable;
            const uint64_t            returned_crc   = pixel_proof_valid ? resource->seed_pixel_crc : 0;
            const bool                is_black       = pixel_proof_valid && resource->seed_black_crc != 0 && returned_crc == resource->seed_black_crc;
            const bool                is_zero        = pixel_proof_valid && resource->seed_zero_crc != 0 && returned_crc == resource->seed_zero_crc;
            if (proof != nullptr) {
                proof->pixel_proof_valid           = pixel_proof_valid;
                proof->pixel_identity_valid        = pixel_proof_valid;
                proof->pixel_content_valid         = proof->fd_content_generation != 0;
                proof->pixel_source_is_placeholder = false;
                proof->pixel_color_state           = resource->seed_pixel_color_state;
                proof->pixel_crc                   = returned_crc;
                proof->black_crc                   = resource->seed_black_crc;
                proof->zero_crc                    = resource->seed_zero_crc;
                proof->seed_pixels_valid           = proof->fd_content_generation != 0 && pixel_proof_valid;
                proof->placeholder_pixels          = false;
            }
            VKVV_TRACE("returned-fd-pixel-proof",
                       "surface=%u fd_dev=%llu fd_ino=%llu stream=%llu codec=0x%x content_gen=%llu fd_content_gen=%llu pixel_source=%s returned_crc=0x%llx black_crc=0x%llx "
                       "zero_crc=0x%llx is_black=%u is_zero=%u pixel_proof_valid=%u may_be_sampled_by_client=0 returned_fd=1 sample_bytes=0 proof_enabled=%u",
                       owner->surface_id, static_cast<unsigned long long>(fd.dev), static_cast<unsigned long long>(fd.ino), static_cast<unsigned long long>(owner->stream_id),
                       owner->codec_operation, static_cast<unsigned long long>(owner->content_generation),
                       static_cast<unsigned long long>(export_resource_fd_content_generation(resource)), vkvv_export_pixel_source_name(pixel_source),
                       static_cast<unsigned long long>(returned_crc), static_cast<unsigned long long>(resource->seed_black_crc),
                       static_cast<unsigned long long>(resource->seed_zero_crc), is_black ? 1U : 0U, is_zero ? 1U : 0U, pixel_proof_valid ? 1U : 0U,
                       export_pixel_proof_enabled() ? 1U : 0U);
            trace_pixel_proof_computed(owner->surface_id, fd.dev, fd.ino, export_resource_fd_content_generation(resource), pixel_source, returned_crc, resource->seed_black_crc,
                                       resource->seed_zero_crc, is_black, is_zero, 0, returned_state);
            return true;
        }
        uint64_t                returned_crc      = 0;
        VkDeviceSize            sample_bytes      = 0;
        bool                    crc_valid         = false;
        if (export_pixel_proof_enabled() && format != nullptr) {
            crc_valid = readback_image_luma_sample(runtime, resource->image, &resource->layout, format, resource->extent, "returned fd pixel proof", &returned_crc, &sample_bytes,
                                                   proof_reason, sizeof(proof_reason));
        }
        const bool                is_black              = crc_valid && black_crc != 0 && returned_crc == black_crc;
        const bool                is_zero               = crc_valid && zero_crc != 0 && returned_crc == zero_crc;
        const bool                pixel_proof_valid     = pixel_proof_valid_for_source(pixel_source, crc_valid, is_black, is_zero);
        const VkvvPixelProofState returned_state        = pixel_proof_state_from_sample(crc_valid, returned_crc, black_crc, zero_crc, is_black, is_zero);
        const VkvvPixelColorState color_state           = pixel_color_state_from_sample(crc_valid, is_black, is_zero);
        const bool                source_is_placeholder = pixel_source == VkvvExportPixelSource::Placeholder;
        const bool                content_valid         = !source_is_placeholder && export_resource_fd_content_generation(resource) != 0 &&
            (pixel_source == VkvvExportPixelSource::DecodedContent || pixel_source == VkvvExportPixelSource::StreamLocalSeed);
        if (proof != nullptr) {
            proof->pixel_proof_valid           = pixel_proof_valid;
            proof->pixel_identity_valid        = pixel_proof_valid;
            proof->pixel_content_valid         = content_valid;
            proof->pixel_source_is_placeholder = source_is_placeholder;
            proof->pixel_color_state           = color_state;
            proof->pixel_crc                   = crc_valid ? returned_crc : 0;
            proof->black_crc                   = black_crc;
            proof->zero_crc                    = zero_crc;
            proof->decoded_pixels_valid =
                pixel_source == VkvvExportPixelSource::DecodedContent && proof->fd_content_generation != 0 && (!export_pixel_proof_enabled() || pixel_proof_valid);
            proof->seed_pixels_valid =
                pixel_source == VkvvExportPixelSource::StreamLocalSeed && proof->fd_content_generation != 0 && resource->seed_pixel_proof_valid && pixel_proof_valid;
            proof->placeholder_pixels = source_is_placeholder;
        }
        VKVV_TRACE("returned-fd-pixel-proof",
                   "surface=%u fd_dev=%llu fd_ino=%llu stream=%llu codec=0x%x content_gen=%llu fd_content_gen=%llu pixel_source=%s returned_crc=0x%llx black_crc=0x%llx "
                   "zero_crc=0x%llx is_black=%u is_zero=%u pixel_proof_valid=%u may_be_sampled_by_client=%u returned_fd=1 sample_bytes=%llu proof_enabled=%u",
                   owner->surface_id, static_cast<unsigned long long>(fd.dev), static_cast<unsigned long long>(fd.ino), static_cast<unsigned long long>(owner->stream_id),
                   owner->codec_operation, static_cast<unsigned long long>(owner->content_generation),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(resource)), vkvv_export_pixel_source_name(pixel_source),
                   static_cast<unsigned long long>(crc_valid ? returned_crc : 0), static_cast<unsigned long long>(black_crc), static_cast<unsigned long long>(zero_crc),
                   is_black ? 1U : 0U, is_zero ? 1U : 0U, pixel_proof_valid ? 1U : 0U, export_resource_fd_may_be_sampled_by_client(resource) ? 1U : 0U,
                   static_cast<unsigned long long>(sample_bytes), export_pixel_proof_enabled() ? 1U : 0U);
        trace_pixel_proof_computed(owner->surface_id, fd.dev, fd.ino, export_resource_fd_content_generation(resource), pixel_source, crc_valid ? returned_crc : 0, black_crc,
                                   zero_crc, is_black, is_zero, sample_bytes, returned_state);
        return true;
    }

    void trace_seed_pixel_proof(VulkanRuntime* runtime, SurfaceResource* source, ExportResource* target, const char* copy_status) {
        (void)runtime;
        if (target != nullptr) {
            target->seed_pixel_proof_valid = false;
            target->seed_pixel_crc         = 0;
            target->seed_black_crc         = 0;
            target->seed_zero_crc          = 0;
            target->seed_pixel_proof_state = VkvvPixelProofState::Unknown;
            target->seed_pixel_color_state = VkvvPixelColorState::Unknown;
        }
        if (source == nullptr || target == nullptr) {
            return;
        }
        char                    proof_reason[256] = {};
        const ExportFormatInfo* format            = export_format_for_surface(nullptr, source, proof_reason, sizeof(proof_reason));
        const uint64_t          black_crc         = pixel_reference_crc(format, source->coded_extent, true);
        const uint64_t          zero_crc          = pixel_reference_crc(format, source->coded_extent, false);
        const SeedPixelProofState source_proof        = predecode_seed_source_pixel_proof_state(source);
        const bool                copy_success        = copy_status == nullptr || std::strcmp(copy_status, "success") == 0;
        const bool                target_has_seed_gen = target->seed_source_generation != 0 && target->exported_fd.fd_content_generation != 0;
        const bool                target_valid        = copy_success && target_has_seed_gen && source_proof.valid;
        const bool                target_matches      = target_valid;
        const VkvvPixelProofState source_state        = source_proof.state;
        const VkvvPixelProofState target_state        = target_valid ? source_state : VkvvPixelProofState::Unavailable;
        const uint64_t            source_crc          = source_proof.valid ? source_proof.crc : 0;
        const uint64_t            target_crc          = target_valid ? source_proof.crc : 0;
        const VkDeviceSize        source_bytes        = source_proof.sample_bytes;
        const VkDeviceSize        target_bytes        = target_valid ? source_proof.sample_bytes : 0;
        const bool                source_black        = source_proof.is_black;
        const bool                source_zero         = source_proof.is_zero;
        const bool                target_black        = target_valid && source_black;
        const bool                target_zero         = target_valid && source_zero;
        const bool                source_valid        = source_proof.valid;
        const VkvvPixelColorState source_color_state  = source_proof.color_state;
        const VkvvPixelColorState target_color_state  = target_valid ? source_proof.color_state : VkvvPixelColorState::Unknown;
        target->seed_pixel_proof_valid                = target_valid;
        target->seed_pixel_crc                        = target_crc;
        target->seed_black_crc                        = black_crc;
        target->seed_zero_crc                         = zero_crc;
        target->seed_pixel_proof_state                = target_state;
        target->seed_pixel_color_state                = target_color_state;
        trace_pixel_proof_computed(source->surface_id, source->export_resource.fd_dev, source->export_resource.fd_ino, source->export_resource.present_generation,
                                   VkvvExportPixelSource::DecodedContent, source_crc, black_crc, zero_crc, source_black, source_zero, source_bytes, source_state);
        trace_pixel_proof_computed(target->owner_surface_id, target->fd_dev, target->fd_ino, target->exported_fd.fd_content_generation,
                                   VkvvExportPixelSource::StreamLocalSeed, target_crc, black_crc, zero_crc, target_black, target_zero, target_bytes, target_state);
        VKVV_TRACE("seed-source-pixel-proof",
                   "source_surface=%u source_stream=%llu source_codec=0x%x source_present_gen=%llu source_fd_content_gen=%llu source_crc=0x%llx black_crc=0x%llx "
                   "zero_crc=0x%llx is_black=%u is_zero=%u pixel_proof_valid=%u valid_for_seed=%u sample_bytes=%llu proof_enabled=%u",
                   source->surface_id, static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   static_cast<unsigned long long>(source->export_resource.present_generation),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(&source->export_resource)), static_cast<unsigned long long>(source_crc),
                   static_cast<unsigned long long>(black_crc), static_cast<unsigned long long>(zero_crc), source_black ? 1U : 0U, source_zero ? 1U : 0U,
                   source_proof.identity_valid ? 1U : 0U, source_valid ? 1U : 0U, static_cast<unsigned long long>(source_bytes), export_pixel_proof_enabled() ? 1U : 0U);
        VKVV_TRACE("seed-target-pixel-proof",
                   "target_surface=%u source_surface=%u target_fd_dev=%llu target_fd_ino=%llu target_crc_after_copy=0x%llx source_crc=0x%llx matches_source=%u "
                   "source_color_state=%s target_color_state=%s target_is_black=%u target_is_zero=%u pixel_proof_valid=%u pixel_proof_state=%s target_valid=%u "
                   "reject_reason=%s copy_status=%s source_sample_bytes=%llu target_sample_bytes=%llu sample_bytes=%llu",
                   target->owner_surface_id, source->surface_id, static_cast<unsigned long long>(target->fd_dev), static_cast<unsigned long long>(target->fd_ino),
                   static_cast<unsigned long long>(target_crc), static_cast<unsigned long long>(source_crc), target_matches ? 1U : 0U,
                   vkvv_pixel_color_state_name(source_color_state), vkvv_pixel_color_state_name(target_color_state), target_black ? 1U : 0U, target_zero ? 1U : 0U,
                   target_valid ? 1U : 0U, vkvv_pixel_proof_state_name(target_state), target_valid ? 1U : 0U,
                   target_valid ? "none" :
                                  (!copy_success        ? "copy-failed" :
                                       !source_valid ? "source-proof-invalid" :
                                       !target_has_seed_gen ? "target-fd-content-generation-missing" :
                                                             "target-invalid"),
                   copy_status != nullptr ? copy_status : "unknown", static_cast<unsigned long long>(source_bytes), static_cast<unsigned long long>(target_bytes),
                   static_cast<unsigned long long>(target_bytes));
        if (!target_valid) {
            VKVV_TRACE("seed-target-proof-failed",
                       "surface=%u target_surface=%u source_surface=%u driver=%llu stream=%llu codec=0x%x reason=%s status=%d copy_status=%s source_valid=%u "
                       "target_fd_content_gen=%llu",
                       target->owner_surface_id, target->owner_surface_id, source->surface_id, static_cast<unsigned long long>(target->driver_instance_id),
                       static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                       !copy_success             ? "copy-failed" :
                           !source_valid         ? "source-proof-invalid" :
                           !target_has_seed_gen  ? "target-fd-content-generation-missing" :
                                                    "target-invalid",
                       VA_STATUS_ERROR_OPERATION_FAILED, copy_status != nullptr ? copy_status : "unknown", source_valid ? 1U : 0U,
                       static_cast<unsigned long long>(target->exported_fd.fd_content_generation));
        }
    }

    bool attach_imported_export_resource_by_fd(VulkanRuntime* runtime, SurfaceResource* source) {
        if (runtime == nullptr || source == nullptr || source->export_resource.image != VK_NULL_HANDLE || !source->import.external || !source->import.fd.valid) {
            return false;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        RetainedExportMatch         best_miss   = RetainedExportMatch::FdMismatch;
        bool                        saw_backing = false;
        for (auto it = runtime->retained_exports.begin(); it != runtime->retained_exports.end(); ++it) {
            saw_backing                     = true;
            const RetainedExportMatch match = retained_export_match_import(*it, source->import, source->driver_instance_id, source->stream_id, source->codec_operation,
                                                                           source->va_fourcc, source->format, source->coded_extent);
            if (match != RetainedExportMatch::Match) {
                if (match != RetainedExportMatch::FdMismatch) {
                    best_miss = match;
                }
                continue;
            }

            ExportResource& retained                       = it->resource;
            source->export_resource                        = retained;
            source->export_resource.driver_instance_id     = source->driver_instance_id;
            source->export_resource.stream_id              = source->stream_id;
            source->export_resource.codec_operation        = source->codec_operation;
            source->export_resource.owner_surface_id       = source->surface_id;
            source->export_resource.predecode_exported     = false;
            source->export_resource.predecode_seeded       = false;
            source->export_resource.black_placeholder      = false;
            source->export_resource.seed_source_surface_id = VA_INVALID_ID;
            source->export_resource.seed_source_generation = 0;
            source->export_resource.seed_pixel_proof_valid = false;
            source->export_retained_attached               = true;
            source->export_import_attached                 = true;
            it->state                                      = RetainedExportState::Attached;
            runtime->retained_export_memory_bytes          = runtime->retained_export_memory_bytes > source->export_resource.allocation_size ?
                runtime->retained_export_memory_bytes - source->export_resource.allocation_size :
                0;
            trace_export_fd_lifetime(source, &source->export_resource, "retained-attach", source->export_resource.content_generation,
                                     export_resource_fd_may_be_sampled_by_client(&source->export_resource));
            VKVV_TRACE("export-import-attach",
                       "surface=%u driver=%llu stream=%llu codec=0x%x import_fd_dev=%llu import_fd_ino=%llu old_owner=%u old_driver=%llu old_stream=%llu old_codec=0x%x "
                       "shadow_mem=0x%llx shadow_gen=%llu retained=%zu retained_mem=%llu seq=%llu",
                       source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                       static_cast<unsigned long long>(source->import.fd.dev), static_cast<unsigned long long>(source->import.fd.ino), retained.owner_surface_id,
                       static_cast<unsigned long long>(retained.driver_instance_id), static_cast<unsigned long long>(retained.stream_id), retained.codec_operation,
                       vkvv_trace_handle(source->export_resource.memory), static_cast<unsigned long long>(source->export_resource.content_generation),
                       runtime->retained_exports.size() - 1, static_cast<unsigned long long>(runtime->retained_export_memory_bytes),
                       static_cast<unsigned long long>(it->retained_sequence));
            runtime->retained_exports.erase(it);
            note_retained_export_attached_locked(runtime);
            return true;
        }

        VKVV_TRACE(
            "export-import-attach-miss",
            "surface=%u driver=%llu stream=%llu codec=0x%x import_fd_dev=%llu import_fd_ino=%llu format=%d fourcc=0x%x extent=%ux%u retained=%zu retained_mem=%llu reason=%s",
            source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
            static_cast<unsigned long long>(source->import.fd.dev), static_cast<unsigned long long>(source->import.fd.ino), source->format, source->va_fourcc,
            source->coded_extent.width, source->coded_extent.height, runtime->retained_exports.size(), static_cast<unsigned long long>(runtime->retained_export_memory_bytes),
            saw_backing ? retained_export_match_reason(best_miss) : "no-retained-backing");
        return false;
    }

    bool ensure_export_only_surface_resource(VkvvSurface* surface, const ExportFormatInfo* format, VkExtent2D extent, char* reason, size_t reason_size) {
        if (surface == nullptr || format == nullptr) {
            std::snprintf(reason, reason_size, "missing surface export resource state");
            return false;
        }

        auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
        if (resource == nullptr) {
            resource = new (std::nothrow) SurfaceResource();
            if (resource == nullptr) {
                std::snprintf(reason, reason_size, "out of memory creating export-only surface resource");
                return false;
            }
            surface->vulkan = resource;
        }

        if (resource->image != VK_NULL_HANDLE) {
            resource->driver_instance_id = surface->driver_instance_id;
            resource->stream_id          = surface->stream_id;
            resource->codec_operation    = static_cast<VkVideoCodecOperationFlagsKHR>(surface->codec_operation);
            resource->visible_extent     = {surface->width, surface->height};
            resource->import             = surface->import;
            clear_surface_direct_import_present_state(resource);
            clear_surface_av1_visible_output_trace(resource);
            return true;
        }

        resource->extent                                 = extent;
        resource->coded_extent                           = extent;
        resource->visible_extent                         = {surface->width, surface->height};
        resource->driver_instance_id                     = surface->driver_instance_id;
        resource->stream_id                              = surface->stream_id;
        resource->codec_operation                        = static_cast<VkVideoCodecOperationFlagsKHR>(surface->codec_operation);
        resource->surface_id                             = surface->id;
        resource->format                                 = format->vk_format;
        resource->va_rt_format                           = surface->rt_format;
        resource->va_fourcc                              = surface->fourcc;
        resource->decode_key                             = {};
        resource->allocation_size                        = 0;
        resource->plane_layouts[0]                       = {};
        resource->plane_layouts[1]                       = {};
        resource->plane_count                            = 0;
        resource->drm_format_modifier                    = 0;
        resource->exportable                             = false;
        resource->has_drm_format_modifier                = false;
        resource->import                                 = surface->import;
        resource->layout                                 = VK_IMAGE_LAYOUT_UNDEFINED;
        resource->last_nondisplay_skip_generation        = 0;
        resource->last_nondisplay_skip_shadow_generation = 0;
        resource->last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
        resource->last_display_refresh_generation        = 0;
        clear_surface_direct_import_present_state(resource);
        clear_surface_av1_visible_output_trace(resource);
        return true;
    }

    bool export_resource_matches_surface(const SurfaceResource* source) {
        if (source == nullptr) {
            return false;
        }

        const ExportResource& resource = source->export_resource;
        return resource.image != VK_NULL_HANDLE && resource.driver_instance_id == source->driver_instance_id && resource.stream_id == source->stream_id &&
            resource.codec_operation == source->codec_operation && resource.format == source->format && resource.va_fourcc == source->va_fourcc &&
            resource.extent.width >= source->coded_extent.width && resource.extent.height >= source->coded_extent.height;
    }

    void retag_predecode_export_to_source(SurfaceResource* source) {
        if (source == nullptr || source->content_generation == 0 || source->stream_id == 0 || source->codec_operation == 0) {
            return;
        }

        ExportResource& resource = source->export_resource;
        if (!resource.predecode_exported || resource.content_generation != 0 || resource.image == VK_NULL_HANDLE || resource.memory == VK_NULL_HANDLE ||
            resource.format != source->format || resource.va_fourcc != source->va_fourcc || resource.extent.width < source->coded_extent.width ||
            resource.extent.height < source->coded_extent.height) {
            return;
        }

        resource.driver_instance_id = source->driver_instance_id;
        resource.stream_id          = source->stream_id;
        resource.codec_operation    = source->codec_operation;
        resource.owner_surface_id   = source->surface_id;
        VKVV_TRACE("export-retag-predecode", "surface=%u driver=%llu stream=%llu codec=0x%x shadow_mem=0x%llx content_gen=%llu seed_surface=%u seed_gen=%llu", source->surface_id,
                   static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   vkvv_trace_handle(resource.memory), static_cast<unsigned long long>(source->content_generation), resource.seed_source_surface_id,
                   static_cast<unsigned long long>(resource.seed_source_generation));
    }

    bool predecode_seed_target_matches(const ExportResource* target, const SurfaceResource* source) {
        return target != nullptr && source != nullptr && target != &source->export_resource && target->image != VK_NULL_HANDLE && target->memory != VK_NULL_HANDLE &&
            target->predecode_exported && !target->predecode_seeded && target->content_generation == 0 && target->seed_source_surface_id == VA_INVALID_ID &&
            target->seed_source_generation == 0 && target->driver_instance_id == source->driver_instance_id && target->stream_id != 0 && target->stream_id == source->stream_id &&
            target->codec_operation != 0 && target->codec_operation == source->codec_operation && target->format == source->format && target->va_fourcc == source->va_fourcc &&
            target->extent.width == source->coded_extent.width && target->extent.height == source->coded_extent.height;
    }

    bool predecode_seed_policy_keeps_placeholder(const ExportResource* target, const SurfaceResource* source) {
        return predecode_seed_target_matches(target, source) && (!predecode_seed_source_safe_for_client(source) || !predecode_seed_source_pixel_proof_state(source).valid);
    }

    bool can_seed_predecode_target(const ExportResource* target, const SurfaceResource* source) {
        return predecode_seed_target_matches(target, source) && predecode_seed_source_safe_for_client(source) && predecode_seed_source_pixel_proof_state(source).valid;
    }

    bool pixel_proof_value_is_off(const char* value) {
        return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "off") == 0;
    }

    bool pixel_proof_value_is_seed(const char* value) {
        return value != nullptr && std::strcmp(value, "seed") == 0;
    }

    bool pixel_proof_value_is_all(const char* value) {
        return value != nullptr && (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0 || std::strcmp(value, "all") == 0);
    }

    VkvvPixelProofMode export_pixel_proof_mode() {
        const char* mode = std::getenv("VKVV_PIXEL_PROOF_MODE");
        if (mode != nullptr && mode[0] != '\0') {
            if (pixel_proof_value_is_off(mode)) {
                return VkvvPixelProofMode::Off;
            }
            if (pixel_proof_value_is_all(mode)) {
                return VkvvPixelProofMode::All;
            }
            return VkvvPixelProofMode::Seed;
        }

        const char* legacy = std::getenv("VKVV_PIXEL_PROOF");
        if (legacy == nullptr || legacy[0] == '\0') {
            legacy = std::getenv("VKVV_EXPORT_PIXEL_PROOF");
        }
        if (legacy == nullptr || legacy[0] == '\0') {
            legacy = std::getenv("VKVV_TRACE_PIXEL_PROOF");
        }
        if (legacy != nullptr && legacy[0] != '\0') {
            if (pixel_proof_value_is_off(legacy)) {
                return VkvvPixelProofMode::Off;
            }
            return pixel_proof_value_is_seed(legacy) ? VkvvPixelProofMode::Seed : VkvvPixelProofMode::All;
        }
        return VkvvPixelProofMode::Seed;
    }

    bool export_pixel_proof_enabled() {
        return export_pixel_proof_mode() != VkvvPixelProofMode::Off;
    }

    bool export_seed_pixel_proof_required() {
        return export_pixel_proof_mode() != VkvvPixelProofMode::Off;
    }

    bool export_visible_pixel_proof_required() {
        return export_pixel_proof_mode() == VkvvPixelProofMode::All;
    }

    uint64_t fnv1a64_u64_update(uint64_t hash, uint64_t value) {
        for (uint32_t i = 0; i < 8; i++) {
            hash ^= (value >> (i * 8)) & 0xffU;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t fnv1a64_bytes(const uint8_t* bytes, size_t size) {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    uint64_t pixel_proof_hash_seed(const ExportFormatInfo* format, VkExtent2D extent) {
        uint64_t hash = 1469598103934665603ull;
        hash          = fnv1a64_u64_update(hash, extent.width);
        hash          = fnv1a64_u64_update(hash, extent.height);
        hash          = fnv1a64_u64_update(hash, format != nullptr ? format->va_fourcc : 0);
        if (format != nullptr) {
            for (uint32_t i = 0; i < format->layer_count; i++) {
                const ExportLayerInfo& layer           = format->layers[i];
                const uint32_t         bytes_per_pixel = export_plane_bytes_per_pixel(layer.drm_format);
                const VkExtent3D       plane_extent    = export_layer_extent(extent, layer);
                hash                                   = fnv1a64_u64_update(hash, layer.drm_format);
                hash                                   = fnv1a64_u64_update(hash, plane_extent.width);
                hash                                   = fnv1a64_u64_update(hash, plane_extent.height);
                hash                                   = fnv1a64_u64_update(hash, static_cast<uint64_t>(plane_extent.width) * bytes_per_pixel);
            }
        }
        return hash;
    }

    uint32_t pixel_proof_sample_count(uint32_t layer_index) {
        return layer_index == 0 ? 32u : 16u;
    }

    uint32_t pixel_proof_grid_cols(uint32_t sample_count) {
        return sample_count <= 4 ? sample_count : 4u;
    }

    uint32_t pixel_proof_grid_rows(uint32_t sample_count) {
        const uint32_t cols = pixel_proof_grid_cols(sample_count);
        return cols == 0 ? 0 : static_cast<uint32_t>((sample_count + cols - 1) / cols);
    }

    VkOffset3D pixel_proof_sample_offset(VkExtent3D plane_extent, uint32_t sample, uint32_t sample_count) {
        const uint32_t cols = pixel_proof_grid_cols(sample_count);
        const uint32_t rows = pixel_proof_grid_rows(sample_count);
        const uint32_t col  = cols != 0 ? sample % cols : 0;
        const uint32_t row  = cols != 0 ? sample / cols : 0;
        const uint32_t x    = cols > 1 ? static_cast<uint32_t>((static_cast<uint64_t>(plane_extent.width - 1) * (2 * col + 1)) / (2 * cols)) : 0;
        const uint32_t y    = rows > 1 ? static_cast<uint32_t>((static_cast<uint64_t>(plane_extent.height - 1) * (2 * row + 1)) / (2 * rows)) : 0;
        return {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
    }

    void hash_reference_sample_byte(uint64_t* hash, uint8_t value) {
        *hash ^= value;
        *hash *= 1099511628211ull;
    }

    uint64_t pixel_reference_crc(const ExportFormatInfo* format, VkExtent2D extent, bool black) {
        if (format == nullptr) {
            return 0;
        }
        uint64_t hash = pixel_proof_hash_seed(format, extent);
        for (uint32_t i = 0; i < format->layer_count; i++) {
            const ExportLayerInfo& layer           = format->layers[i];
            const uint32_t         bytes_per_pixel = export_plane_bytes_per_pixel(layer.drm_format);
            if (bytes_per_pixel == 0) {
                return 0;
            }
            const uint32_t samples = pixel_proof_sample_count(i);
            for (uint32_t sample = 0; sample < samples; sample++) {
                switch (layer.drm_format) {
                    case DRM_FORMAT_R8: hash_reference_sample_byte(&hash, black ? 16 : 0); break;
                    case DRM_FORMAT_GR88:
                        hash_reference_sample_byte(&hash, black ? 128 : 0);
                        hash_reference_sample_byte(&hash, black ? 128 : 0);
                        break;
                    case DRM_FORMAT_R16: {
                        const uint16_t value = black ? static_cast<uint16_t>(64u << 6u) : 0;
                        hash_reference_sample_byte(&hash, static_cast<uint8_t>(value & 0xffu));
                        hash_reference_sample_byte(&hash, static_cast<uint8_t>((value >> 8u) & 0xffu));
                        break;
                    }
                    case DRM_FORMAT_GR1616: {
                        const uint16_t value = black ? static_cast<uint16_t>(512u << 6u) : 0;
                        hash_reference_sample_byte(&hash, static_cast<uint8_t>(value & 0xffu));
                        hash_reference_sample_byte(&hash, static_cast<uint8_t>((value >> 8u) & 0xffu));
                        hash_reference_sample_byte(&hash, static_cast<uint8_t>(value & 0xffu));
                        hash_reference_sample_byte(&hash, static_cast<uint8_t>((value >> 8u) & 0xffu));
                        break;
                    }
                    default: return 0;
                }
            }
        }
        return hash;
    }

    bool pixel_identity_valid_from_state(VkvvPixelProofState state) {
        return state == VkvvPixelProofState::ValidNonBlack || state == VkvvPixelProofState::Black || state == VkvvPixelProofState::Zero;
    }

    VkvvPixelColorState pixel_color_state_from_sample(bool sample_ok, bool is_black, bool is_zero) {
        if (!sample_ok) {
            return VkvvPixelColorState::Unknown;
        }
        if (is_zero) {
            return VkvvPixelColorState::Zero;
        }
        if (is_black) {
            return VkvvPixelColorState::Black;
        }
        return VkvvPixelColorState::Nonzero;
    }

    bool pixel_proof_valid_for_source(VkvvExportPixelSource source, bool crc_valid, bool is_black, bool is_zero) {
        (void)is_black;
        (void)is_zero;
        if (!crc_valid) {
            return false;
        }
        switch (source) {
            case VkvvExportPixelSource::DecodedContent:
            case VkvvExportPixelSource::StreamLocalSeed:
            case VkvvExportPixelSource::RetainedUnknown: return true;
            default: return false;
        }
    }

    VkvvPixelProofState pixel_proof_state_from_sample(bool sample_ok, uint64_t crc, uint64_t black_crc, uint64_t zero_crc, bool is_black, bool is_zero) {
        if (!sample_ok || crc == 0) {
            return VkvvPixelProofState::Unavailable;
        }
        if (is_zero || (zero_crc != 0 && crc == zero_crc)) {
            return VkvvPixelProofState::Zero;
        }
        if (is_black || (black_crc != 0 && crc == black_crc)) {
            return VkvvPixelProofState::Black;
        }
        return VkvvPixelProofState::ValidNonBlack;
    }

    void trace_pixel_proof_computed(VASurfaceID surface_id, uint64_t fd_dev, uint64_t fd_ino, uint64_t generation, VkvvExportPixelSource pixel_source, uint64_t crc,
                                    uint64_t black_crc, uint64_t zero_crc, bool is_black, bool is_zero, VkDeviceSize sample_bytes, VkvvPixelProofState state) {
        VKVV_TRACE("pixel-proof-computed",
                   "surface=%u fd_dev=%llu fd_ino=%llu generation=%llu pixel_source=%s crc=0x%llx black_crc=0x%llx zero_crc=0x%llx is_black=%u is_zero=%u "
                   "sample_bytes=%llu state=%s pixel_proof_mode=%s",
                   surface_id, static_cast<unsigned long long>(fd_dev), static_cast<unsigned long long>(fd_ino), static_cast<unsigned long long>(generation),
                   vkvv_export_pixel_source_name(pixel_source), static_cast<unsigned long long>(crc), static_cast<unsigned long long>(black_crc),
                   static_cast<unsigned long long>(zero_crc), is_black ? 1U : 0U, is_zero ? 1U : 0U, static_cast<unsigned long long>(sample_bytes),
                   vkvv_pixel_proof_state_name(state), vkvv_pixel_proof_mode_name(export_pixel_proof_mode()));
    }

    SeedPixelProofState predecode_seed_source_pixel_proof_state(const SurfaceResource* source) {
        SeedPixelProofState state{};
        if (!export_pixel_proof_enabled() || source == nullptr || !source->decode_pixel_proof_valid || !source->present_pixel_proof_valid ||
            !source->present_pixel_matches_decode) {
            state.state = VkvvPixelProofState::Unavailable;
            return state;
        }

        char                    proof_reason[128] = {};
        const ExportFormatInfo* format            = export_format_for_surface(nullptr, source, proof_reason, sizeof(proof_reason));
        const uint64_t          black_crc         = pixel_reference_crc(format, source->coded_extent, true);
        const uint64_t          zero_crc          = pixel_reference_crc(format, source->coded_extent, false);
        state.is_black                            = black_crc != 0 && source->present_pixel_crc == black_crc;
        state.is_zero                             = zero_crc != 0 && source->present_pixel_crc == zero_crc;
        state.crc                                 = source->present_pixel_crc;
        state.sample_bytes                        = source->present_pixel_proof_valid ? 1 : 0;
        state.state          = pixel_proof_state_from_sample(source->present_pixel_proof_valid, source->present_pixel_crc, black_crc, zero_crc, state.is_black, state.is_zero);
        state.color_state    = pixel_color_state_from_sample(source->present_pixel_proof_valid, state.is_black, state.is_zero);
        state.identity_valid = pixel_identity_valid_from_state(state.state);
        state.source_is_placeholder = source->export_resource.bootstrap_export || source->export_resource.black_placeholder ||
            (source->export_resource.predecode_quarantined && source->export_resource.content_generation == 0);
        state.content_valid = !state.source_is_placeholder && source->content_generation != 0 && export_resource_fd_content_generation(&source->export_resource) != 0;
        state.valid         = state.identity_valid && state.content_valid;
        return state;
    }

    uint64_t fnv1a64_bytes_update(uint64_t hash, const uint8_t* bytes, size_t size) {
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool readback_image_luma_sample(VulkanRuntime* runtime, VkImage image, VkImageLayout* layout, const ExportFormatInfo* format, VkExtent2D extent, const char* label,
                                    uint64_t* crc, VkDeviceSize* sample_bytes, char* reason, size_t reason_size) {
        if (runtime == nullptr || image == VK_NULL_HANDLE || layout == nullptr || format == nullptr || crc == nullptr || sample_bytes == nullptr || format->layer_count == 0) {
            std::snprintf(reason, reason_size, "missing pixel proof input");
            return false;
        }

        std::vector<VkBufferImageCopy> regions;
        std::vector<VkDeviceSize>      region_sizes;
        *sample_bytes = 0;
        for (uint32_t i = 0; i < format->layer_count; i++) {
            const ExportLayerInfo& layer           = format->layers[i];
            const uint32_t         bytes_per_pixel = export_plane_bytes_per_pixel(layer.drm_format);
            if (bytes_per_pixel == 0) {
                std::snprintf(reason, reason_size, "unsupported pixel proof plane format 0x%x", layer.drm_format);
                return false;
            }
            const VkExtent3D plane_extent = export_layer_extent(extent, layer);
            const uint32_t   samples      = pixel_proof_sample_count(i);
            const VkExtent3D sample_extent{1, 1, 1};
            for (uint32_t sample = 0; sample < samples; sample++) {
                *sample_bytes = align_up(*sample_bytes, bytes_per_pixel);
                VkBufferImageCopy region{};
                region.bufferOffset                    = *sample_bytes;
                region.bufferRowLength                 = 0;
                region.bufferImageHeight               = 0;
                region.imageSubresource.aspectMask     = layer.aspect;
                region.imageSubresource.mipLevel       = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount     = 1;
                region.imageOffset                     = pixel_proof_sample_offset(plane_extent, sample, samples);
                region.imageExtent                     = sample_extent;
                const VkDeviceSize region_size         = bytes_per_pixel;
                regions.push_back(region);
                region_sizes.push_back(region_size);
                *sample_bytes += region_size;
            }
        }
        ScopedUploadBuffer readback(runtime);
        if (!create_staging_transfer_buffer(runtime, *sample_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback.buffer, label, reason, reason_size)) {
            return false;
        }

        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", label, reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", label, reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", label, reason, reason_size)) {
            return false;
        }

        const VkImageLayout                old_layout = *layout;
        std::vector<VkImageMemoryBarrier2> barriers;
        add_raw_image_barrier(&barriers, image, old_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        vkCmdCopyImageToBuffer(runtime->command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer.buffer, static_cast<uint32_t>(regions.size()), regions.data());

        barriers.clear();
        add_raw_image_barrier(&barriers, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, old_layout, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        *layout = old_layout;

        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", label, reason, reason_size)) {
            return false;
        }
        if (!submit_command_buffer_and_wait(runtime, reason, reason_size, label, CommandUse::Export)) {
            return false;
        }

        result = vkMapMemory(runtime->device, readback.buffer.memory, 0, readback.buffer.size, 0, &readback.buffer.mapped);
        if (!record_vk_result(runtime, result, "vkMapMemory", label, reason, reason_size)) {
            return false;
        }
        if (!readback.buffer.coherent) {
            VkMappedMemoryRange range{};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readback.buffer.memory;
            range.offset = 0;
            range.size   = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(runtime->device, 1, &range);
        }
        const auto* mapped = static_cast<const uint8_t*>(readback.buffer.mapped);
        uint64_t    hash   = pixel_proof_hash_seed(format, extent);
        for (size_t i = 0; i < regions.size(); i++) {
            hash = fnv1a64_bytes_update(hash, mapped + regions[i].bufferOffset, static_cast<size_t>(region_sizes[i]));
        }
        *crc = hash;
        vkUnmapMemory(runtime->device, readback.buffer.memory);
        readback.buffer.mapped = nullptr;
        return true;
    }

    void trace_predecode_keep_placeholder(const ExportResource* target, const SurfaceResource* source) {
        if (target == nullptr || source == nullptr) {
            return;
        }
        const bool  thumbnail_like        = predecode_seed_target_thumbnail_like(target);
        const bool  bootstrap_placeholder = source->export_resource.bootstrap_export || source->export_resource.black_placeholder || source->export_resource.predecode_quarantined;
        const char* reject_reason = bootstrap_placeholder ? "bootstrap-placeholder" : (predecode_seed_source_safe_for_client(source) ? "no-pixel-proof" : "source-not-client-safe");
        VKVV_TRACE("predecode-seed-policy",
                   "surface=%u source_surface=%u action=neutral-placeholder reason=%s presentable=0 present_pinned=0 decoded=0 thumbnail_like=%u "
                   "content_gen=%llu target_mem=0x%llx source_external_release_ok=%u source_present_gen=%llu source_fd_content_gen=%llu",
                   target->owner_surface_id, source->surface_id, reject_reason, thumbnail_like ? 1U : 0U, static_cast<unsigned long long>(target->content_generation),
                   vkvv_trace_handle(target->memory), export_visible_release_satisfied(&source->export_resource) ? 1U : 0U,
                   static_cast<unsigned long long>(source->export_resource.present_generation),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(&source->export_resource)));
        VKVV_TRACE("predecode-export-policy",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu pending_decode=0 policy=stream-local-last-visible action=neutral-placeholder source_surface=%u "
                   "source_present_gen=%llu source_external_release_ok=%u",
                   target->owner_surface_id, target->codec_operation, static_cast<unsigned long long>(target->stream_id),
                   static_cast<unsigned long long>(target->content_generation), source->surface_id, static_cast<unsigned long long>(source->export_resource.present_generation),
                   export_visible_release_satisfied(&source->export_resource) ? 1U : 0U);
    }

    bool export_seed_source_valid(const SurfaceResource* source) {
        return source != nullptr && source->image != VK_NULL_HANDLE && source->content_generation != 0 && source->export_seed_generation == source->content_generation &&
            source->driver_instance_id != 0 && source->stream_id != 0 && source->codec_operation != 0 && source->format != VK_FORMAT_UNDEFINED && source->va_fourcc != 0 &&
            source->coded_extent.width != 0 && source->coded_extent.height != 0;
    }

    bool export_seed_target_valid(const SurfaceResource* target) {
        return target != nullptr && target->export_resource.image != VK_NULL_HANDLE && target->export_resource.memory != VK_NULL_HANDLE && target->driver_instance_id != 0 &&
            target->stream_id != 0 && target->codec_operation != 0 && target->format != VK_FORMAT_UNDEFINED && target->va_fourcc != 0 && target->coded_extent.width != 0 &&
            target->coded_extent.height != 0;
    }

    bool export_seed_record_source_still_valid(const ExportSeedRecord& record) {
        const SurfaceResource* source = record.resource;
        return source != nullptr && source->driver_instance_id == record.driver_instance_id && source->stream_id == record.stream_id &&
            source->codec_operation == record.codec_operation && source->surface_id == record.surface_id && source->format == record.format &&
            source->va_fourcc == record.va_fourcc && source->coded_extent.width == record.coded_extent.width && source->coded_extent.height == record.coded_extent.height &&
            source->export_seed_generation == record.content_generation && source->content_generation == record.content_generation && export_seed_source_valid(source);
    }

    bool export_seed_record_matches(const ExportSeedRecord& record, const SurfaceResource* target) {
        return target != nullptr && export_seed_record_source_still_valid(record) && record.driver_instance_id == target->driver_instance_id &&
            record.stream_id == target->stream_id && record.codec_operation == target->codec_operation && record.format == target->format &&
            record.va_fourcc == target->va_fourcc && record.coded_extent.width == target->coded_extent.width && record.coded_extent.height == target->coded_extent.height;
    }

    void remember_export_seed_resource_locked(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (runtime == nullptr || !export_seed_source_valid(resource)) {
            return;
        }

        for (size_t i = 0; i < runtime->export_seed_records.size();) {
            const ExportSeedRecord& record = runtime->export_seed_records[i];
            if (!export_seed_record_source_still_valid(record) || record.resource == resource ||
                (record.driver_instance_id == resource->driver_instance_id && record.stream_id == resource->stream_id && record.codec_operation == resource->codec_operation &&
                 record.format == resource->format && record.va_fourcc == resource->va_fourcc && record.coded_extent.width == resource->coded_extent.width &&
                 record.coded_extent.height == resource->coded_extent.height)) {
                runtime->export_seed_records.erase(runtime->export_seed_records.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            i++;
        }

        runtime->export_seed_records.push_back({
            .driver_instance_id = resource->driver_instance_id,
            .stream_id          = resource->stream_id,
            .codec_operation    = resource->codec_operation,
            .format             = resource->format,
            .va_fourcc          = resource->va_fourcc,
            .coded_extent       = resource->coded_extent,
            .resource           = resource,
            .surface_id         = resource->surface_id,
            .content_generation = resource->export_seed_generation,
        });
        VKVV_TRACE("export-seed-register", "codec=0x%x stream=%llu source_surface=%u source_content_gen=%llu source_shadow_gen=%llu visible=1 refresh_export=1 published=%u",
                   resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), resource->surface_id,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                   surface_resource_has_published_visible_output(resource) ? 1U : 0U);
    }

    void unregister_export_seed_resource_locked(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (runtime == nullptr || resource == nullptr) {
            return;
        }
        for (size_t i = 0; i < runtime->export_seed_records.size();) {
            const ExportSeedRecord& record = runtime->export_seed_records[i];
            if (record.resource == resource ||
                (record.driver_instance_id == resource->driver_instance_id && record.surface_id == resource->surface_id && record.stream_id == resource->stream_id &&
                 record.codec_operation == resource->codec_operation && record.format == resource->format && record.va_fourcc == resource->va_fourcc)) {
                runtime->export_seed_records.erase(runtime->export_seed_records.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            i++;
        }
    }

    SurfaceResource* find_export_seed_source_locked(VulkanRuntime* runtime, const SurfaceResource* target) {
        if (runtime == nullptr || !export_seed_target_valid(target)) {
            return nullptr;
        }

        size_t           candidate_count       = 0;
        size_t           valid_candidate_count = 0;
        SurfaceResource* selected              = nullptr;
        SurfaceResource* valid_source          = nullptr;
        const char*      selected_reason       = "none";
        const char*      reject_summary        = "none";
        for (size_t i = 0; i < runtime->export_seed_records.size();) {
            ExportSeedRecord& record = runtime->export_seed_records[i];
            if (!export_seed_record_source_still_valid(record)) {
                VKVV_TRACE("export-seed-stale-drop",
                           "record_surface=%u record_stream=%llu record_codec=0x%x record_gen=%llu source_surface=%u source_gen=%llu source_seed_gen=%llu source_shadow_gen=%llu",
                           record.surface_id, static_cast<unsigned long long>(record.stream_id), record.codec_operation, static_cast<unsigned long long>(record.content_generation),
                           record.resource != nullptr ? record.resource->surface_id : VA_INVALID_ID,
                           record.resource != nullptr ? static_cast<unsigned long long>(record.resource->content_generation) : 0ULL,
                           record.resource != nullptr ? static_cast<unsigned long long>(record.resource->export_seed_generation) : 0ULL,
                           record.resource != nullptr ? static_cast<unsigned long long>(record.resource->export_resource.content_generation) : 0ULL);
                runtime->export_seed_records.erase(runtime->export_seed_records.begin() + static_cast<std::ptrdiff_t>(i));
                reject_summary = "stale-record";
                continue;
            }
            candidate_count++;
            SurfaceResource* source      = record.resource;
            const bool       same_driver = source != nullptr && source->driver_instance_id == target->driver_instance_id;
            const bool       same_stream = source != nullptr && source->stream_id == target->stream_id;
            const bool       same_codec  = source != nullptr && source->codec_operation == target->codec_operation;
            const bool       same_fourcc = source != nullptr && source->va_fourcc == target->va_fourcc;
            const bool       same_visible =
                source != nullptr && source->visible_extent.width == target->visible_extent.width && source->visible_extent.height == target->visible_extent.height;
            const bool same_coded  = source != nullptr && source->coded_extent.width == target->coded_extent.width && source->coded_extent.height == target->coded_extent.height;
            const bool same_domain = export_seed_record_matches(record, target) && source != target;
            const VkvvExportPixelSource candidate_pixel_source =
                source != nullptr ? export_pixel_source_for_resource(source, &source->export_resource) : VkvvExportPixelSource::None;
            const bool bootstrap_placeholder =
                source != nullptr && (source->export_resource.bootstrap_export || source->export_resource.black_placeholder || source->export_resource.predecode_quarantined);
            const bool                source_safe        = source != nullptr && !bootstrap_placeholder && predecode_seed_source_safe_for_client(source);
            const SeedPixelProofState source_pixel_proof = predecode_seed_source_pixel_proof_state(source);
            const bool candidate_placeholder = candidate_pixel_source == VkvvExportPixelSource::Placeholder || source_pixel_proof.source_is_placeholder || bootstrap_placeholder;
            const bool valid = same_domain && same_visible && source_safe && !candidate_placeholder && source_pixel_proof.identity_valid && source_pixel_proof.content_valid;
            if (valid) {
                valid_candidate_count++;
            }
            const char* proof_reject_reason = source_pixel_proof.state == VkvvPixelProofState::Mismatch ? "pixel-proof-mismatch" :
                !source_pixel_proof.identity_valid                                                      ? "no-pixel-proof" :
                !source_pixel_proof.content_valid                                                       ? "content-invalid" :
                                                                                                          "none";
            const char* reject_reason       = valid                                       ? "none" :
                source == target                                                          ? "self" :
                !same_driver                                                              ? "driver-mismatch" :
                !same_stream                                                              ? "stream-mismatch" :
                !same_codec                                                               ? "codec-mismatch" :
                !same_fourcc                                                              ? "fourcc-mismatch" :
                !same_visible                                                             ? "visible-extent-mismatch" :
                !same_coded                                                               ? "coded-extent-mismatch" :
                candidate_placeholder                                                     ? "bootstrap-placeholder" :
                !source_safe                                                              ? "source-not-client-safe" :
                (!source_pixel_proof.identity_valid || !source_pixel_proof.content_valid) ? proof_reject_reason :
                                                                                            "domain-mismatch";
            if (!valid && reject_summary[0] == 'n' && std::strcmp(reject_summary, "none") == 0) {
                reject_summary = reject_reason;
            }
            VKVV_TRACE("export-seed-candidate",
                       "target_surface=%u candidate_surface=%u same_driver=%u same_stream=%u same_codec=%u same_fourcc=%u same_visible_extent=%u same_coded_extent=%u "
                       "same_sequence_generation=%u same_session_generation=%u candidate_present_gen=%llu candidate_fd_content_gen=%llu candidate_external_release_ok=%u "
                       "candidate_pixel_source=%s candidate_pixel_identity_valid=%u candidate_pixel_content_valid=%u candidate_pixel_color_state=%s "
                       "candidate_pixel_proof_state=%s candidate_pixel_proof_valid=%u candidate_is_black=%u candidate_is_zero=%u candidate_valid=%u reject_reason=%s",
                       target->surface_id, source != nullptr ? source->surface_id : VA_INVALID_ID, same_driver ? 1U : 0U, same_stream ? 1U : 0U, same_codec ? 1U : 0U,
                       same_fourcc ? 1U : 0U, same_visible ? 1U : 0U, same_coded ? 1U : 0U,
                       source != nullptr && source->export_seed_generation == target->export_seed_generation ? 1U : 0U, same_stream ? 1U : 0U,
                       source != nullptr ? static_cast<unsigned long long>(source->export_resource.present_generation) : 0ULL,
                       source != nullptr ? static_cast<unsigned long long>(export_resource_fd_content_generation(&source->export_resource)) : 0ULL,
                       source != nullptr && export_visible_release_satisfied(&source->export_resource) ? 1U : 0U, vkvv_export_pixel_source_name(candidate_pixel_source),
                       source_pixel_proof.identity_valid ? 1U : 0U, source_pixel_proof.content_valid ? 1U : 0U, vkvv_pixel_color_state_name(source_pixel_proof.color_state),
                       vkvv_pixel_proof_state_name(source_pixel_proof.state), source_pixel_proof.valid ? 1U : 0U, source_pixel_proof.is_black ? 1U : 0U,
                       source_pixel_proof.is_zero ? 1U : 0U, valid ? 1U : 0U, reject_reason);
            if (same_domain && selected == nullptr) {
                selected = source;
                if (valid) {
                    selected_reason = "stream-local-valid";
                } else {
                    selected_reason = reject_reason;
                }
            }
            if (valid && valid_source == nullptr) {
                valid_source = source;
            }
            i++;
        }
        VKVV_TRACE("export-seed-candidate-scan",
                   "target_surface=%u target_driver=%llu target_stream=%llu target_codec=0x%x target_fourcc=0x%x target_visible_width=%u target_visible_height=%u "
                   "target_coded_width=%u target_coded_height=%u target_sequence_generation=%llu target_session_generation=%llu candidate_count=%zu valid_candidate_count=%zu "
                   "selected_surface=%u selected_reason=%s reject_summary=%s",
                   target->surface_id, static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                   target->va_fourcc, target->visible_extent.width, target->visible_extent.height, target->coded_extent.width, target->coded_extent.height,
                   static_cast<unsigned long long>(target->export_seed_generation), static_cast<unsigned long long>(target->stream_id), candidate_count, valid_candidate_count,
                   selected != nullptr ? selected->surface_id : VA_INVALID_ID, selected_reason, reject_summary);
        return valid_source;
    }

    std::string export_seed_records_string(const VulkanRuntime* runtime) {
        if (runtime == nullptr || runtime->export_seed_records.empty()) {
            return "-";
        }
        std::string text;
        char        item[160] = {};
        for (const ExportSeedRecord& record : runtime->export_seed_records) {
            std::snprintf(item, sizeof(item), "%ssurf%u/stream%llu/codec0x%x/gen%llu/seed%llu/shadow%llu/%ux%u", text.empty() ? "" : ",", record.surface_id,
                          static_cast<unsigned long long>(record.stream_id), record.codec_operation, static_cast<unsigned long long>(record.content_generation),
                          record.resource != nullptr ? static_cast<unsigned long long>(record.resource->export_seed_generation) : 0ULL,
                          record.resource != nullptr ? static_cast<unsigned long long>(record.resource->export_resource.content_generation) : 0ULL, record.coded_extent.width,
                          record.coded_extent.height);
            text += item;
        }
        return text.empty() ? "-" : text;
    }

    std::vector<ExportResource*> collect_predecode_seed_targets_locked(VulkanRuntime* runtime, SurfaceResource* source) {
        std::vector<ExportResource*> targets;
        if (runtime == nullptr || source == nullptr || source->content_generation == 0) {
            return targets;
        }

        for (size_t i = 0; i < runtime->predecode_exports.size();) {
            PredecodeExportRecord& record = runtime->predecode_exports[i];
            if (!predecode_export_record_still_valid(record)) {
                VKVV_TRACE("predecode-export-stale-drop", "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx gen=%llu", record.owner_surface_id,
                           static_cast<unsigned long long>(record.driver_instance_id), static_cast<unsigned long long>(record.stream_id), record.codec_operation,
                           vkvv_trace_handle(record.memory), static_cast<unsigned long long>(record.content_generation));
                runtime->predecode_exports.erase(runtime->predecode_exports.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (predecode_seed_target_matches(record.resource, source) && predecode_seed_policy_keeps_placeholder(record.resource, source)) {
                mark_export_predecode_nonpresentable(record.resource);
                trace_predecode_keep_placeholder(record.resource, source);
            } else if (can_seed_predecode_target(record.resource, source)) {
                targets.push_back(record.resource);
            }
            i++;
        }
        for (RetainedExportBacking& backing : runtime->retained_exports) {
            if (predecode_seed_target_matches(&backing.resource, source) && predecode_seed_policy_keeps_placeholder(&backing.resource, source)) {
                mark_export_predecode_nonpresentable(&backing.resource);
                trace_predecode_keep_placeholder(&backing.resource, source);
            } else if (can_seed_predecode_target(&backing.resource, source)) {
                targets.push_back(&backing.resource);
            }
        }
        return targets;
    }

    struct ExportCopyTargetSnapshot {
        ExportResource* resource           = nullptr;
        VkImage         image              = VK_NULL_HANDLE;
        VkDeviceMemory  memory             = VK_NULL_HANDLE;
        uint64_t        content_generation = 0;
        bool            predecode_exported = false;
    };

    ExportCopyTargetSnapshot snapshot_export_copy_target(ExportResource* resource) {
        ExportCopyTargetSnapshot snapshot{};
        snapshot.resource = resource;
        if (resource != nullptr) {
            snapshot.image              = resource->image;
            snapshot.memory             = resource->memory;
            snapshot.content_generation = resource->content_generation;
            snapshot.predecode_exported = resource->predecode_exported;
        }
        return snapshot;
    }

    bool export_copy_target_still_matches(const ExportCopyTargetSnapshot& snapshot) {
        return snapshot.resource != nullptr && snapshot.resource->image == snapshot.image && snapshot.resource->memory == snapshot.memory &&
            snapshot.resource->content_generation == snapshot.content_generation && snapshot.resource->predecode_exported == snapshot.predecode_exported;
    }

    bool export_copy_source_still_matches(const SurfaceResource* source, VkImage image, uint64_t content_generation) {
        return source != nullptr && source->image == image && source->content_generation == content_generation;
    }

    bool copy_surface_to_export_targets_locked(VulkanRuntime* runtime, SurfaceResource* source, ExportResource* owner_export, bool copy_owner_export,
                                               const std::vector<ExportResource*>& predecode_seed_targets, std::unique_lock<std::mutex>& export_lock, char* reason,
                                               size_t reason_size, VkvvExportCopyReason owner_copy_reason, bool owner_refresh_export) {
        if (runtime == nullptr || source == nullptr || source->image == VK_NULL_HANDLE) {
            std::snprintf(reason, reason_size, "missing surface export copy source");
            return false;
        }
        if (owner_export == nullptr && predecode_seed_targets.empty()) {
            return true;
        }

        const ExportFormatInfo* format = export_format_for_surface(nullptr, source, reason, reason_size);
        if (format == nullptr) {
            return false;
        }

        const VkImage                         source_image              = source->image;
        const uint64_t                        source_content_generation = source->content_generation;
        const uint64_t                        source_shadow_generation  = source->export_resource.content_generation;
        const ExportCopyTargetSnapshot        owner_snapshot            = snapshot_export_copy_target(owner_export);
        std::vector<ExportCopyTargetSnapshot> predecode_snapshots;
        predecode_snapshots.reserve(predecode_seed_targets.size());
        for (ExportResource* target : predecode_seed_targets) {
            predecode_snapshots.push_back(snapshot_export_copy_target(target));
        }

        std::unique_lock<std::mutex> command_lock(runtime->command_mutex);
        if (!ensure_command_resources(runtime, reason, reason_size)) {
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!record_vk_result(runtime, result, "vkResetFences", "surface export copy", reason, reason_size)) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "surface export copy", reason, reason_size)) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "surface export copy", reason, reason_size)) {
            return false;
        }

        std::vector<VkImageMemoryBarrier2> barriers;
        add_raw_image_barrier(&barriers, source->image, source->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        if (owner_export != nullptr) {
            if (copy_owner_export && owner_export->exported) {
                mark_export_visible_acquire(source, owner_export);
            }
            add_raw_image_barrier(&barriers, owner_export->image, owner_export->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        for (ExportResource* target : predecode_seed_targets) {
            add_raw_image_barrier(&barriers, target->image, target->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (owner_export != nullptr) {
            owner_export->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        for (ExportResource* target : predecode_seed_targets) {
            target->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }

        VkImageCopy regions[2]{};
        for (uint32_t i = 0; i < format->layer_count; i++) {
            regions[i].srcSubresource.aspectMask = format->layers[i].aspect;
            regions[i].srcSubresource.layerCount = 1;
            regions[i].dstSubresource.aspectMask = format->layers[i].aspect;
            regions[i].dstSubresource.layerCount = 1;
            regions[i].extent                    = export_layer_extent(source->coded_extent, format->layers[i]);
        }

        if (owner_export != nullptr && copy_owner_export) {
            vkCmdCopyImage(runtime->command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, owner_export->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           format->layer_count, regions);
        }
        for (ExportResource* target : predecode_seed_targets) {
            VKVV_TRACE(
                "export-copy-predecode-target",
                "source_surface=%u source_stream=%llu source_codec=0x%x source_gen=%llu target_owner=%u target_stream=%llu target_codec=0x%x target_mem=0x%llx target_gen=%llu",
                source->surface_id, static_cast<unsigned long long>(source->stream_id), source->codec_operation, static_cast<unsigned long long>(source->content_generation),
                target->owner_surface_id, static_cast<unsigned long long>(target->stream_id), target->codec_operation, vkvv_trace_handle(target->memory),
                static_cast<unsigned long long>(target->content_generation));
            vkCmdCopyImage(runtime->command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, format->layer_count,
                           regions);
        }

        barriers.clear();
        add_raw_image_barrier(&barriers, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                              VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
        if (owner_export != nullptr) {
            add_raw_image_barrier(&barriers, owner_export->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        }
        for (ExportResource* target : predecode_seed_targets) {
            add_raw_image_barrier(&barriers, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        }
        if (!barriers.empty()) {
            VkDependencyInfo dependency{};
            dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dependency.pImageMemoryBarriers    = barriers.data();
            vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
        }
        source->layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
        if (owner_export != nullptr) {
            owner_export->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        for (ExportResource* target : predecode_seed_targets) {
            target->layout = VK_IMAGE_LAYOUT_GENERAL;
        }

        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "surface export copy", reason, reason_size)) {
            return false;
        }
        if (!submit_command_buffer(runtime, reason, reason_size, "surface export copy", CommandUse::Export)) {
            return false;
        }
        const bool trace_enabled = vkvv_trace_enabled();
        const auto wait_start    = trace_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        export_lock.unlock();
        const bool waited = wait_for_command_fence(runtime, std::numeric_limits<uint64_t>::max(), reason, reason_size, "surface export copy");
        export_lock.lock();
        if (!waited) {
            return false;
        }
        if (trace_enabled) {
            uint64_t copy_targets = 0;
            uint64_t copy_bytes   = 0;
            if (owner_export != nullptr && copy_owner_export) {
                copy_targets++;
                copy_bytes += static_cast<uint64_t>(owner_export->allocation_size);
            }
            for (const ExportResource* target : predecode_seed_targets) {
                if (target != nullptr) {
                    copy_targets++;
                    copy_bytes += static_cast<uint64_t>(target->allocation_size);
                }
            }
            const auto wait_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count());
            VKVV_TRACE("export-copy-metrics", "surface=%u driver=%llu stream=%llu codec=0x%x owner_copy=%u predecode_targets=%zu copy_targets=%llu copy_bytes=%llu wait_ns=%llu",
                       source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                       owner_export != nullptr && copy_owner_export ? 1U : 0U, predecode_seed_targets.size(), static_cast<unsigned long long>(copy_targets),
                       static_cast<unsigned long long>(copy_bytes), static_cast<unsigned long long>(wait_ns));
        }

        const bool source_matches = export_copy_source_still_matches(source, source_image, source_content_generation);
        if (owner_export != nullptr && copy_owner_export) {
            if (!source_matches || !export_copy_target_still_matches(owner_snapshot)) {
                std::snprintf(reason, reason_size, "surface export copy target changed before owner publish");
                VKVV_TRACE("export-copy-publish-skip",
                           "kind=owner source_surface=%u source_match=%u source_gen=%llu expected_gen=%llu target_owner=%u target_mem=0x%llx expected_mem=0x%llx",
                           source->surface_id, source_matches ? 1U : 0U, static_cast<unsigned long long>(source->content_generation),
                           static_cast<unsigned long long>(source_content_generation), owner_export->owner_surface_id, vkvv_trace_handle(owner_export->memory),
                           vkvv_trace_handle(owner_snapshot.memory));
                return false;
            }
            const bool                    was_bootstrap_export        = owner_export->bootstrap_export;
            const uint64_t                previous_present_generation = owner_export->present_generation;
            const VkvvExportPresentSource previous_present_source     = owner_export->present_source;
            owner_export->content_generation                          = source->content_generation;
            owner_export->predecode_exported                          = false;
            owner_export->predecode_seeded                            = false;
            owner_export->black_placeholder                           = false;
            owner_export->seed_source_surface_id                      = VA_INVALID_ID;
            owner_export->seed_source_generation                      = 0;
            owner_export->seed_pixel_proof_valid                      = false;
            owner_export->seed_pixel_crc                              = 0;
            owner_export->seed_black_crc                              = 0;
            owner_export->seed_zero_crc                               = 0;
            owner_export->seed_pixel_proof_state                      = VkvvPixelProofState::Unknown;
            owner_export->seed_pixel_color_state                      = VkvvPixelColorState::Unknown;
            mark_export_fd_written(owner_export, source->content_generation);
            if (owner_refresh_export) {
                clear_export_present_state(owner_export);
            } else {
                owner_export->present_pinned            = false;
                owner_export->presentable               = false;
                owner_export->published_visible         = false;
                owner_export->present_generation        = previous_present_generation;
                owner_export->present_source            = previous_present_source;
                owner_export->client_visible_shadow     = owner_export->exported || export_resource_fd_may_be_sampled_by_client(owner_export);
                owner_export->private_nondisplay_shadow = false;
            }
            if (owner_refresh_export || export_resource_fd_may_be_sampled_by_client(owner_export)) {
                mark_export_visible_release(source, owner_export, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            }
            if (owner_refresh_export && !export_visible_pixel_proof_required()) {
                pin_export_visible_present(source, owner_export, VkvvExportPresentSource::VisibleRefresh);
            }
            if (owner_export->predecode_quarantined && owner_export->content_generation != 0) {
                exit_predecode_quarantine(source, owner_export, export_visible_release_satisfied(owner_export));
            }
            if (was_bootstrap_export) {
                VKVV_TRACE("bootstrap-export-upgrade",
                           "surface=%u driver=%llu fd_dev=%llu fd_ino=%llu codec=0x%x stream=%llu content_gen=%llu fd_content_gen=%llu display_visible=%u presentable=%u "
                           "published_visible=%u",
                           source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(owner_export->fd_dev),
                           static_cast<unsigned long long>(owner_export->fd_ino), source->codec_operation, static_cast<unsigned long long>(source->stream_id),
                           static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(export_resource_fd_content_generation(owner_export)),
                           owner_refresh_export ? 1U : 0U, owner_export->presentable ? 1U : 0U, owner_export->published_visible ? 1U : 0U);
            }
            owner_export->bootstrap_export = false;
            owner_export->export_role      = VkvvExportRole::DecodedPresentation;
            trace_exported_fd_freshness_check(source, owner_export, owner_refresh_export, owner_refresh_export, "copied-to-export-fd");
            VKVV_TRACE("export-copy-proof",
                       "codec=0x%x surface=%u source_surface=%u target_surface=%u source_content_gen=%llu target_content_gen_before=%llu target_content_gen_after=%llu "
                       "source_shadow_gen=%llu target_shadow_gen_before=%llu target_shadow_gen_after=%llu fd_content_gen=%llu copy_reason=%s refresh_export=%u",
                       source->codec_operation, source->surface_id, source->surface_id, owner_export->owner_surface_id, static_cast<unsigned long long>(source->content_generation),
                       static_cast<unsigned long long>(owner_snapshot.content_generation), static_cast<unsigned long long>(owner_export->content_generation),
                       static_cast<unsigned long long>(source_shadow_generation), static_cast<unsigned long long>(owner_snapshot.content_generation),
                       static_cast<unsigned long long>(owner_export->content_generation), static_cast<unsigned long long>(export_resource_fd_content_generation(owner_export)),
                       vkvv_export_copy_reason_name(owner_copy_reason), owner_refresh_export ? 1U : 0U);
        }
        for (size_t i = 0; i < predecode_seed_targets.size(); i++) {
            ExportResource*                 target   = predecode_seed_targets[i];
            const ExportCopyTargetSnapshot& snapshot = predecode_snapshots[i];
            if (!source_matches || !export_copy_target_still_matches(snapshot) || !can_seed_predecode_target(target, source)) {
                VKVV_TRACE("export-copy-publish-skip",
                           "kind=predecode source_surface=%u source_match=%u source_gen=%llu expected_gen=%llu target_owner=%u target_mem=0x%llx expected_mem=0x%llx "
                           "target_gen=%llu expected_target_gen=%llu target_predecode=%u expected_predecode=%u",
                           source->surface_id, source_matches ? 1U : 0U, static_cast<unsigned long long>(source->content_generation),
                           static_cast<unsigned long long>(source_content_generation), target != nullptr ? target->owner_surface_id : VA_INVALID_ID,
                           target != nullptr ? vkvv_trace_handle(target->memory) : 0ULL, vkvv_trace_handle(snapshot.memory),
                           target != nullptr ? static_cast<unsigned long long>(target->content_generation) : 0ULL, static_cast<unsigned long long>(snapshot.content_generation),
                           target != nullptr && target->predecode_exported ? 1U : 0U, snapshot.predecode_exported ? 1U : 0U);
                continue;
            }
            target->black_placeholder = false;
            if (target->predecode_exported) {
                const uint64_t source_present_generation = source->export_resource.present_generation != 0 ? source->export_resource.present_generation : source->content_generation;
                uint64_t       seed_fd_content_generation = export_resource_fd_content_generation(&source->export_resource);
                if (seed_fd_content_generation == 0) {
                    seed_fd_content_generation = source_present_generation;
                }
                target->predecode_exported                                  = true;
                target->predecode_seeded                                    = true;
                target->predecode_quarantined                               = true;
                target->black_placeholder                                   = false;
                target->seed_source_surface_id                              = source->surface_id;
                target->seed_source_generation                              = seed_fd_content_generation;
                target->exported_fd.fd_content_generation                   = seed_fd_content_generation;
                target->exported_fd.last_written_content_generation         = seed_fd_content_generation;
                target->exported_fd.detached_from_surface                   = false;
                target->exported_fd.may_be_sampled_by_client                = false;
                target->presentable                                         = false;
                target->present_pinned                                      = false;
                target->published_visible                                   = false;
                target->present_generation                                  = 0;
                target->present_source                                      = VkvvExportPresentSource::PredecodePlaceholder;
                target->client_visible_shadow                               = false;
                target->private_nondisplay_shadow                           = false;
                mark_export_predecode_nonpresentable(target);
                VKVV_TRACE("predecode-target-seed-state",
                           "surface=%u target_surface=%u source_surface=%u driver=%llu stream=%llu codec=0x%x source_present_gen=%llu source_fd_content_gen=%llu "
                           "target_content_gen=%llu target_fd_content_gen_after=%llu target_last_written_content_gen_after=%llu target_pixel_source=seed "
                           "target_presentable=0 target_published_visible=0 target_predecode_quarantined=%u target_predecode_exported=%u",
                           target->owner_surface_id, target->owner_surface_id, source->surface_id, static_cast<unsigned long long>(target->driver_instance_id),
                           static_cast<unsigned long long>(target->stream_id), target->codec_operation, static_cast<unsigned long long>(source_present_generation),
                           static_cast<unsigned long long>(seed_fd_content_generation), static_cast<unsigned long long>(target->content_generation),
                           static_cast<unsigned long long>(target->exported_fd.fd_content_generation),
                           static_cast<unsigned long long>(target->exported_fd.last_written_content_generation), target->predecode_quarantined ? 1U : 0U,
                           target->predecode_exported ? 1U : 0U);
            } else {
                target->content_generation = source->content_generation;
                clear_export_present_state(target);
                mark_export_fd_written(target, source->content_generation);
            }
            VKVV_TRACE("export-predecode-seeded",
                       "source_surface=%u source_stream=%llu source_codec=0x%x source_gen=%llu target_owner=%u target_mem=0x%llx target_gen=%llu fd_content_gen=%llu "
                       "target_predecode=%u",
                       source->surface_id, static_cast<unsigned long long>(source->stream_id), source->codec_operation, static_cast<unsigned long long>(source->content_generation),
                       target->owner_surface_id, vkvv_trace_handle(target->memory), static_cast<unsigned long long>(target->content_generation),
                       static_cast<unsigned long long>(target->exported_fd.fd_content_generation), target->predecode_exported ? 1U : 0U);
            const bool thumbnail_like = predecode_seed_target_thumbnail_like(target);
            VKVV_TRACE("predecode-seed-policy",
                       "surface=%u source_surface=%u action=stream-local-seed presentable=0 present_pinned=0 thumbnail_like=%u content_gen=%llu fd_content_gen=%llu "
                       "target_mem=0x%llx",
                       target->owner_surface_id, source->surface_id, thumbnail_like ? 1U : 0U, static_cast<unsigned long long>(target->content_generation),
                       static_cast<unsigned long long>(export_resource_fd_content_generation(target)), vkvv_trace_handle(target->memory));
            VKVV_TRACE("predecode-export-policy",
                       "surface=%u codec=0x%x stream=%llu content_gen=%llu pending_decode=0 policy=stream-local-last-visible action=stream-local-seed source_surface=%u "
                       "source_present_gen=%llu source_external_release_ok=%u",
                       target->owner_surface_id, target->codec_operation, static_cast<unsigned long long>(target->stream_id),
                       static_cast<unsigned long long>(target->content_generation), source->surface_id, static_cast<unsigned long long>(source->export_resource.present_generation),
                       export_visible_release_satisfied(&source->export_resource) ? 1U : 0U);
            trace_exported_fd_freshness_check(source, target, true, false, "stream-local-seed");
            VKVV_TRACE("export-copy-proof",
                       "codec=0x%x surface=%u source_surface=%u target_surface=%u source_content_gen=%llu target_content_gen_before=%llu target_content_gen_after=%llu "
                       "source_shadow_gen=%llu target_shadow_gen_before=%llu target_shadow_gen_after=%llu fd_content_gen=%llu target_fd_content_gen_after=%llu "
                       "target_shadow_pixel_gen=%llu target_pixel_source=%s source_present_gen=%llu source_fd_content_gen=%llu copy_reason=%s refresh_export=1",
                       source->codec_operation, source->surface_id, source->surface_id, target->owner_surface_id, static_cast<unsigned long long>(source->content_generation),
                       static_cast<unsigned long long>(snapshot.content_generation), static_cast<unsigned long long>(target->content_generation),
                       static_cast<unsigned long long>(source_shadow_generation), static_cast<unsigned long long>(snapshot.content_generation),
                       static_cast<unsigned long long>(target->content_generation), static_cast<unsigned long long>(target->exported_fd.fd_content_generation),
                       static_cast<unsigned long long>(target->exported_fd.fd_content_generation),
                       static_cast<unsigned long long>(target->exported_fd.fd_content_generation), vkvv_export_pixel_source_name(export_pixel_source_for_resource(nullptr, target)),
                       static_cast<unsigned long long>(source->export_resource.present_generation),
                       static_cast<unsigned long long>(export_resource_fd_content_generation(&source->export_resource)),
                       vkvv_export_copy_reason_name(VkvvExportCopyReason::PredecodePlaceholderSeed));
            trace_predecode_target_present_state(target, "predecode-target-seeded");
            trace_seed_pixel_proof(runtime, source, target, "success");
            if (!target->predecode_exported) {
                VKVV_TRACE("export-transition-published",
                           "source_surface=%u source_driver=%llu source_stream=%llu source_codec=0x%x source_gen=%llu target_owner=%u target_driver=%llu target_stream=%llu "
                           "target_codec=0x%x target_mem=0x%llx target_gen=%llu",
                           source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id),
                           source->codec_operation, static_cast<unsigned long long>(source->content_generation), target->owner_surface_id,
                           static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                           vkvv_trace_handle(target->memory), static_cast<unsigned long long>(target->content_generation));
            }
        }
        return true;
    }

    void add_raw_image_barrier(std::vector<VkImageMemoryBarrier2>* barriers, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                               VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        if (old_layout == new_layout) {
            return;
        }
        VkImageMemoryBarrier2 barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask                    = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : src_stage;
        barrier.srcAccessMask                   = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : src_access;
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

    bool copy_surface_to_export_resource(VulkanRuntime* runtime, SurfaceResource* source, uint32_t* seeded_predecode_exports, char* reason, size_t reason_size) {
        if (seeded_predecode_exports != nullptr) {
            *seeded_predecode_exports = 0;
        }
        const ExportFormatInfo* format = export_format_for_surface(nullptr, source, reason, reason_size);
        if (format == nullptr) {
            return false;
        }
        VKVV_TRACE("export-copy-enter", "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u",
                   source != nullptr ? source->surface_id : VA_INVALID_ID, source != nullptr ? static_cast<unsigned long long>(source->driver_instance_id) : 0ULL,
                   source != nullptr ? static_cast<unsigned long long>(source->stream_id) : 0ULL, source != nullptr ? source->codec_operation : 0U,
                   source != nullptr ? static_cast<unsigned long long>(source->content_generation) : 0ULL,
                   source != nullptr ? vkvv_trace_handle(source->export_resource.memory) : 0ULL,
                   source != nullptr ? static_cast<unsigned long long>(source->export_resource.content_generation) : 0ULL,
                   source != nullptr && source->export_resource.predecode_exported ? 1U : 0U, source != nullptr && source->export_resource.predecode_seeded ? 1U : 0U);
        retag_predecode_export_to_source(source);
        if (source->export_resource.exported && !export_resource_matches_surface(source)) {
            VKVV_TRACE("export-copy-detach-mismatch", "surface=%u driver=%llu stream=%llu codec=0x%x shadow_stream=%llu shadow_codec=0x%x shadow_mem=0x%llx", source->surface_id,
                       static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                       static_cast<unsigned long long>(source->export_resource.stream_id), source->export_resource.codec_operation,
                       vkvv_trace_handle(source->export_resource.memory));
            detach_export_resource(runtime, source);
        }
        if (!ensure_export_resource(runtime, source, reason, reason_size)) {
            return false;
        }

        std::unique_lock<std::mutex> export_lock(runtime->export_mutex);
        std::vector<ExportResource*> predecode_seed_targets = collect_predecode_seed_targets_locked(runtime, source);
        const bool                   owner_export_current   = source->content_generation != 0 && source->export_resource.content_generation == source->content_generation;
        const bool                   force_owner_copy       = av1_visible_export_requires_copy(source);
        if (owner_export_current && predecode_seed_targets.empty() && !force_owner_copy) {
            source->export_seed_generation = source->content_generation;
            remember_export_seed_resource_locked(runtime, source);
            trace_exported_fd_freshness_check(source, &source->export_resource, true, true, "already-current");
            VKVV_TRACE("export-copy-skip-current", "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu seed_gen=%llu shadow_mem=0x%llx shadow_gen=%llu",
                       source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                       static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(source->export_seed_generation),
                       vkvv_trace_handle(source->export_resource.memory), static_cast<unsigned long long>(source->export_resource.content_generation));
            return true;
        }

        ExportResource* export_resource = &source->export_resource;
        const bool      copy_owner      = !owner_export_current || force_owner_copy;
        VKVV_TRACE("export-copy-targets",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu owner_copy=%u forced_owner_copy=%u retained_attached=%u import_attached=%u owner_mem=0x%llx "
                   "owner_gen=%llu predecode_targets=%zu",
                   source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   static_cast<unsigned long long>(source->content_generation), copy_owner ? 1U : 0U, force_owner_copy ? 1U : 0U, source->export_retained_attached ? 1U : 0U,
                   source->export_import_attached ? 1U : 0U, vkvv_trace_handle(source->export_resource.memory),
                   static_cast<unsigned long long>(source->export_resource.content_generation), predecode_seed_targets.size());
        if (!copy_surface_to_export_targets_locked(runtime, source, export_resource, copy_owner, predecode_seed_targets, export_lock, reason, reason_size,
                                                   VkvvExportCopyReason::VisibleRefresh, true)) {
            return false;
        }
        unregister_predecode_export_resource_locked(runtime, export_resource);
        if (copy_owner) {
            clear_surface_export_attach_state(source);
        }
        source->export_seed_generation = source->content_generation;
        remember_export_seed_resource_locked(runtime, source);
        if (seeded_predecode_exports != nullptr) {
            *seeded_predecode_exports = static_cast<uint32_t>(predecode_seed_targets.size());
        }
        VKVV_TRACE("export-copy-done", "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu seeded_targets=%zu predecode=%u seeded=%u",
                   source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   static_cast<unsigned long long>(source->content_generation), vkvv_trace_handle(source->export_resource.memory),
                   static_cast<unsigned long long>(source->export_resource.content_generation), predecode_seed_targets.size(), source->export_resource.predecode_exported ? 1U : 0U,
                   source->export_resource.predecode_seeded ? 1U : 0U);
        return true;
    }

    bool refresh_nondisplay_export_resource(VulkanRuntime* runtime, SurfaceResource* source, char* reason, size_t reason_size) {
        if (runtime == nullptr || source == nullptr || source->image == VK_NULL_HANDLE || source->export_resource.image == VK_NULL_HANDLE ||
            source->export_resource.memory == VK_NULL_HANDLE || !source->export_resource.exported) {
            std::snprintf(reason, reason_size, "missing non-display export refresh backing");
            return false;
        }
        std::unique_lock<std::mutex> export_lock(runtime->export_mutex);
        std::vector<ExportResource*> no_predecode_targets;
        if (!copy_surface_to_export_targets_locked(runtime, source, &source->export_resource, true, no_predecode_targets, export_lock, reason, reason_size,
                                                   VkvvExportCopyReason::NondisplayCurrentRefresh, false)) {
            return false;
        }
        unregister_predecode_export_resource_locked(runtime, &source->export_resource);
        clear_surface_export_attach_state(source);
        source->export_seed_generation                 = 0;
        source->last_nondisplay_skip_generation        = source->content_generation;
        source->last_nondisplay_skip_shadow_generation = source->export_resource.content_generation;
        source->last_nondisplay_skip_shadow_memory     = source->export_resource.memory;
        return true;
    }

    bool seed_predecode_export_from_last_good(VulkanRuntime* runtime, SurfaceResource* target, char* reason, size_t reason_size) {
        if (runtime == nullptr || target == nullptr || target->export_resource.image == VK_NULL_HANDLE || target->export_resource.memory == VK_NULL_HANDLE ||
            target->export_resource.predecode_seeded) {
            return true;
        }

        std::unique_lock<std::mutex> export_lock(runtime->export_mutex);
        SurfaceResource*             source = find_export_seed_source_locked(runtime, target);
        if (source == nullptr || source == target) {
            VKVV_TRACE("export-seed-miss", "surface=%u driver=%llu stream=%llu codec=0x%x format=%d fourcc=0x%x extent=%ux%u target_gen=%llu shadow_gen=%llu", target->surface_id,
                       static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation, target->format,
                       target->va_fourcc, target->coded_extent.width, target->coded_extent.height, static_cast<unsigned long long>(target->content_generation),
                       static_cast<unsigned long long>(target->export_resource.content_generation));
            if (vkvv_trace_deep_enabled()) {
                const std::string records = export_seed_records_string(runtime);
                vkvv_trace_emit(
                    "export-seed-miss", "surface=%u driver=%llu stream=%llu codec=0x%x format=%d fourcc=0x%x extent=%ux%u target_gen=%llu shadow_gen=%llu records=\"%s\"",
                    target->surface_id, static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                    target->format, target->va_fourcc, target->coded_extent.width, target->coded_extent.height, static_cast<unsigned long long>(target->content_generation),
                    static_cast<unsigned long long>(target->export_resource.content_generation), records.c_str());
            }
            VKVV_TRACE("predecode-export-policy",
                       "surface=%u codec=0x%x stream=%llu content_gen=%llu pending_decode=0 policy=neutral-placeholder action=neutral-placeholder source_surface=%u "
                       "source_present_gen=0 source_external_release_ok=0",
                       target->surface_id, target->codec_operation, static_cast<unsigned long long>(target->stream_id), static_cast<unsigned long long>(target->content_generation),
                       VA_INVALID_ID);
            return true;
        }

        std::vector<ExportResource*> targets{&target->export_resource};
        target->export_resource.predecode_exported = true;
        if (predecode_seed_target_matches(&target->export_resource, source) && predecode_seed_policy_keeps_placeholder(&target->export_resource, source)) {
            mark_export_predecode_nonpresentable(&target->export_resource);
            trace_predecode_keep_placeholder(&target->export_resource, source);
            return true;
        }
        if (!can_seed_predecode_target(&target->export_resource, source)) {
            VKVV_TRACE("export-seed-reject",
                       "surface=%u driver=%llu stream=%llu codec=0x%x source_surface=%u target_gen=%llu shadow_gen=%llu target_decoded=%u predecode=%u seeded=%u "
                       "seed_surface=%u seed_gen=%llu source_driver=%llu source_stream=%llu source_codec=0x%x source_format=%d source_fourcc=0x%x source_extent=%ux%u "
                       "source_present_gen=%llu source_external_release_ok=%u source_fd_content_gen=%llu",
                       target->surface_id, static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                       source->surface_id, static_cast<unsigned long long>(target->content_generation), static_cast<unsigned long long>(target->export_resource.content_generation),
                       target->content_generation != 0 ? 1U : 0U, target->export_resource.predecode_exported ? 1U : 0U, target->export_resource.predecode_seeded ? 1U : 0U,
                       target->export_resource.seed_source_surface_id, static_cast<unsigned long long>(target->export_resource.seed_source_generation),
                       static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation, source->format,
                       source->va_fourcc, source->coded_extent.width, source->coded_extent.height, static_cast<unsigned long long>(source->export_resource.present_generation),
                       export_visible_release_satisfied(&source->export_resource) ? 1U : 0U,
                       static_cast<unsigned long long>(export_resource_fd_content_generation(&source->export_resource)));
            VKVV_TRACE("predecode-export-policy",
                       "surface=%u codec=0x%x stream=%llu content_gen=%llu pending_decode=0 policy=stream-local-last-visible action=failed-no-valid-source source_surface=%u "
                       "source_present_gen=%llu source_external_release_ok=%u",
                       target->surface_id, target->codec_operation, static_cast<unsigned long long>(target->stream_id), static_cast<unsigned long long>(target->content_generation),
                       source->surface_id, static_cast<unsigned long long>(source->export_resource.present_generation),
                       export_visible_release_satisfied(&source->export_resource) ? 1U : 0U);
            return true;
        }
        const bool thumbnail_like = predecode_seed_target_thumbnail_like(&target->export_resource);
        VKVV_TRACE("predecode-seed-policy",
                   "surface=%u source_surface=%u action=stream-local-seed presentable=0 present_pinned=0 thumbnail_like=%u content_gen=%llu target_mem=0x%llx", target->surface_id,
                   source->surface_id, thumbnail_like ? 1U : 0U, static_cast<unsigned long long>(target->export_resource.content_generation),
                   vkvv_trace_handle(target->export_resource.memory));
        VKVV_TRACE("export-seed-hit",
                   "surface=%u driver=%llu stream=%llu codec=0x%x source_surface=%u source_gen=%llu source_seed_gen=%llu source_shadow_gen=%llu target_mem=0x%llx target_gen=%llu",
                   target->surface_id, static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                   source->surface_id, static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(source->export_seed_generation),
                   static_cast<unsigned long long>(source->export_resource.content_generation), vkvv_trace_handle(target->export_resource.memory),
                   static_cast<unsigned long long>(target->export_resource.content_generation));
        return copy_surface_to_export_targets_locked(runtime, source, nullptr, false, targets, export_lock, reason, reason_size, VkvvExportCopyReason::VisibleRefresh, true);
    }

    void remember_export_seed_resource(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (runtime == nullptr || resource == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        remember_export_seed_resource_locked(runtime, resource);
    }

    void unregister_export_seed_resource(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (runtime == nullptr || resource == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        unregister_export_seed_resource_locked(runtime, resource);
    }

} // namespace vkvv
