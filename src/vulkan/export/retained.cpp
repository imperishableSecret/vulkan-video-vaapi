#include "vulkan/runtime_internal.h"

#include <algorithm>

namespace vkvv {

namespace {

constexpr VkDeviceSize mib = 1024ull * 1024ull;
constexpr VkDeviceSize min_global_cap_bytes = 128ull * mib;
constexpr VkDeviceSize max_global_cap_bytes = 512ull * mib;

} // namespace

VkvvFdIdentity retained_export_fd_identity(const ExportResource &resource) {
    VkvvFdIdentity identity{};
    identity.valid = resource.fd_stat_valid;
    identity.dev = resource.fd_dev;
    identity.ino = resource.fd_ino;
    return identity;
}

RetainedExportMatch retained_export_match_import(
        const RetainedExportBacking &backing,
        const VkvvExternalSurfaceImport &import,
        unsigned int va_fourcc,
        VkFormat format,
        VkExtent2D coded_extent) {
    if (!import.external) {
        return RetainedExportMatch::MissingImport;
    }
    if (!import.fd.valid || !backing.fd.valid) {
        return RetainedExportMatch::MissingFd;
    }
    if (backing.fd.dev != import.fd.dev || backing.fd.ino != import.fd.ino) {
        return RetainedExportMatch::FdMismatch;
    }

    const ExportResource &resource = backing.resource;
    if (resource.va_fourcc != va_fourcc) {
        return RetainedExportMatch::FourccMismatch;
    }
    if (resource.format != format) {
        return RetainedExportMatch::FormatMismatch;
    }
    if (resource.extent.width < coded_extent.width ||
        resource.extent.height < coded_extent.height) {
        return RetainedExportMatch::ExtentMismatch;
    }
    return RetainedExportMatch::Match;
}

const char *retained_export_match_reason(RetainedExportMatch match) {
    switch (match) {
    case RetainedExportMatch::Match:
        return "match";
    case RetainedExportMatch::MissingImport:
        return "missing-import";
    case RetainedExportMatch::MissingFd:
        return "missing-fd";
    case RetainedExportMatch::FdMismatch:
        return "fd-mismatch";
    case RetainedExportMatch::FourccMismatch:
        return "fourcc-mismatch";
    case RetainedExportMatch::FormatMismatch:
        return "format-mismatch";
    case RetainedExportMatch::ExtentMismatch:
        return "extent-mismatch";
    }
    return "unknown";
}

bool retained_export_matches_window(
        const ExportResource &resource,
        const TransitionRetentionWindow &window) {
    return window.active &&
           resource.driver_instance_id == window.driver_instance_id &&
           resource.stream_id == window.stream_id &&
           resource.codec_operation == window.codec_operation &&
           resource.format == window.format &&
           resource.va_fourcc == window.va_fourcc &&
           resource.extent.width == window.coded_extent.width &&
           resource.extent.height == window.coded_extent.height;
}

bool retained_export_seed_can_replace_window(
        const TransitionRetentionWindow &window,
        const ExportResource &seed) {
    if (!window.active || retained_export_matches_window(seed, window)) {
        return true;
    }

    const bool active_window_tracks_decode = window.codec_operation != 0;
    const bool seed_tracks_decode = seed.codec_operation != 0;
    return !active_window_tracks_decode || seed_tracks_decode;
}

VkDeviceSize retained_export_global_cap_bytes(const VkPhysicalDeviceMemoryProperties &properties) {
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

RetainedExportBudget retained_export_budget_from_expected(
        size_t expected_count,
        VkDeviceSize expected_bytes,
        VkDeviceSize global_cap_bytes) {
    RetainedExportBudget budget{};
    budget.global_cap_bytes = global_cap_bytes == 0 ? min_global_cap_bytes : global_cap_bytes;
    if (expected_count == 0 || expected_bytes == 0) {
        return budget;
    }

    budget.average_bytes = std::max<VkDeviceSize>(1, expected_bytes / expected_count);
    budget.headroom_count = std::min<size_t>(4, std::max<size_t>(2, expected_count / 4));
    budget.target_count = expected_count + budget.headroom_count;

    const VkDeviceSize headroom_bytes = budget.average_bytes * budget.headroom_count;
    const VkDeviceSize unclamped = expected_bytes + headroom_bytes;
    budget.target_bytes = std::min(unclamped, budget.global_cap_bytes);
    return budget;
}

} // namespace vkvv
