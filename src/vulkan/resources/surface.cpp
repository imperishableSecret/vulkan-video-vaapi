#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <new>
#include <vector>
#include <drm_fourcc.h>

namespace vkvv {

    VulkanRuntime::~VulkanRuntime() {
        if (device != VK_NULL_HANDLE && runtime_pending_work_count(this) > 0) {
            char     reason[512] = {};
            VAStatus status      = drain_pending_work_before_sync_command(this, reason, sizeof(reason));
            if (status != VA_STATUS_SUCCESS) {
                VKVV_TRACE("pending-teardown-drain-failed", "status=%d reason=\"%s\"", status, reason);
                discard_pending_work_for_teardown(this, status, reason);
            }
        }
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

    void destroy_export_resource(VulkanRuntime* runtime, ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        if (resource->predecode_quarantined) {
            const bool bootstrap_destroyed_unused = resource->bootstrap_export && resource->content_generation == 0 && !resource->predecode_had_decode_submit;
            trace_predecode_quarantine_outcome(nullptr, resource, bootstrap_destroyed_unused ? "bootstrap-destroyed-unused" : "destroyed", "surface-destroy", false);
            if (bootstrap_destroyed_unused) {
                const uint64_t now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                const uint64_t age_ms = resource->predecode_quarantine_enter_ms != 0 && now_ms >= resource->predecode_quarantine_enter_ms ?
                    now_ms - resource->predecode_quarantine_enter_ms :
                    0;
                VKVV_TRACE("bootstrap-export-destroyed-unused",
                           "surface=%u fd_dev=%llu fd_ino=%llu age_ms=%llu content_gen=%llu had_decode_submit=%u",
                           resource->owner_surface_id, static_cast<unsigned long long>(resource->predecode_fd_dev),
                           static_cast<unsigned long long>(resource->predecode_fd_ino), static_cast<unsigned long long>(age_ms),
                           static_cast<unsigned long long>(resource->content_generation), resource->predecode_had_decode_submit ? 1U : 0U);
                VKVV_TRACE("bootstrap-placeholder-destroyed-unused",
                           "surface=%u fd_dev=%llu fd_ino=%llu age_ms=%llu content_gen=%llu had_decode_submit=%u reason=surface-destroy",
                           resource->owner_surface_id, static_cast<unsigned long long>(resource->predecode_fd_dev),
                           static_cast<unsigned long long>(resource->predecode_fd_ino), static_cast<unsigned long long>(age_ms),
                           static_cast<unsigned long long>(resource->content_generation), resource->predecode_had_decode_submit ? 1U : 0U);
            }
            VKVV_TRACE("export-resource-destroy", "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx fd_dev=%llu fd_ino=%llu content_gen=%llu predecode_quarantined=1",
                       resource->owner_surface_id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                       resource->codec_operation, vkvv_trace_handle(resource->memory), static_cast<unsigned long long>(resource->predecode_fd_dev),
                       static_cast<unsigned long long>(resource->predecode_fd_ino), static_cast<unsigned long long>(resource->predecode_generation));
        }
        trace_export_fd_lifetime(nullptr, resource, "destroy", resource->content_generation, export_resource_fd_may_be_sampled_by_client(resource));
        if (resource->image != VK_NULL_HANDLE) {
            vkDestroyImage(runtime->device, resource->image, nullptr);
            resource->image = VK_NULL_HANDLE;
        }
        if (resource->memory != VK_NULL_HANDLE) {
            vkFreeMemory(runtime->device, resource->memory, nullptr);
            resource->memory = VK_NULL_HANDLE;
        }
        resource->driver_instance_id           = 0;
        resource->stream_id                    = 0;
        resource->codec_operation              = 0;
        resource->owner_surface_id             = VA_INVALID_ID;
        resource->extent                       = {};
        resource->format                       = VK_FORMAT_UNDEFINED;
        resource->va_fourcc                    = 0;
        resource->allocation_size              = 0;
        resource->plane_layouts[0]             = {};
        resource->plane_layouts[1]             = {};
        resource->plane_count                  = 0;
        resource->drm_format_modifier          = 0;
        resource->has_drm_format_modifier      = false;
        resource->exported                     = false;
        resource->predecode_exported           = false;
        resource->predecode_seeded             = false;
        resource->predecode_quarantined        = false;
        resource->predecode_fd_dev             = 0;
        resource->predecode_fd_ino             = 0;
        resource->predecode_generation         = 0;
        resource->predecode_quarantine_enter_ms = 0;
        resource->predecode_had_va_begin       = false;
        resource->predecode_had_decode_submit  = false;
        resource->predecode_had_visible_decode = false;
        resource->bootstrap_export             = false;
        resource->black_placeholder            = false;
        resource->export_role                  = VkvvExportRole::Unknown;
        resource->export_intent                = VkvvExportIntent::Unknown;
        resource->raw_export_flags             = 0;
        resource->export_mem_type              = 0;
        resource->seed_source_surface_id       = VA_INVALID_ID;
        resource->seed_source_generation       = 0;
        resource->seed_pixel_proof_valid       = false;
        resource->content_generation           = 0;
        resource->decode_shadow_generation     = 0;
        resource->decode_shadow_private_active = false;
        resource->fd_stat_valid                = false;
        resource->fd_dev                       = 0;
        resource->fd_ino                       = 0;
        resource->exported_fd                  = {};
        resource->external_sync                = {};
        clear_export_present_state(resource);
        resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
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
        stream_seed_records.clear();
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
        exports.erase(
            std::remove_if(exports.begin(), exports.end(), [resource](const PredecodeExportRecord& record) { return predecode_export_record_matches_resource(record, resource); }),
            exports.end());
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
        PredecodeExportRecord       record = make_predecode_export_record(resource);
        for (PredecodeExportRecord& existing : runtime->predecode_exports) {
            if (predecode_export_record_matches_resource(existing, resource)) {
                existing = record;
                return;
            }
        }
        try {
            runtime->predecode_exports.push_back(record);
        } catch (const std::bad_alloc&) {
            VKVV_TRACE("predecode-export-register-failed", "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx reason=allocation-failed", resource->owner_surface_id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       vkvv_trace_handle(resource->memory));
        }
    }

    PredecodeExportRecord make_predecode_export_record(ExportResource* resource) {
        PredecodeExportRecord record{};
        if (resource == nullptr) {
            return record;
        }
        record.resource           = resource;
        record.image              = resource->image;
        record.memory             = resource->memory;
        record.driver_instance_id = resource->driver_instance_id;
        record.stream_id          = resource->stream_id;
        record.codec_operation    = resource->codec_operation;
        record.owner_surface_id   = resource->owner_surface_id;
        record.format             = resource->format;
        record.va_fourcc          = resource->va_fourcc;
        record.extent             = resource->extent;
        record.content_generation = resource->content_generation;
        return record;
    }

    bool predecode_export_record_matches_resource(const PredecodeExportRecord& record, const ExportResource* resource) {
        if (resource == nullptr) {
            return false;
        }
        return record.resource == resource ||
            (record.memory == resource->memory && record.image == resource->image && record.driver_instance_id == resource->driver_instance_id &&
             record.stream_id == resource->stream_id && record.codec_operation == resource->codec_operation && record.owner_surface_id == resource->owner_surface_id &&
             record.format == resource->format && record.va_fourcc == resource->va_fourcc && record.extent.width == resource->extent.width &&
             record.extent.height == resource->extent.height);
    }

    bool predecode_export_record_still_valid(const PredecodeExportRecord& record) {
        const ExportResource* resource = record.resource;
        return resource != nullptr && record.image != VK_NULL_HANDLE && record.memory != VK_NULL_HANDLE && resource->image == record.image && resource->memory == record.memory &&
            resource->driver_instance_id == record.driver_instance_id && resource->stream_id == record.stream_id && resource->codec_operation == record.codec_operation &&
            resource->owner_surface_id == record.owner_surface_id && resource->format == record.format && resource->va_fourcc == record.va_fourcc &&
            resource->extent.width == record.extent.width && resource->extent.height == record.extent.height && resource->content_generation == record.content_generation &&
            resource->predecode_exported;
    }

    void close_transition_retention_window_locked(VulkanRuntime* runtime, const char* reason) {
        if (runtime == nullptr || !runtime->transition_retention.active) {
            return;
        }

        const TransitionRetentionWindow closed = runtime->transition_retention;
        VKVV_TRACE("retained-export-window-close", "driver=%llu stream=%llu codec=0x%x retained=%zu retained_mem=%llu attached=%zu target=%zu budget=%llu reason=%s",
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
                    VKVV_TRACE("retained-export-window-keep", "driver=%llu stream=%llu codec=0x%x seed_owner=%u seed_driver=%llu seed_stream=%llu seed_codec=0x%x reason=%s",
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
                    VKVV_TRACE("retained-export-window-open", "driver=%llu stream=%llu codec=0x%x fourcc=0x%x format=%d extent=%ux%u reason=%s",
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
        VKVV_TRACE("retained-export-budget",
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
            VKVV_TRACE("retained-export-prune",
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
        const VASurfaceID      owner    = resource.owner_surface_id;
        const uint64_t         driver   = resource.driver_instance_id;
        const uint64_t         stream   = resource.stream_id;
        const auto             codec    = resource.codec_operation;
        backing.state                   = RetainedExportState::Expired;
        destroy_export_resource(runtime, &resource);
        runtime->retained_exports.erase(runtime->retained_exports.begin() + static_cast<std::ptrdiff_t>(index));
        runtime->retained_export_memory_bytes = runtime->retained_export_memory_bytes > bytes ? runtime->retained_export_memory_bytes - bytes : 0;
        VKVV_TRACE("retained-export-remove", "owner=%u driver=%llu stream=%llu codec=0x%x bytes=%llu retained=%zu retained_mem=%llu", owner,
                   static_cast<unsigned long long>(driver), static_cast<unsigned long long>(stream), codec, static_cast<unsigned long long>(bytes),
                   runtime->retained_exports.size(), static_cast<unsigned long long>(runtime->retained_export_memory_bytes));
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

        const bool role_retainable = resource->export_resource.exported && resource->export_resource.client_visible_shadow && !resource->export_resource.private_nondisplay_shadow;
        if (resource->export_resource.private_nondisplay_shadow || (resource->export_resource.exported && !role_retainable)) {
            VKVV_TRACE("retained-role-mismatch-drop", "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx exported=%u client_visible=%u private_only=%u",
                       resource->export_resource.owner_surface_id, static_cast<unsigned long long>(resource->export_resource.driver_instance_id),
                       static_cast<unsigned long long>(resource->export_resource.stream_id), resource->export_resource.codec_operation,
                       vkvv_trace_handle(resource->export_resource.memory), resource->export_resource.exported ? 1U : 0U, resource->export_resource.client_visible_shadow ? 1U : 0U,
                       resource->export_resource.private_nondisplay_shadow ? 1U : 0U);
        }
        const bool                   retainable = role_retainable && resource->export_resource.allocation_size != 0 && !runtime->device_lost;
        std::unique_lock<std::mutex> lock(runtime->export_mutex);
        if (retainable) {
            try {
                runtime->retained_exports.reserve(runtime->retained_exports.size() + 1);
            } catch (const std::bad_alloc&) {
                VKVV_TRACE("retained-export-add-failed", "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx bytes=%llu reason=allocation-failed",
                           resource->export_resource.owner_surface_id, static_cast<unsigned long long>(resource->export_resource.driver_instance_id),
                           static_cast<unsigned long long>(resource->export_resource.stream_id), resource->export_resource.codec_operation,
                           vkvv_trace_handle(resource->export_resource.memory), static_cast<unsigned long long>(resource->export_resource.allocation_size));
                unregister_predecode_export_resource_locked(runtime, &resource->export_resource);
                ExportResource failed_detach = resource->export_resource;
                resource->export_resource    = {};
                resource->exported           = false;
                lock.unlock();
                destroy_export_resource(runtime, &failed_detach);
                return;
            }
        }

        unregister_predecode_export_resource_locked(runtime, &resource->export_resource);
        resource->exported = false;
        clear_surface_export_attach_state(resource);
        RetainedExportBacking backing{};
        ExportResource&       detached        = backing.resource;
        detached                              = resource->export_resource;
        resource->export_resource             = {};
        detached.driver_instance_id           = resource->driver_instance_id;
        detached.stream_id                    = resource->stream_id;
        detached.codec_operation              = resource->codec_operation;
        detached.owner_surface_id             = resource->surface_id;
        detached.client_visible_shadow        = detached.exported;
        detached.private_nondisplay_shadow    = false;
        detached.decode_shadow_private_active = false;
        detached.decode_shadow_generation     = detached.content_generation;
        mark_export_fd_detached(&detached);
        trace_export_fd_lifetime(resource, &detached, "retained-detach", detached.content_generation, false);
        if (!retainable) {
            lock.unlock();
            destroy_export_resource(runtime, &detached);
            return;
        }

        try {
            for (size_t i = 0; i < runtime->retained_exports.size();) {
                if (detached_export_same_owner(runtime->retained_exports[i], detached.driver_instance_id, detached.owner_surface_id)) {
                    remove_detached_export_locked(runtime, i);
                    continue;
                }
                i++;
            }
            backing.fd                        = retained_export_fd_identity(detached);
            backing.state                     = RetainedExportState::Detached;
            backing.retained_sequence         = runtime->retained_export_sequence + 1;
            const VkDeviceSize detached_bytes = detached.allocation_size;
            runtime->retained_exports.push_back(std::move(backing));
            runtime->retained_export_sequence++;
            RetainedExportBacking& retained = runtime->retained_exports.back();
            runtime->retained_export_memory_bytes += detached_bytes;
            VKVV_TRACE("retained-export-add",
                       "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx bytes=%llu fd_stat=%u fd_dev=%llu fd_ino=%llu retained=%zu retained_mem=%llu seq=%llu",
                       retained.resource.owner_surface_id, static_cast<unsigned long long>(retained.resource.driver_instance_id),
                       static_cast<unsigned long long>(retained.resource.stream_id), retained.resource.codec_operation, vkvv_trace_handle(retained.resource.memory),
                       static_cast<unsigned long long>(detached_bytes), retained.fd.valid ? 1U : 0U, static_cast<unsigned long long>(retained.fd.dev),
                       static_cast<unsigned long long>(retained.fd.ino), runtime->retained_exports.size(), static_cast<unsigned long long>(runtime->retained_export_memory_bytes),
                       static_cast<unsigned long long>(retained.retained_sequence));
            VKVV_TRACE("retained-present-shadow-attach",
                       "owner=%u driver=%llu stream=%llu codec=0x%x mem=0x%llx content_gen=%llu present_gen=%llu client_visible=1 private_only=0 retained=%zu seq=%llu",
                       retained.resource.owner_surface_id, static_cast<unsigned long long>(retained.resource.driver_instance_id),
                       static_cast<unsigned long long>(retained.resource.stream_id), retained.resource.codec_operation, vkvv_trace_handle(retained.resource.memory),
                       static_cast<unsigned long long>(retained.resource.content_generation), static_cast<unsigned long long>(retained.resource.present_generation),
                       runtime->retained_exports.size(), static_cast<unsigned long long>(retained.retained_sequence));
            refresh_transition_retention_window_locked(runtime, &retained.resource, "detach");
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
        resource->layout                                 = VK_IMAGE_LAYOUT_UNDEFINED;
        resource->last_nondisplay_skip_generation        = 0;
        resource->last_nondisplay_skip_shadow_generation = 0;
        resource->last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
        resource->last_display_refresh_generation        = 0;
        destroy_export_resource(runtime, &resource->private_decode_shadow);
        clear_surface_export_attach_state(resource);
        clear_surface_direct_import_present_state(resource);
        clear_surface_av1_visible_output_trace(resource);
    }

    void destroy_surface_resource_raw(VulkanRuntime* runtime, SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }

        trace_export_fd_lifetime(resource, &resource->export_resource, "surface-destroy", resource->content_generation,
                                 export_resource_fd_may_be_sampled_by_client(&resource->export_resource));
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
                destroy_export_resource(runtime, &existing->private_decode_shadow);
                clear_surface_direct_import_present_state(existing);
                clear_surface_av1_visible_output_trace(existing);
            }
            existing->driver_instance_id = surface->driver_instance_id;
            existing->stream_id          = stream_id;
            existing->codec_operation    = codec_operation;
            existing->surface_id         = surface->id;
            existing->visible_extent     = {surface->width, surface->height};
            existing->import             = surface->import;
            clear_surface_direct_import_present_state(existing);
            clear_surface_av1_visible_output_trace(existing);
            VKVV_TRACE("surface-resource-reuse",
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
        clear_surface_direct_import_present_state(resource);
        clear_surface_av1_visible_output_trace(resource);
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
        VKVV_TRACE("surface-resource-create",
                   "surface=%u driver=%llu stream=%llu surface_codec=0x%x key_codec=0x%x resource_codec=0x%x extent=%ux%u exportable=%u decode_mem=%llu shadow_mem=0x%llx "
                   "imported=%u import_fd_stat=%u import_fd_dev=%llu import_fd_ino=%llu",
                   surface->id, static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), surface->codec_operation,
                   key.codec_operation, resource->codec_operation, extent.width, extent.height, resource->exportable ? 1U : 0U,
                   static_cast<unsigned long long>(resource->allocation_size), vkvv_trace_handle(resource->export_resource.memory), resource->import.external ? 1U : 0U,
                   resource->import.fd.valid ? 1U : 0U, static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino));
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
        barrier.dstStageMask                    = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
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
