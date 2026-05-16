#include "vulkan/export/internal.h"
#include "telemetry.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

        bool create_staging_transfer_buffer(VulkanRuntime* runtime, VkDeviceSize size, UploadBuffer* staging, char* reason, size_t reason_size) {
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size        = size;
            buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &staging->buffer);
            if (!record_vk_result(runtime, result, "vkCreateBuffer", "export shadow init", reason, reason_size)) {
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
                    std::snprintf(reason, reason_size, "no host-visible memory type for export shadow init");
                    return false;
                }
            }

            VkMemoryAllocateInfo allocate_info{};
            allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate_info.allocationSize  = requirements.size;
            allocate_info.memoryTypeIndex = memory_type_index;
            result                        = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &staging->memory);
            if (!record_vk_result(runtime, result, "vkAllocateMemory", "export shadow init", reason, reason_size)) {
                destroy_upload_buffer(runtime, staging);
                return false;
            }

            result = vkBindBufferMemory(runtime->device, staging->buffer, staging->memory, 0);
            if (!record_vk_result(runtime, result, "vkBindBufferMemory", "export shadow init", reason, reason_size)) {
                destroy_upload_buffer(runtime, staging);
                return false;
            }
            return true;
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
            if (!create_staging_transfer_buffer(runtime, staging_size, &staging.buffer, reason, reason_size)) {
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
            if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "export shadow init")) {
                return false;
            }

            resource->content_generation     = 0;
            resource->predecode_exported     = false;
            resource->predecode_seeded       = false;
            resource->black_placeholder      = true;
            resource->seed_source_surface_id = VA_INVALID_ID;
            resource->seed_source_generation = 0;
            return true;
        }

    } // namespace

    bool create_export_resource_with_tiling(VulkanRuntime* runtime, ExportResource* resource, const ExportFormatInfo* format, VkExtent2D extent, VkImageTiling tiling, char* reason,
                                            size_t reason_size) {
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
        image_info.pNext         = &external_image;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.format        = format->vk_format;
        image_info.extent        = {extent.width, extent.height, 1};
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling        = tiling;
        image_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
        if (!record_vk_result(runtime, result, "vkCreateImage", "export shadow image", reason, reason_size)) {
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
        allocate_info.pNext           = &export_allocate;
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;

        result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &resource->memory);
        if (!record_vk_result(runtime, result, "vkAllocateMemory", "export shadow image", reason, reason_size)) {
            destroy_export_resource(runtime, resource);
            return false;
        }

        result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
        if (!record_vk_result(runtime, result, "vkBindImageMemory", "export shadow image", reason, reason_size)) {
            destroy_export_resource(runtime, resource);
            return false;
        }

        for (uint32_t i = 0; i < format->layer_count; i++) {
            VkImageSubresource plane{};
            plane.aspectMask = format->layers[i].aspect;
            vkGetImageSubresourceLayout(runtime->device, resource->image, &plane, &resource->plane_layouts[i]);
        }

        resource->extent             = extent;
        resource->format             = format->vk_format;
        resource->va_fourcc          = format->va_fourcc;
        resource->allocation_size    = requirements.size;
        resource->plane_count        = format->layer_count;
        resource->content_generation = 0;
        if (vkvv_perf_enabled()) {
            perf_update_high_water(runtime->perf.export_image_high_water, static_cast<uint64_t>(resource->allocation_size));
        }
        if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
            modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
            result                    = runtime->get_image_drm_format_modifier_properties(runtime->device, resource->image, &modifier_properties);
            if (!record_vk_result(runtime, result, "vkGetImageDrmFormatModifierPropertiesEXT", "export shadow image", reason, reason_size)) {
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
                for (auto it = runtime->retained_exports.begin(); it != runtime->retained_exports.end(); ++it) {
                    ExportResource& retained = it->resource;
                    if (retained.driver_instance_id == source->driver_instance_id && retained.owner_surface_id == source->surface_id && retained.stream_id == source->stream_id &&
                        retained.codec_operation == source->codec_operation && retained.format == source->format && retained.va_fourcc == source->va_fourcc &&
                        retained.extent.width == source->coded_extent.width && retained.extent.height == source->coded_extent.height) {
                        *resource                    = retained;
                        resource->driver_instance_id = source->driver_instance_id;
                        resource->stream_id          = source->stream_id;
                        resource->codec_operation    = source->codec_operation;
                        resource->owner_surface_id   = source->surface_id;
                        resource->content_generation = 0;
                        it->state                    = RetainedExportState::Attached;
                        runtime->retained_export_memory_bytes =
                            runtime->retained_export_memory_bytes > resource->allocation_size ? runtime->retained_export_memory_bytes - resource->allocation_size : 0;
                        runtime->retained_exports.erase(it);
                        note_retained_export_attached_locked(runtime);
                        reattached = true;
                        break;
                    }
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

    bool attach_imported_export_resource_by_fd(VulkanRuntime* runtime, SurfaceResource* source) {
        if (runtime == nullptr || source == nullptr || source->export_resource.image != VK_NULL_HANDLE || !source->import.external || !source->import.fd.valid) {
            return false;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        RetainedExportMatch         best_miss   = RetainedExportMatch::FdMismatch;
        bool                        saw_backing = false;
        for (auto it = runtime->retained_exports.begin(); it != runtime->retained_exports.end(); ++it) {
            saw_backing                     = true;
            const RetainedExportMatch match = retained_export_match_import(*it, source->import, source->va_fourcc, source->format, source->coded_extent);
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
            it->state                                      = RetainedExportState::Attached;
            runtime->retained_export_memory_bytes          = runtime->retained_export_memory_bytes > source->export_resource.allocation_size ?
                runtime->retained_export_memory_bytes - source->export_resource.allocation_size :
                0;
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
            target->predecode_exported && target->content_generation == 0 && target->driver_instance_id == source->driver_instance_id && target->stream_id != 0 &&
            target->stream_id == source->stream_id && target->codec_operation != 0 && target->codec_operation == source->codec_operation && target->format == source->format &&
            target->va_fourcc == source->va_fourcc && target->extent.width == source->coded_extent.width && target->extent.height == source->coded_extent.height;
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
                continue;
            }
            if (export_seed_record_matches(record, target) && record.resource != target) {
                return record.resource;
            }
            i++;
        }
        return nullptr;
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
            if (predecode_seed_target_matches(record.resource, source)) {
                targets.push_back(record.resource);
            }
            i++;
        }
        for (RetainedExportBacking& backing : runtime->retained_exports) {
            if (predecode_seed_target_matches(&backing.resource, source)) {
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
                                               size_t reason_size) {
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
        if (!submit_command_buffer(runtime, reason, reason_size, "surface export copy")) {
            return false;
        }
        const bool perf_enabled = vkvv_perf_enabled();
        const auto wait_start   = perf_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        export_lock.unlock();
        const bool waited = wait_for_command_fence(runtime, std::numeric_limits<uint64_t>::max(), reason, reason_size, "surface export copy");
        export_lock.lock();
        if (!waited) {
            return false;
        }
        if (perf_enabled) {
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
            const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count();
            runtime->perf.export_copy_count.fetch_add(1, std::memory_order_relaxed);
            runtime->perf.export_copy_targets.fetch_add(copy_targets, std::memory_order_relaxed);
            runtime->perf.export_copy_bytes.fetch_add(copy_bytes, std::memory_order_relaxed);
            runtime->perf.export_copy_wait_ns.fetch_add(static_cast<uint64_t>(wait_ns), std::memory_order_relaxed);
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
            owner_export->content_generation     = source->content_generation;
            owner_export->predecode_exported     = false;
            owner_export->predecode_seeded       = false;
            owner_export->black_placeholder      = false;
            owner_export->seed_source_surface_id = VA_INVALID_ID;
            owner_export->seed_source_generation = 0;
        }
        for (size_t i = 0; i < predecode_seed_targets.size(); i++) {
            ExportResource*                 target   = predecode_seed_targets[i];
            const ExportCopyTargetSnapshot& snapshot = predecode_snapshots[i];
            if (!source_matches || !export_copy_target_still_matches(snapshot) || !predecode_seed_target_matches(target, source)) {
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
                target->predecode_seeded       = true;
                target->seed_source_surface_id = source->surface_id;
                target->seed_source_generation = source->content_generation;
            } else {
                target->content_generation = source->content_generation;
            }
            VKVV_TRACE("export-predecode-seeded",
                       "source_surface=%u source_stream=%llu source_codec=0x%x source_gen=%llu target_owner=%u target_mem=0x%llx target_gen=%llu target_predecode=%u",
                       source->surface_id, static_cast<unsigned long long>(source->stream_id), source->codec_operation, static_cast<unsigned long long>(source->content_generation),
                       target->owner_surface_id, vkvv_trace_handle(target->memory), static_cast<unsigned long long>(target->content_generation),
                       target->predecode_exported ? 1U : 0U);
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
        if (owner_export_current && predecode_seed_targets.empty()) {
            source->export_seed_generation = source->content_generation;
            remember_export_seed_resource_locked(runtime, source);
            VKVV_TRACE("export-copy-skip-current", "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu seed_gen=%llu shadow_mem=0x%llx shadow_gen=%llu",
                       source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                       static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(source->export_seed_generation),
                       vkvv_trace_handle(source->export_resource.memory), static_cast<unsigned long long>(source->export_resource.content_generation));
            return true;
        }

        ExportResource* export_resource = &source->export_resource;
        VKVV_TRACE("export-copy-targets", "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu owner_copy=%u owner_mem=0x%llx owner_gen=%llu predecode_targets=%zu",
                   source->surface_id, static_cast<unsigned long long>(source->driver_instance_id), static_cast<unsigned long long>(source->stream_id), source->codec_operation,
                   static_cast<unsigned long long>(source->content_generation), owner_export_current ? 0U : 1U, vkvv_trace_handle(source->export_resource.memory),
                   static_cast<unsigned long long>(source->export_resource.content_generation), predecode_seed_targets.size());
        if (!copy_surface_to_export_targets_locked(runtime, source, export_resource, !owner_export_current, predecode_seed_targets, export_lock, reason, reason_size)) {
            return false;
        }
        unregister_predecode_export_resource_locked(runtime, export_resource);
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
            return true;
        }

        std::vector<ExportResource*> targets{&target->export_resource};
        target->export_resource.predecode_exported = true;
        VKVV_TRACE("export-seed-hit",
                   "surface=%u driver=%llu stream=%llu codec=0x%x source_surface=%u source_gen=%llu source_seed_gen=%llu source_shadow_gen=%llu target_mem=0x%llx target_gen=%llu",
                   target->surface_id, static_cast<unsigned long long>(target->driver_instance_id), static_cast<unsigned long long>(target->stream_id), target->codec_operation,
                   source->surface_id, static_cast<unsigned long long>(source->content_generation), static_cast<unsigned long long>(source->export_seed_generation),
                   static_cast<unsigned long long>(source->export_resource.content_generation), vkvv_trace_handle(target->export_resource.memory),
                   static_cast<unsigned long long>(target->export_resource.content_generation));
        return copy_surface_to_export_targets_locked(runtime, source, nullptr, false, targets, export_lock, reason, reason_size);
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
