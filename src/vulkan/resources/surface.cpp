#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>
#include <drm_fourcc.h>

namespace vkvv {

    VulkanRuntime::~VulkanRuntime() {
        destroy_command_resources();
        destroy_detached_export_resources();
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    bool enumerate_drm_format_modifiers(VulkanRuntime* runtime, VkFormat format, VkFormatFeatureFlags2 required, std::vector<uint64_t>* modifiers) {
        VkDrmFormatModifierPropertiesList2EXT list{};
        list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT;

        VkFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        properties.pNext = &list;
        vkGetPhysicalDeviceFormatProperties2(runtime->physical_device, format, &properties);
        if (list.drmFormatModifierCount == 0) {
            return false;
        }

        std::vector<VkDrmFormatModifierProperties2EXT> modifier_properties(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties = modifier_properties.data();
        vkGetPhysicalDeviceFormatProperties2(runtime->physical_device, format, &properties);

        modifiers->clear();
        modifiers->reserve(list.drmFormatModifierCount);
        for (const VkDrmFormatModifierProperties2EXT& property : modifier_properties) {
            if ((property.drmFormatModifierTilingFeatures & required) == required) {
                modifiers->push_back(property.drmFormatModifier);
            }
        }
        return !modifiers->empty();
    }

    bool enumerate_decode_drm_format_modifiers(VulkanRuntime* runtime, const DecodeImageKey& key, std::vector<uint64_t>* modifiers) {
        return enumerate_drm_format_modifiers(runtime, key.picture_format, VK_FORMAT_FEATURE_2_VIDEO_DECODE_OUTPUT_BIT_KHR | VK_FORMAT_FEATURE_2_VIDEO_DECODE_DPB_BIT_KHR,
                                              modifiers);
    }

    bool decode_image_key_matches(const DecodeImageKey& existing, const DecodeImageKey& requested) {
        return existing.codec_operation == requested.codec_operation && existing.codec_profile == requested.codec_profile && existing.picture_format == requested.picture_format &&
            existing.reference_picture_format == requested.reference_picture_format && existing.va_rt_format == requested.va_rt_format &&
            existing.va_fourcc == requested.va_fourcc && existing.coded_extent.width >= requested.coded_extent.width &&
            existing.coded_extent.height >= requested.coded_extent.height && existing.usage == requested.usage && existing.create_flags == requested.create_flags &&
            existing.tiling == requested.tiling && existing.chroma_subsampling == requested.chroma_subsampling && existing.luma_bit_depth == requested.luma_bit_depth &&
            existing.chroma_bit_depth == requested.chroma_bit_depth;
    }

    bool encode_image_key_matches(const EncodeImageKey& existing, const EncodeImageKey& requested) {
        return existing.codec_operation == requested.codec_operation && existing.codec_profile == requested.codec_profile && existing.picture_format == requested.picture_format &&
            existing.va_rt_format == requested.va_rt_format && existing.va_fourcc == requested.va_fourcc && existing.coded_extent.width >= requested.coded_extent.width &&
            existing.coded_extent.height >= requested.coded_extent.height && existing.usage == requested.usage && existing.create_flags == requested.create_flags &&
            existing.tiling == requested.tiling && existing.chroma_subsampling == requested.chroma_subsampling && existing.luma_bit_depth == requested.luma_bit_depth &&
            existing.chroma_bit_depth == requested.chroma_bit_depth;
    }

    VkFormat encode_input_vk_format(unsigned int fourcc) {
        switch (fourcc) {
            case VA_FOURCC_NV12: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
            case VA_FOURCC_P010: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    VkVideoComponentBitDepthFlagBitsKHR encode_input_bit_depth(unsigned int fourcc) {
        return fourcc == VA_FOURCC_P010 ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR : VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    }

    unsigned int encode_input_rt_format(unsigned int fourcc) {
        return fourcc == VA_FOURCC_P010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    }

    bool make_encode_input_key(const VkvvSurface* surface, const VkvvImage* image, EncodeImageKey* key, char* reason, size_t reason_size) {
        if (surface == nullptr || image == nullptr || key == nullptr) {
            std::snprintf(reason, reason_size, "missing encode input key source");
            return false;
        }
        if (image->image.format.fourcc != VA_FOURCC_NV12) {
            std::snprintf(reason, reason_size, "H.264 encode input only supports NV12 currently");
            return false;
        }
        const VkFormat format = encode_input_vk_format(image->image.format.fourcc);
        if (format == VK_FORMAT_UNDEFINED) {
            std::snprintf(reason, reason_size, "unsupported encode input fourcc=0x%x", image->image.format.fourcc);
            return false;
        }

        const VkVideoComponentBitDepthFlagBitsKHR bit_depth = encode_input_bit_depth(image->image.format.fourcc);
        *key                                                = {
            .codec_operation    = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
            .codec_profile      = STD_VIDEO_H264_PROFILE_IDC_HIGH,
            .picture_format     = format,
            .va_rt_format       = encode_input_rt_format(image->image.format.fourcc),
            .va_fourcc          = image->image.format.fourcc,
            .coded_extent       = {surface->width, surface->height},
            .usage              = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .create_flags       = 0,
            .tiling             = VK_IMAGE_TILING_OPTIMAL,
            .chroma_subsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            .luma_bit_depth     = bit_depth,
            .chroma_bit_depth   = bit_depth,
        };
        return true;
    }

    bool create_host_transfer_buffer(VulkanRuntime* runtime, const void* data, VkDeviceSize data_size, UploadBuffer* staging, char* reason, size_t reason_size) {
        if (runtime == nullptr || data == nullptr || data_size == 0 || staging == nullptr) {
            std::snprintf(reason, reason_size, "missing encode input upload buffer data");
            return false;
        }

        staging->size = data_size;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size        = staging->size;
        buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result         = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &staging->buffer);
        if (!record_vk_result(runtime, result, "vkCreateBuffer", "encode input upload", reason, reason_size)) {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(runtime->device, staging->buffer, &requirements);
        staging->capacity        = data_size;
        staging->allocation_size = requirements.size;

        uint32_t memory_type_index = 0;
        staging->coherent          = true;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &memory_type_index)) {
            staging->coherent = false;
            if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index)) {
                destroy_upload_buffer(runtime, staging);
                std::snprintf(reason, reason_size, "no host-visible memory type for encode input upload");
                return false;
            }
        }

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;
        result                        = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &staging->memory);
        if (!record_vk_result(runtime, result, "vkAllocateMemory", "encode input upload", reason, reason_size)) {
            destroy_upload_buffer(runtime, staging);
            return false;
        }

        result = vkBindBufferMemory(runtime->device, staging->buffer, staging->memory, 0);
        if (!record_vk_result(runtime, result, "vkBindBufferMemory", "encode input upload", reason, reason_size)) {
            destroy_upload_buffer(runtime, staging);
            return false;
        }

        void* mapped = nullptr;
        result       = vkMapMemory(runtime->device, staging->memory, 0, staging->size, 0, &mapped);
        if (!record_vk_result(runtime, result, "vkMapMemory", "encode input upload", reason, reason_size)) {
            destroy_upload_buffer(runtime, staging);
            return false;
        }
        std::memcpy(mapped, data, static_cast<size_t>(data_size));
        if (!staging->coherent) {
            VkMappedMemoryRange range{};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = staging->memory;
            range.offset = 0;
            range.size   = VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(runtime->device, 1, &range);
        }
        vkUnmapMemory(runtime->device, staging->memory);
        return true;
    }

    bool fill_encode_input_copy_regions(const VAImage& image, VkBufferImageCopy* regions, uint32_t* region_count, char* reason, size_t reason_size) {
        if (regions == nullptr || region_count == nullptr || image.num_planes < 2) {
            std::snprintf(reason, reason_size, "invalid encode input image layout");
            return false;
        }

        struct Plane {
            VkImageAspectFlags aspect;
            uint32_t           width_divisor;
            uint32_t           height_divisor;
            uint32_t           texel_bytes;
            uint32_t           pitch;
            uint32_t           offset;
        };

        Plane planes[2]{};
        if (image.format.fourcc == VA_FOURCC_NV12) {
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, 1, 1, 1, image.pitches[0], image.offsets[0]};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, 2, 2, 2, image.pitches[1], image.offsets[1]};
        } else if (image.format.fourcc == VA_FOURCC_P010) {
            planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, 1, 1, 2, image.pitches[0], image.offsets[0]};
            planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, 2, 2, 4, image.pitches[1], image.offsets[1]};
        } else {
            std::snprintf(reason, reason_size, "unsupported encode input copy fourcc=0x%x", image.format.fourcc);
            return false;
        }

        for (uint32_t i = 0; i < 2; i++) {
            const uint32_t extent_width  = (image.width + planes[i].width_divisor - 1u) / planes[i].width_divisor;
            const uint32_t extent_height = (image.height + planes[i].height_divisor - 1u) / planes[i].height_divisor;
            if (planes[i].texel_bytes == 0 || planes[i].pitch % planes[i].texel_bytes != 0) {
                std::snprintf(reason, reason_size, "unsupported encode input pitch for plane %u", i);
                return false;
            }
            const uint32_t row_length = planes[i].pitch / planes[i].texel_bytes;
            if (row_length < extent_width) {
                std::snprintf(reason, reason_size, "encode input pitch too small for plane %u", i);
                return false;
            }

            regions[i]                             = {};
            regions[i].bufferOffset                = planes[i].offset;
            regions[i].bufferRowLength             = row_length == extent_width ? 0 : row_length;
            regions[i].bufferImageHeight           = 0;
            regions[i].imageSubresource.aspectMask = planes[i].aspect;
            regions[i].imageSubresource.layerCount = 1;
            regions[i].imageExtent                 = {extent_width, extent_height, 1};
        }
        *region_count = 2;
        return true;
    }

    VkPipelineStageFlags2 destination_stage_for_layout(VkImageLayout layout) {
        switch (layout) {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            case VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR:
            case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR:
            case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR: return VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
            case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR:
            case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
            case VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR: return VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
            default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    void destroy_export_resource(VulkanRuntime* runtime, ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        if (resource->image != VK_NULL_HANDLE) {
            vkDestroyImage(runtime->device, resource->image, nullptr);
            resource->image = VK_NULL_HANDLE;
        }
        if (resource->memory != VK_NULL_HANDLE) {
            vkFreeMemory(runtime->device, resource->memory, nullptr);
            resource->memory = VK_NULL_HANDLE;
        }
        resource->driver_instance_id      = 0;
        resource->stream_id               = 0;
        resource->codec_operation         = 0;
        resource->owner_surface_id        = VA_INVALID_ID;
        resource->extent                  = {};
        resource->format                  = VK_FORMAT_UNDEFINED;
        resource->va_fourcc               = 0;
        resource->allocation_size         = 0;
        resource->plane_layouts[0]        = {};
        resource->plane_layouts[1]        = {};
        resource->plane_count             = 0;
        resource->drm_format_modifier     = 0;
        resource->has_drm_format_modifier = false;
        resource->exported                = false;
        resource->predecode_exported      = false;
        resource->predecode_seeded        = false;
        resource->black_placeholder       = false;
        resource->seed_source_surface_id  = VA_INVALID_ID;
        resource->seed_source_generation  = 0;
        resource->content_generation      = 0;
        resource->fd_stat_valid           = false;
        resource->fd_dev                  = 0;
        resource->fd_ino                  = 0;
        resource->layout                  = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    VkDeviceSize export_memory_bytes(const SurfaceResource* resource) {
        if (resource == nullptr) {
            return 0;
        }
        return resource->export_resource.allocation_size;
    }

    size_t runtime_retained_export_count(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        return runtime->retained_exports.size();
    }

    VkDeviceSize runtime_retained_export_memory_bytes(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        return runtime->retained_export_memory_bytes;
    }

    size_t runtime_detached_export_count(VulkanRuntime* runtime) {
        return runtime_retained_export_count(runtime);
    }

    VkDeviceSize runtime_detached_export_memory_bytes(VulkanRuntime* runtime) {
        return runtime_retained_export_memory_bytes(runtime);
    }

    void VulkanRuntime::destroy_detached_export_resources() {
        std::lock_guard<std::mutex> lock(export_mutex);
        predecode_exports.clear();
        export_seed_records.clear();
        for (RetainedExportBacking& backing : retained_exports) {
            destroy_export_resource(this, &backing.resource);
        }
        retained_exports.clear();
        retained_export_memory_bytes = 0;
    }

    void unregister_predecode_export_resource_locked(VulkanRuntime* runtime, ExportResource* resource) {
        if (runtime == nullptr || resource == nullptr) {
            return;
        }
        auto& exports = runtime->predecode_exports;
        exports.erase(std::remove(exports.begin(), exports.end(), resource), exports.end());
    }

    void unregister_predecode_export_resource(VulkanRuntime* runtime, ExportResource* resource) {
        if (runtime == nullptr || resource == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        unregister_predecode_export_resource_locked(runtime, resource);
    }

    void register_predecode_export_resource(VulkanRuntime* runtime, ExportResource* resource) {
        if (runtime == nullptr || resource == nullptr || resource->image == VK_NULL_HANDLE) {
            return;
        }
        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        if (std::find(runtime->predecode_exports.begin(), runtime->predecode_exports.end(), resource) == runtime->predecode_exports.end()) {
            runtime->predecode_exports.push_back(resource);
        }
    }

    void close_transition_retention_window_locked(VulkanRuntime* runtime, const char* reason) {
        if (runtime == nullptr || !runtime->transition_retention.active) {
            return;
        }

        const TransitionRetentionWindow closed = runtime->transition_retention;
        vkvv_trace("retained-export-window-close", "driver=%llu stream=%llu codec=0x%x retained=%zu retained_mem=%llu attached=%zu target=%zu budget=%llu reason=%s",
                   static_cast<unsigned long long>(closed.driver_instance_id), static_cast<unsigned long long>(closed.stream_id), closed.codec_operation, closed.retained_count,
                   static_cast<unsigned long long>(closed.retained_bytes), closed.attached_count, closed.budget.target_count,
                   static_cast<unsigned long long>(closed.budget.target_bytes), reason != nullptr ? reason : "unknown");
        runtime->transition_retention          = {};
        runtime->retained_export_count_limit   = 4;
        runtime->retained_export_memory_budget = 64ull * 1024ull * 1024ull;
    }

    void refresh_transition_retention_window_locked(VulkanRuntime* runtime, const ExportResource* seed, const char* reason) {
        if (runtime == nullptr) {
            return;
        }
        if (seed != nullptr) {
            TransitionRetentionWindow& window      = runtime->transition_retention;
            const bool                 same_window = retained_export_matches_window(*seed, window);
            if (!same_window) {
                if (!retained_export_seed_can_replace_window(window, *seed)) {
                    vkvv_trace("retained-export-window-keep", "driver=%llu stream=%llu codec=0x%x seed_owner=%u seed_driver=%llu seed_stream=%llu seed_codec=0x%x reason=%s",
                               static_cast<unsigned long long>(window.driver_instance_id), static_cast<unsigned long long>(window.stream_id), window.codec_operation,
                               seed->owner_surface_id, static_cast<unsigned long long>(seed->driver_instance_id), static_cast<unsigned long long>(seed->stream_id),
                               seed->codec_operation, reason != nullptr ? reason : "unknown");
                } else {
                    window                    = {};
                    window.active             = true;
                    window.driver_instance_id = seed->driver_instance_id;
                    window.stream_id          = seed->stream_id;
                    window.codec_operation    = seed->codec_operation;
                    window.format             = seed->format;
                    window.va_fourcc          = seed->va_fourcc;
                    window.coded_extent       = seed->extent;
                    vkvv_trace("retained-export-window-open", "driver=%llu stream=%llu codec=0x%x fourcc=0x%x format=%d extent=%ux%u reason=%s",
                               static_cast<unsigned long long>(window.driver_instance_id), static_cast<unsigned long long>(window.stream_id), window.codec_operation,
                               window.va_fourcc, window.format, window.coded_extent.width, window.coded_extent.height, reason != nullptr ? reason : "unknown");
                }
            }
        }

        TransitionRetentionWindow& window = runtime->transition_retention;
        if (!window.active) {
            return;
        }

        size_t       retained_count = 0;
        VkDeviceSize retained_bytes = 0;
        for (const RetainedExportBacking& backing : runtime->retained_exports) {
            if (retained_export_matches_window(backing.resource, window)) {
                retained_count++;
                retained_bytes += backing.resource.allocation_size;
            }
        }
        if (retained_count == 0) {
            close_transition_retention_window_locked(runtime, reason);
            return;
        }

        window.retained_count                  = retained_count;
        window.retained_bytes                  = retained_bytes;
        const VkDeviceSize global_cap          = retained_export_global_cap_bytes(runtime->memory_properties);
        window.budget                          = retained_export_budget_from_expected(retained_count, retained_bytes, global_cap);
        runtime->retained_export_count_limit   = std::max<size_t>(4, window.budget.target_count);
        runtime->retained_export_memory_budget = std::max<VkDeviceSize>(64ull * 1024ull * 1024ull, window.budget.target_bytes);
        vkvv_trace("retained-export-budget",
                   "driver=%llu stream=%llu codec=0x%x retained=%zu retained_mem=%llu average=%llu headroom=%zu target=%zu budget=%llu global_cap=%llu reason=%s",
                   static_cast<unsigned long long>(window.driver_instance_id), static_cast<unsigned long long>(window.stream_id), window.codec_operation, window.retained_count,
                   static_cast<unsigned long long>(window.retained_bytes), static_cast<unsigned long long>(window.budget.average_bytes), window.budget.headroom_count,
                   window.budget.target_count, static_cast<unsigned long long>(window.budget.target_bytes), static_cast<unsigned long long>(window.budget.global_cap_bytes),
                   reason != nullptr ? reason : "unknown");
    }

    void prune_detached_export_resources_locked(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return;
        }

        while (!runtime->retained_exports.empty() &&
               (runtime->retained_exports.size() > runtime->retained_export_count_limit || runtime->retained_export_memory_bytes > runtime->retained_export_memory_budget)) {
            size_t victim_index = 0;
            if (runtime->transition_retention.active) {
                for (size_t i = 0; i < runtime->retained_exports.size(); i++) {
                    if (!retained_export_matches_window(runtime->retained_exports[i].resource, runtime->transition_retention)) {
                        victim_index = i;
                        break;
                    }
                }
            }
            RetainedExportBacking& oldest_backing = runtime->retained_exports[victim_index];
            ExportResource&        oldest         = oldest_backing.resource;
            const VkDeviceSize     bytes          = oldest.allocation_size;
            const bool             window_match   = retained_export_matches_window(oldest, runtime->transition_retention);
            oldest_backing.state                  = RetainedExportState::Expired;
            vkvv_trace("retained-export-prune",
                       "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx bytes=%llu fd_stat=%u fd_dev=%llu fd_ino=%llu retained=%zu retained_mem=%llu limit=%zu budget=%llu "
                       "window_match=%u",
                       oldest.owner_surface_id, static_cast<unsigned long long>(oldest.driver_instance_id), static_cast<unsigned long long>(oldest.stream_id),
                       oldest.codec_operation, vkvv_trace_handle(oldest.memory), static_cast<unsigned long long>(bytes), oldest_backing.fd.valid ? 1U : 0U,
                       static_cast<unsigned long long>(oldest_backing.fd.dev), static_cast<unsigned long long>(oldest_backing.fd.ino), runtime->retained_exports.size(),
                       static_cast<unsigned long long>(runtime->retained_export_memory_bytes), runtime->retained_export_count_limit,
                       static_cast<unsigned long long>(runtime->retained_export_memory_budget), window_match ? 1U : 0U);
            destroy_export_resource(runtime, &oldest);
            runtime->retained_exports.erase(runtime->retained_exports.begin() + static_cast<std::ptrdiff_t>(victim_index));
            runtime->retained_export_memory_bytes = runtime->retained_export_memory_bytes > bytes ? runtime->retained_export_memory_bytes - bytes : 0;
            refresh_transition_retention_window_locked(runtime, nullptr, "prune");
        }
    }

    void note_retained_export_attached_locked(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return;
        }
        if (runtime->transition_retention.active) {
            runtime->transition_retention.attached_count++;
        }
        refresh_transition_retention_window_locked(runtime, nullptr, "attach");
    }

    bool detached_export_same_owner(const RetainedExportBacking& backing, uint64_t driver_instance_id, VASurfaceID surface_id) {
        const ExportResource& resource = backing.resource;
        return driver_instance_id != 0 && resource.driver_instance_id == driver_instance_id && resource.owner_surface_id == surface_id;
    }

    bool detached_export_exact_match(const RetainedExportBacking& backing, uint64_t driver_instance_id, VASurfaceID surface_id, uint64_t stream_id,
                                     VkVideoCodecOperationFlagsKHR codec_operation, unsigned int va_fourcc, VkFormat format, VkExtent2D coded_extent) {
        const ExportResource& resource = backing.resource;
        return format != VK_FORMAT_UNDEFINED && detached_export_same_owner(backing, driver_instance_id, surface_id) && resource.stream_id == stream_id &&
            resource.codec_operation == codec_operation && resource.va_fourcc == va_fourcc && resource.format == format && resource.extent.width == coded_extent.width &&
            resource.extent.height == coded_extent.height;
    }

    void remove_detached_export_locked(VulkanRuntime* runtime, size_t index) {
        RetainedExportBacking& backing  = runtime->retained_exports[index];
        ExportResource&        resource = backing.resource;
        const VkDeviceSize     bytes    = resource.allocation_size;
        backing.state                   = RetainedExportState::Expired;
        destroy_export_resource(runtime, &resource);
        runtime->retained_exports.erase(runtime->retained_exports.begin() + static_cast<std::ptrdiff_t>(index));
        runtime->retained_export_memory_bytes = runtime->retained_export_memory_bytes > bytes ? runtime->retained_export_memory_bytes - bytes : 0;
        refresh_transition_retention_window_locked(runtime, nullptr, "remove");
    }

    void prune_detached_exports_for_surface(VulkanRuntime* runtime, uint64_t driver_instance_id, VASurfaceID surface_id, uint64_t stream_id,
                                            VkVideoCodecOperationFlagsKHR codec_operation, unsigned int va_fourcc, VkFormat format, VkExtent2D coded_extent) {
        if (runtime == nullptr || driver_instance_id == 0 || surface_id == VA_INVALID_ID) {
            return;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        for (size_t i = 0; i < runtime->retained_exports.size();) {
            const RetainedExportBacking& backing = runtime->retained_exports[i];
            if (detached_export_same_owner(backing, driver_instance_id, surface_id) &&
                !detached_export_exact_match(backing, driver_instance_id, surface_id, stream_id, codec_operation, va_fourcc, format, coded_extent)) {
                remove_detached_export_locked(runtime, i);
                continue;
            }
            i++;
        }
    }

    void prune_detached_exports_for_driver(VulkanRuntime* runtime, uint64_t driver_instance_id) {
        if (runtime == nullptr || driver_instance_id == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        for (size_t i = 0; i < runtime->retained_exports.size();) {
            if (runtime->retained_exports[i].resource.driver_instance_id == driver_instance_id) {
                remove_detached_export_locked(runtime, i);
                continue;
            }
            i++;
        }
    }

    void detach_export_resource(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (runtime == nullptr || resource == nullptr || resource->export_resource.image == VK_NULL_HANDLE) {
            return;
        }

        unregister_predecode_export_resource(runtime, &resource->export_resource);
        resource->exported = false;
        RetainedExportBacking backing{};
        ExportResource&       detached = backing.resource;
        detached                       = resource->export_resource;
        resource->export_resource      = {};
        detached.driver_instance_id    = resource->driver_instance_id;
        detached.stream_id             = resource->stream_id;
        detached.codec_operation       = resource->codec_operation;
        detached.owner_surface_id      = resource->surface_id;
        if (!detached.exported || detached.allocation_size == 0 || runtime->device_lost) {
            destroy_export_resource(runtime, &detached);
            return;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        try {
            for (size_t i = 0; i < runtime->retained_exports.size();) {
                if (detached_export_same_owner(runtime->retained_exports[i], detached.driver_instance_id, detached.owner_surface_id)) {
                    remove_detached_export_locked(runtime, i);
                    continue;
                }
                i++;
            }
            backing.fd                = retained_export_fd_identity(detached);
            backing.state             = RetainedExportState::Detached;
            backing.retained_sequence = ++runtime->retained_export_sequence;
            runtime->retained_export_memory_bytes += detached.allocation_size;
            vkvv_trace("retained-export-add",
                       "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx bytes=%llu fd_stat=%u fd_dev=%llu fd_ino=%llu retained=%zu retained_mem=%llu seq=%llu",
                       detached.owner_surface_id, static_cast<unsigned long long>(detached.driver_instance_id), static_cast<unsigned long long>(detached.stream_id),
                       detached.codec_operation, vkvv_trace_handle(detached.memory), static_cast<unsigned long long>(detached.allocation_size), backing.fd.valid ? 1U : 0U,
                       static_cast<unsigned long long>(backing.fd.dev), static_cast<unsigned long long>(backing.fd.ino), runtime->retained_exports.size() + 1,
                       static_cast<unsigned long long>(runtime->retained_export_memory_bytes), static_cast<unsigned long long>(backing.retained_sequence));
            runtime->retained_exports.push_back(std::move(backing));
            refresh_transition_retention_window_locked(runtime, &runtime->retained_exports.back().resource, "detach");
            prune_detached_export_resources_locked(runtime);
        } catch (...) { destroy_export_resource(runtime, &detached); }
    }

    void destroy_decode_resource_handles(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }
        if (resource->view != VK_NULL_HANDLE) {
            vkDestroyImageView(runtime->device, resource->view, nullptr);
            resource->view = VK_NULL_HANDLE;
        }
        if (resource->image != VK_NULL_HANDLE) {
            vkDestroyImage(runtime->device, resource->image, nullptr);
            resource->image = VK_NULL_HANDLE;
        }
        if (resource->memory != VK_NULL_HANDLE) {
            vkFreeMemory(runtime->device, resource->memory, nullptr);
            resource->memory = VK_NULL_HANDLE;
        }
        resource->allocation_size                        = 0;
        resource->stream_id                              = 0;
        resource->codec_operation                        = 0;
        resource->plane_layouts[0]                       = {};
        resource->plane_layouts[1]                       = {};
        resource->plane_count                            = 0;
        resource->drm_format_modifier                    = 0;
        resource->exportable                             = false;
        resource->has_drm_format_modifier                = false;
        resource->decode_key                             = {};
        resource->encode_key                             = {};
        resource->layout                                 = VK_IMAGE_LAYOUT_UNDEFINED;
        resource->last_nondisplay_skip_generation        = 0;
        resource->last_nondisplay_skip_shadow_generation = 0;
        resource->last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
        resource->last_display_refresh_generation        = 0;
    }

    void destroy_surface_resource_raw(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }

        unregister_export_seed_resource(runtime, resource);
        detach_export_resource(runtime, resource);
        destroy_decode_resource_handles(runtime, resource);
        delete resource;
    }

    void destroy_surface_resource(VulkanRuntime* runtime, VkvvSurface* surface) {
        if (surface == nullptr || surface->vulkan == nullptr) {
            return;
        }

        destroy_surface_resource_raw(runtime, static_cast<SurfaceResource*>(surface->vulkan));
        surface->vulkan      = nullptr;
        surface->work_state  = VKVV_SURFACE_WORK_READY;
        surface->sync_status = VA_STATUS_SUCCESS;
        surface->decoded     = false;
    }

    bool ensure_surface_resource(VulkanRuntime* runtime, VkvvSurface* surface, const DecodeImageKey& key, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, "decode surface resource")) {
            return false;
        }
        if (surface == nullptr) {
            std::snprintf(reason, reason_size, "missing target surface");
            return false;
        }
        if (key.codec_operation == 0 || key.picture_format == VK_FORMAT_UNDEFINED || key.reference_picture_format == VK_FORMAT_UNDEFINED || key.coded_extent.width == 0 ||
            key.coded_extent.height == 0 || key.usage == 0) {
            std::snprintf(reason, reason_size, "invalid decode image key");
            return false;
        }

        auto* existing = static_cast<SurfaceResource*>(surface->vulkan);
        if (existing != nullptr && existing->image != VK_NULL_HANDLE && decode_image_key_matches(existing->decode_key, key)) {
            const uint64_t stream_id       = surface->stream_id;
            const auto     codec_operation = static_cast<VkVideoCodecOperationFlagsKHR>(key.codec_operation);
            if (existing->stream_id != stream_id || existing->codec_operation != codec_operation) {
                unregister_export_seed_resource(runtime, existing);
                detach_export_resource(runtime, existing);
                existing->content_generation                     = 0;
                existing->export_seed_generation                 = 0;
                existing->last_nondisplay_skip_generation        = 0;
                existing->last_nondisplay_skip_shadow_generation = 0;
                existing->last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
                existing->last_display_refresh_generation        = 0;
            }
            existing->driver_instance_id = surface->driver_instance_id;
            existing->stream_id          = stream_id;
            existing->codec_operation    = codec_operation;
            existing->surface_id         = surface->id;
            existing->visible_extent     = {surface->width, surface->height};
            existing->import             = surface->import;
            vkvv_trace("surface-resource-reuse",
                       "surface=%u driver=%llu stream=%llu surface_codec=0x%x key_codec=0x%x resource_codec=0x%x content_gen=%llu shadow_gen=%llu predecode=%u imported=%u "
                       "import_fd_stat=%u import_fd_dev=%llu import_fd_ino=%llu",
                       surface->id, static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(stream_id), surface->codec_operation,
                       key.codec_operation, existing->codec_operation, static_cast<unsigned long long>(existing->content_generation),
                       static_cast<unsigned long long>(existing->export_resource.content_generation), existing->export_resource.predecode_exported ? 1U : 0U,
                       existing->import.external ? 1U : 0U, existing->import.fd.valid ? 1U : 0U, static_cast<unsigned long long>(existing->import.fd.dev),
                       static_cast<unsigned long long>(existing->import.fd.ino));
            return true;
        }
        if (existing != nullptr && existing->image != VK_NULL_HANDLE && surface->decoded) {
            std::snprintf(reason, reason_size, "decoded reference surface decode image key mismatch: codec=0x%x format=%d fourcc=0x%x extent=%ux%u", key.codec_operation,
                          key.picture_format, key.va_fourcc, key.coded_extent.width, key.coded_extent.height);
            return false;
        }

        if (existing != nullptr && existing->image != VK_NULL_HANDLE) {
            destroy_surface_resource(runtime, surface);
            existing = nullptr;
        }

        const VkExtent2D       extent = key.coded_extent;
        const VideoProfileSpec profile_spec{
            .operation = static_cast<VkVideoCodecOperationFlagBitsKHR>(key.codec_operation),
            .bit_depth = key.luma_bit_depth,
        };
        VideoProfileChain         profile_chain(profile_spec);
        VkVideoProfileListInfoKHR profile_list{};
        profile_list.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        profile_list.profileCount = 1;
        profile_list.pProfiles    = &profile_chain.profile;

        const bool export_layout_supported     = key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT || key.tiling == VK_IMAGE_TILING_LINEAR;
        const bool export_descriptor_supported = (key.picture_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && key.va_fourcc == VA_FOURCC_NV12) ||
            (key.picture_format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 && key.va_fourcc == VA_FOURCC_P010);
        const bool                      request_exportable = runtime->surface_export && export_descriptor_supported && export_layout_supported;

        VkExternalMemoryImageCreateInfo external_image{};
        external_image.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        std::vector<uint64_t>                     drm_modifiers;
        VkImageDrmFormatModifierListCreateInfoEXT drm_modifier_list{};
        drm_modifier_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            if (!enumerate_decode_drm_format_modifiers(runtime, key, &drm_modifiers)) {
                std::snprintf(reason, reason_size, "no DRM format modifiers support decode surfaces");
                return false;
            }
            drm_modifier_list.drmFormatModifierCount = static_cast<uint32_t>(drm_modifiers.size());
            drm_modifier_list.pDrmFormatModifiers    = drm_modifiers.data();
        }

        if (request_exportable) {
            profile_list.pNext = &external_image;
            if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
                external_image.pNext = &drm_modifier_list;
            }
        } else if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            profile_list.pNext = &drm_modifier_list;
        }

        VkImageCreateInfo image_info{};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext         = &profile_list;
        image_info.flags         = key.create_flags;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.format        = key.picture_format;
        image_info.extent        = {extent.width, extent.height, 1};
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling        = key.tiling;
        image_info.usage         = key.usage;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        auto* resource = existing != nullptr ? existing : new (std::nothrow) SurfaceResource();
        if (resource == nullptr) {
            std::snprintf(reason, reason_size, "out of memory creating surface resource");
            return false;
        }
        const bool new_resource = existing == nullptr;

        VkResult   result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
        if (result != VK_SUCCESS) {
            if (new_resource) {
                delete resource;
            }
            record_vk_result(runtime, result, "vkCreateImage", "decode surface", reason, reason_size);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(runtime->device, resource->image, &requirements);

        uint32_t memory_type_index = 0;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
            !find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, 0, &memory_type_index)) {
            if (new_resource) {
                destroy_surface_resource_raw(runtime, resource);
            } else {
                destroy_decode_resource_handles(runtime, resource);
            }
            std::snprintf(reason, reason_size, "no memory type for decode surface image");
            return false;
        }

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;

        VkMemoryDedicatedAllocateInfo dedicated_allocate{};
        dedicated_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated_allocate.image = resource->image;

        VkExportMemoryAllocateInfo export_allocate{};
        export_allocate.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_allocate.pNext       = &dedicated_allocate;
        export_allocate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (request_exportable) {
            allocate_info.pNext = &export_allocate;
        }

        result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &resource->memory);
        if (result != VK_SUCCESS) {
            if (new_resource) {
                destroy_surface_resource_raw(runtime, resource);
            } else {
                destroy_decode_resource_handles(runtime, resource);
            }
            record_vk_result(runtime, result, "vkAllocateMemory", "decode surface", reason, reason_size);
            return false;
        }

        result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
        if (result != VK_SUCCESS) {
            if (new_resource) {
                destroy_surface_resource_raw(runtime, resource);
            } else {
                destroy_decode_resource_handles(runtime, resource);
            }
            record_vk_result(runtime, result, "vkBindImageMemory", "decode surface", reason, reason_size);
            return false;
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                           = resource->image;
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                          = key.picture_format;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 1;

        result = vkCreateImageView(runtime->device, &view_info, nullptr, &resource->view);
        if (result != VK_SUCCESS) {
            if (new_resource) {
                destroy_surface_resource_raw(runtime, resource);
            } else {
                destroy_decode_resource_handles(runtime, resource);
            }
            record_vk_result(runtime, result, "vkCreateImageView", "decode surface", reason, reason_size);
            return false;
        }

        resource->extent                                 = extent;
        resource->coded_extent                           = extent;
        resource->visible_extent                         = {surface->width, surface->height};
        resource->driver_instance_id                     = surface->driver_instance_id;
        resource->stream_id                              = surface->stream_id;
        resource->codec_operation                        = static_cast<VkVideoCodecOperationFlagsKHR>(key.codec_operation);
        resource->surface_id                             = surface->id;
        resource->format                                 = key.picture_format;
        resource->va_rt_format                           = key.va_rt_format;
        resource->va_fourcc                              = key.va_fourcc;
        resource->decode_key                             = key;
        resource->allocation_size                        = requirements.size;
        resource->import                                 = surface->import;
        resource->last_nondisplay_skip_generation        = 0;
        resource->last_nondisplay_skip_shadow_generation = 0;
        resource->last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
        resource->last_display_refresh_generation        = 0;
        if (request_exportable) {
            VkImageSubresource plane0{};
            plane0.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            VkImageSubresource plane1{};
            plane1.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            vkGetImageSubresourceLayout(runtime->device, resource->image, &plane0, &resource->plane_layouts[0]);
            vkGetImageSubresourceLayout(runtime->device, resource->image, &plane1, &resource->plane_layouts[1]);
            resource->plane_count = 2;
            resource->exportable  = true;

            if (key.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
                VkImageDrmFormatModifierPropertiesEXT modifier_properties{};
                modifier_properties.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
                result                    = runtime->get_image_drm_format_modifier_properties(runtime->device, resource->image, &modifier_properties);
                if (result != VK_SUCCESS) {
                    if (new_resource) {
                        destroy_surface_resource_raw(runtime, resource);
                    } else {
                        destroy_decode_resource_handles(runtime, resource);
                    }
                    std::snprintf(reason, reason_size, "vkGetImageDrmFormatModifierPropertiesEXT failed: %d", result);
                    return false;
                }
                resource->drm_format_modifier     = modifier_properties.drmFormatModifier;
                resource->has_drm_format_modifier = true;
            } else {
                resource->drm_format_modifier     = DRM_FORMAT_MOD_LINEAR;
                resource->has_drm_format_modifier = true;
            }
        }
        if (new_resource) {
            surface->vulkan = resource;
        }
        vkvv_trace("surface-resource-create",
                   "surface=%u driver=%llu stream=%llu surface_codec=0x%x key_codec=0x%x resource_codec=0x%x extent=%ux%u exportable=%u decode_mem=%llu shadow_mem=0x%llx "
                   "imported=%u import_fd_stat=%u import_fd_dev=%llu import_fd_ino=%llu",
                   surface->id, static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), surface->codec_operation,
                   key.codec_operation, resource->codec_operation, extent.width, extent.height, resource->exportable ? 1U : 0U,
                   static_cast<unsigned long long>(resource->allocation_size), vkvv_trace_handle(resource->export_resource.memory), resource->import.external ? 1U : 0U,
                   resource->import.fd.valid ? 1U : 0U, static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino));
        return true;
    }

    bool ensure_encode_input_resource(VulkanRuntime* runtime, VkvvSurface* surface, const EncodeImageKey& key, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, "encode input surface resource")) {
            return false;
        }
        if (surface == nullptr) {
            std::snprintf(reason, reason_size, "missing encode input surface");
            return false;
        }
        if (key.codec_operation == 0 || key.picture_format == VK_FORMAT_UNDEFINED || key.coded_extent.width == 0 || key.coded_extent.height == 0 ||
            (key.usage & (VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR)) == 0) {
            std::snprintf(reason, reason_size, "invalid encode image key");
            return false;
        }
        if ((runtime->enabled_encode_operations & key.codec_operation) == 0) {
            std::snprintf(reason, reason_size, "Vulkan runtime did not enable encode operation 0x%x", key.codec_operation);
            return false;
        }

        auto* existing = static_cast<SurfaceResource*>(surface->vulkan);
        if (existing != nullptr && existing->image != VK_NULL_HANDLE && encode_image_key_matches(existing->encode_key, key)) {
            existing->driver_instance_id = surface->driver_instance_id;
            existing->stream_id          = surface->stream_id;
            existing->codec_operation    = key.codec_operation;
            existing->surface_id         = surface->id;
            existing->visible_extent     = {surface->width, surface->height};
            existing->import             = surface->import;
            if ((key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) != 0) {
                surface->role_flags |= VKVV_SURFACE_ROLE_ENCODE_INPUT;
            }
            if ((key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR) != 0) {
                surface->role_flags |= VKVV_SURFACE_ROLE_ENCODE_RECONSTRUCTED | VKVV_SURFACE_ROLE_ENCODE_REFERENCE;
            }
            vkvv_trace("encode-input-resource-reuse", "surface=%u driver=%llu stream=%llu codec=0x%x extent=%ux%u mem=%llu", surface->id,
                       static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), existing->codec_operation,
                       existing->extent.width, existing->extent.height, static_cast<unsigned long long>(existing->allocation_size));
            return true;
        }
        if (existing != nullptr && existing->image != VK_NULL_HANDLE) {
            destroy_surface_resource(runtime, surface);
            existing = nullptr;
        }

        const VideoProfileSpec profile_spec{
            .operation   = static_cast<VkVideoCodecOperationFlagBitsKHR>(key.codec_operation),
            .bit_depth   = key.luma_bit_depth,
            .std_profile = key.codec_profile,
        };
        VideoProfileChain         profile_chain(profile_spec);
        VkVideoProfileListInfoKHR profile_list{};
        profile_list.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        profile_list.profileCount = 1;
        profile_list.pProfiles    = &profile_chain.profile;

        VkImageCreateInfo image_info{};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext         = &profile_list;
        image_info.flags         = key.create_flags;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.format        = key.picture_format;
        image_info.extent        = {key.coded_extent.width, key.coded_extent.height, 1};
        image_info.mipLevels     = 1;
        image_info.arrayLayers   = 1;
        image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling        = key.tiling;
        image_info.usage         = key.usage;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        auto* resource = new (std::nothrow) SurfaceResource();
        if (resource == nullptr) {
            std::snprintf(reason, reason_size, "out of memory creating encode input resource");
            return false;
        }

        VkResult result = vkCreateImage(runtime->device, &image_info, nullptr, &resource->image);
        if (result != VK_SUCCESS) {
            destroy_surface_resource_raw(runtime, resource);
            record_vk_result(runtime, result, "vkCreateImage", "encode input surface", reason, reason_size);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(runtime->device, resource->image, &requirements);

        uint32_t memory_type_index = 0;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
            !find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, 0, &memory_type_index)) {
            destroy_surface_resource_raw(runtime, resource);
            std::snprintf(reason, reason_size, "no memory type for encode input image");
            return false;
        }

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;
        result                        = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &resource->memory);
        if (result != VK_SUCCESS) {
            destroy_surface_resource_raw(runtime, resource);
            record_vk_result(runtime, result, "vkAllocateMemory", "encode input surface", reason, reason_size);
            return false;
        }

        result = vkBindImageMemory(runtime->device, resource->image, resource->memory, 0);
        if (result != VK_SUCCESS) {
            destroy_surface_resource_raw(runtime, resource);
            record_vk_result(runtime, result, "vkBindImageMemory", "encode input surface", reason, reason_size);
            return false;
        }

        VkImageViewUsageCreateInfo view_usage{};
        view_usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        view_usage.usage = key.usage & (VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR);

        VkImageViewCreateInfo view_info{};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext                           = &view_usage;
        view_info.image                           = resource->image;
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                          = key.picture_format;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 1;
        result                                    = vkCreateImageView(runtime->device, &view_info, nullptr, &resource->view);
        if (result != VK_SUCCESS) {
            destroy_surface_resource_raw(runtime, resource);
            record_vk_result(runtime, result, "vkCreateImageView", "encode input surface", reason, reason_size);
            return false;
        }

        resource->extent             = key.coded_extent;
        resource->coded_extent       = key.coded_extent;
        resource->visible_extent     = {surface->width, surface->height};
        resource->driver_instance_id = surface->driver_instance_id;
        resource->stream_id          = surface->stream_id;
        resource->codec_operation    = key.codec_operation;
        resource->surface_id         = surface->id;
        resource->format             = key.picture_format;
        resource->va_rt_format       = key.va_rt_format;
        resource->va_fourcc          = key.va_fourcc;
        resource->encode_key         = key;
        resource->allocation_size    = requirements.size;
        resource->import             = surface->import;
        resource->exportable         = false;
        surface->vulkan              = resource;
        if ((key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) != 0) {
            surface->role_flags |= VKVV_SURFACE_ROLE_ENCODE_INPUT;
        }
        if ((key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR) != 0) {
            surface->role_flags |= VKVV_SURFACE_ROLE_ENCODE_RECONSTRUCTED | VKVV_SURFACE_ROLE_ENCODE_REFERENCE;
        }

        vkvv_trace("encode-input-resource-create", "surface=%u driver=%llu stream=%llu codec=0x%x extent=%ux%u format=%d fourcc=0x%x mem=%llu", surface->id,
                   static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), resource->codec_operation,
                   resource->extent.width, resource->extent.height, resource->format, resource->va_fourcc, static_cast<unsigned long long>(resource->allocation_size));
        return true;
    }

    void add_image_layout_barrier(std::vector<VkImageMemoryBarrier2>* barriers, SurfaceResource* resource, VkImageLayout new_layout, VkAccessFlags2 dst_access) {
        if (resource->layout == new_layout) {
            return;
        }

        VkImageMemoryBarrier2 barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask                    = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask                   = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask                    = destination_stage_for_layout(new_layout);
        barrier.dstAccessMask                   = dst_access;
        barrier.oldLayout                       = resource->layout;
        barrier.newLayout                       = new_layout;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = resource->image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barriers->push_back(barrier);
        resource->layout = new_layout;
    }

    VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource* resource, VkExtent2D coded_extent) {
        VkVideoPictureResourceInfoKHR picture{};
        picture.sType            = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        picture.codedOffset      = {0, 0};
        picture.codedExtent      = coded_extent;
        picture.baseArrayLayer   = 0;
        picture.imageViewBinding = resource->view;
        return picture;
    }

} // namespace vkvv

using namespace vkvv;

void vkvv_vulkan_surface_destroy(void* runtime_ptr, VkvvSurface* surface) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (runtime == nullptr) {
        return;
    }
    destroy_surface_resource(runtime, surface);
}

VAStatus vkvv_vulkan_upload_encode_input_image(void* runtime_ptr, VkvvSurface* surface, const VkvvImage* image, const void* data, size_t data_size, char* reason,
                                               size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (!ensure_runtime_usable(runtime, reason, reason_size, "encode input upload")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (surface == nullptr || image == nullptr || data == nullptr || data_size < image->image.data_size) {
        std::snprintf(reason, reason_size, "missing encode input upload source");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    EncodeImageKey key{};
    if (!make_encode_input_key(surface, image, &key, reason, reason_size)) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    surface->codec_operation = key.codec_operation;
    surface->role_flags |= VKVV_SURFACE_ROLE_ENCODE_INPUT;
    if (!ensure_encode_input_resource(runtime, surface, key, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    if (resource == nullptr || resource->image == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "encode input upload did not create a Vulkan image");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkBufferImageCopy regions[2]{};
    uint32_t          region_count = 0;
    if (!fill_encode_input_copy_regions(image->image, regions, &region_count, reason, reason_size)) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    UploadBuffer staging{};
    if (!create_host_transfer_buffer(runtime, data, image->image.data_size, &staging, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources_for_queue(runtime, runtime->encode_queue_family, runtime->encode_queue, reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (!record_vk_result(runtime, result, "vkResetFences", "encode input upload", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (!record_vk_result(runtime, result, "vkResetCommandBuffer", "encode input upload", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result           = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (!record_vk_result(runtime, result, "vkBeginCommandBuffer", "encode input upload", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    std::vector<VkImageMemoryBarrier2> barriers;
    add_image_layout_barrier(&barriers, resource, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers    = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }

    vkCmdCopyBufferToImage(runtime->command_buffer, staging.buffer, resource->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region_count, regions);

    barriers.clear();
    add_image_layout_barrier(&barriers, resource, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);
    if (!barriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers    = barriers.data();
        vkCmdPipelineBarrier2(runtime->command_buffer, &dependency);
    }

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (!record_vk_result(runtime, result, "vkEndCommandBuffer", "encode input upload", reason, reason_size)) {
        destroy_upload_buffer(runtime, &staging);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!submit_command_buffer_and_wait_on_queue(runtime, runtime->encode_queue, reason, reason_size, "encode input upload")) {
        destroy_upload_buffer(runtime, &staging);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    destroy_upload_buffer(runtime, &staging);
    resource->content_generation++;
    surface->sync_status = VA_STATUS_SUCCESS;
    surface->work_state  = VKVV_SURFACE_WORK_READY;
    std::snprintf(reason, reason_size, "uploaded H.264 encode input image: surface=%u %ux%u bytes=%u mem=%llu gen=%llu", surface->id, surface->width, surface->height,
                  image->image.data_size, static_cast<unsigned long long>(resource->allocation_size), static_cast<unsigned long long>(resource->content_generation));
    return VA_STATUS_SUCCESS;
}
