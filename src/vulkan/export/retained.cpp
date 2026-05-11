#include "vulkan/runtime_internal.h"
#include "va/surface_import.h"

#include <algorithm>
#include <mutex>

namespace vkvv {

    namespace {

        constexpr VkDeviceSize mib                  = 1024ull * 1024ull;
        constexpr VkDeviceSize min_global_cap_bytes = 128ull * mib;
        constexpr VkDeviceSize max_global_cap_bytes = 512ull * mib;

        VkDeviceSize           retained_export_accounted_bytes_locked(const VulkanRuntime* runtime) {
            VkDeviceSize bytes = 0;
            for (const RetainedExportBacking& backing : runtime->retained_exports) {
                bytes += backing.resource.allocation_size;
            }
            return bytes;
        }

    } // namespace

    VkvvFdIdentity retained_export_fd_identity(const ExportResource& resource) {
        VkvvFdIdentity identity{};
        identity.valid = resource.fd_stat_valid;
        identity.dev   = resource.fd_dev;
        identity.ino   = resource.fd_ino;
        return identity;
    }

    VkvvExternalImageIdentity retained_export_image_identity(const ExportResource& resource) {
        VkvvExternalImageIdentity identity{};
        identity.fd                      = retained_export_fd_identity(resource);
        identity.fourcc                  = resource.va_fourcc;
        identity.width                   = resource.extent.width;
        identity.height                  = resource.extent.height;
        identity.has_drm_format_modifier = resource.has_drm_format_modifier;
        identity.drm_format_modifier     = resource.drm_format_modifier;
        return identity;
    }

    RetainedExportMatch retained_export_match_import(const RetainedExportBacking& backing, const VkvvExternalSurfaceImport& import, unsigned int va_fourcc, VkFormat format,
                                                     VkExtent2D coded_extent) {
        if (!import.external) {
            return RetainedExportMatch::MissingImport;
        }
        const ExportResource&           resource     = backing.resource;
        const VkvvExternalImageIdentity import_key   = vkvv_external_image_identity_from_import(import);
        const VkvvExternalImageIdentity retained_key = retained_export_image_identity(resource);

        if (!import_key.fd.valid || !retained_key.fd.valid) {
            return RetainedExportMatch::MissingFd;
        }
        if (!vkvv_fd_identity_equal(retained_key.fd, import_key.fd)) {
            return RetainedExportMatch::FdMismatch;
        }

        if ((import_key.fourcc != 0 && import_key.fourcc != va_fourcc) || retained_key.fourcc != va_fourcc) {
            return RetainedExportMatch::FourccMismatch;
        }
        if (retained_key.has_drm_format_modifier != import_key.has_drm_format_modifier ||
            (retained_key.has_drm_format_modifier && retained_key.drm_format_modifier != import_key.drm_format_modifier)) {
            return RetainedExportMatch::ModifierMismatch;
        }
        if (resource.format != format) {
            return RetainedExportMatch::FormatMismatch;
        }
        if (retained_key.width < coded_extent.width || retained_key.height < coded_extent.height) {
            return RetainedExportMatch::ExtentMismatch;
        }
        return RetainedExportMatch::Match;
    }

    const char* retained_export_match_reason(RetainedExportMatch match) {
        switch (match) {
            case RetainedExportMatch::Match: return "match";
            case RetainedExportMatch::MissingImport: return "missing-import";
            case RetainedExportMatch::MissingFd: return "missing-fd";
            case RetainedExportMatch::FdMismatch: return "fd-mismatch";
            case RetainedExportMatch::FourccMismatch: return "fourcc-mismatch";
            case RetainedExportMatch::ModifierMismatch: return "modifier-mismatch";
            case RetainedExportMatch::FormatMismatch: return "format-mismatch";
            case RetainedExportMatch::ExtentMismatch: return "extent-mismatch";
        }
        return "unknown";
    }

    bool retained_export_matches_window(const ExportResource& resource, const TransitionRetentionWindow& window) {
        return window.active && resource.driver_instance_id == window.driver_instance_id && resource.stream_id == window.stream_id &&
            resource.codec_operation == window.codec_operation && resource.format == window.format && resource.va_fourcc == window.va_fourcc &&
            resource.extent.width == window.coded_extent.width && resource.extent.height == window.coded_extent.height;
    }

    bool retained_export_seed_can_replace_window(const TransitionRetentionWindow& window, const ExportResource& seed) {
        if (!window.active || retained_export_matches_window(seed, window)) {
            return true;
        }

        const bool active_window_tracks_decode = window.codec_operation != 0;
        const bool seed_tracks_decode          = seed.codec_operation != 0;
        return !active_window_tracks_decode || seed_tracks_decode;
    }

    VkDeviceSize retained_export_global_cap_bytes(const VkPhysicalDeviceMemoryProperties& properties) {
        VkDeviceSize largest_device_local_heap = 0;
        for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
            if ((properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
                continue;
            }
            largest_device_local_heap = std::max(largest_device_local_heap, properties.memoryHeaps[i].size);
        }
        if (largest_device_local_heap == 0) {
            return min_global_cap_bytes;
        }
        return std::clamp(largest_device_local_heap / 32, min_global_cap_bytes, max_global_cap_bytes);
    }

    RetainedExportBudget retained_export_budget_from_expected(size_t expected_count, VkDeviceSize expected_bytes, VkDeviceSize global_cap_bytes) {
        RetainedExportBudget budget{};
        budget.global_cap_bytes = global_cap_bytes == 0 ? min_global_cap_bytes : global_cap_bytes;
        if (expected_count == 0 || expected_bytes == 0) {
            return budget;
        }

        budget.average_bytes  = std::max<VkDeviceSize>(1, (expected_bytes / expected_count) + ((expected_bytes % expected_count) != 0 ? 1 : 0));
        budget.headroom_count = std::min<size_t>(4, std::max<size_t>(2, expected_count / 4));
        budget.target_count   = expected_count + budget.headroom_count;

        const VkDeviceSize headroom_bytes = budget.average_bytes * budget.headroom_count;
        const VkDeviceSize unclamped      = expected_bytes + headroom_bytes;
        const VkDeviceSize effective_cap  = std::max(budget.global_cap_bytes, expected_bytes);
        budget.target_bytes               = std::min(unclamped, effective_cap);
        return budget;
    }

    RetainedExportStats runtime_retained_export_stats(VulkanRuntime* runtime) {
        RetainedExportStats stats{};
        if (runtime == nullptr) {
            return stats;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        stats.count                     = runtime->retained_exports.size();
        stats.bytes                     = runtime->retained_export_memory_bytes;
        stats.accounted_bytes           = retained_export_accounted_bytes_locked(runtime);
        stats.accounting_valid          = stats.bytes == stats.accounted_bytes;
        stats.count_limit               = runtime->retained_export_count_limit;
        stats.memory_budget             = runtime->retained_export_memory_budget;
        stats.transition_active         = runtime->transition_retention.active;
        stats.transition_retained_count = runtime->transition_retention.retained_count;
        stats.transition_retained_bytes = runtime->transition_retention.retained_bytes;
        stats.transition_target_count   = runtime->transition_retention.budget.target_count;
        stats.transition_target_bytes   = runtime->transition_retention.budget.target_bytes;
        return stats;
    }

    VkDeviceSize runtime_retained_export_accounted_bytes(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        return retained_export_accounted_bytes_locked(runtime);
    }

    bool runtime_retained_export_memory_accounting_valid(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return true;
        }

        std::lock_guard<std::mutex> lock(runtime->export_mutex);
        return runtime->retained_export_memory_bytes == retained_export_accounted_bytes_locked(runtime);
    }

} // namespace vkvv
