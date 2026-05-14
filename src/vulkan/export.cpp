#include "vulkan/export/internal.h"
#include "va/surface_import.h"
#include "telemetry.h"

#include <algorithm>
#include <cstdio>
#include <unistd.h>

using namespace vkvv;

bool vkvv_vulkan_surface_has_exported_backing(const VkvvSurface* surface) {
    if (surface == nullptr || surface->vulkan == nullptr) {
        return false;
    }
    const auto* resource = static_cast<const SurfaceResource*>(surface->vulkan);
    return resource->exported;
}

bool vkvv_vulkan_surface_has_predecode_export(const VkvvSurface* surface) {
    if (surface == nullptr || surface->vulkan == nullptr) {
        return false;
    }
    const auto* resource = static_cast<const SurfaceResource*>(surface->vulkan);
    return resource->export_resource.image != VK_NULL_HANDLE && resource->export_resource.memory != VK_NULL_HANDLE && resource->export_resource.predecode_exported;
}

VAStatus vkvv_vulkan_prepare_surface_export(void* runtime_ptr, VkvvSurface* surface, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export preparation")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!runtime->surface_export || runtime->get_memory_fd == nullptr) {
        std::snprintf(reason, reason_size, "Vulkan runtime does not support dma-buf surface export");
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (surface == nullptr) {
        std::snprintf(reason, reason_size, "missing surface for export preparation");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    const ExportFormatInfo* format = export_format_for_surface(surface, nullptr, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    char     drain_reason[512] = {};
    VAStatus drain_status      = drain_pending_surface_work_before_sync_command(runtime, surface, drain_reason, sizeof(drain_reason));
    if (drain_status != VA_STATUS_SUCCESS) {
        std::snprintf(reason, reason_size, "%s", drain_reason);
        return drain_status;
    }

    VkExtent2D extent{
        round_up_16(std::max(1u, surface->width)),
        round_up_16(std::max(1u, surface->height)),
    };
    if (!ensure_export_only_surface_resource(surface, format, extent, reason, reason_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    if (!resource->exportable && !ensure_export_resource(runtime, resource, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    const RetainedExportStats retained_stats = runtime_retained_export_stats(runtime);
    VKVV_TRACE("export-prepare",
               "surface=%u driver=%llu stream=%llu codec=0x%x decoded=%u exportable=%u shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu "
               "retained=%zu retained_mem=%llu retained_accounted=%llu retained_ok=%u retained_limit=%zu retained_budget=%llu transition=%u transition_retained=%zu "
               "transition_mem=%llu transition_target=%zu transition_budget=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               surface->decoded ? 1U : 0U, resource->exportable ? 1U : 0U, vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation), retained_stats.count, static_cast<unsigned long long>(retained_stats.bytes),
               static_cast<unsigned long long>(retained_stats.accounted_bytes), retained_stats.accounting_valid ? 1U : 0U, retained_stats.count_limit,
               static_cast<unsigned long long>(retained_stats.memory_budget), retained_stats.transition_active ? 1U : 0U, retained_stats.transition_retained_count,
               static_cast<unsigned long long>(retained_stats.transition_retained_bytes), retained_stats.transition_target_count,
               static_cast<unsigned long long>(retained_stats.transition_target_bytes));

    if (drain_reason[0] != '\0') {
        std::snprintf(reason, reason_size,
                      "surface export resource ready: driver=%llu surface=%u stream=%llu codec=0x%x format=%s visible=%ux%u coded=%ux%u vk_format=%d va_fourcc=0x%x exportable=%u "
                      "shadow=%u decode_mem=%llu export_mem=%llu retained=%zu retained_mem=%llu retained_accounted=%llu retained_ok=%u retained_limit=%zu retained_budget=%llu "
                      "drained=\"%s\"",
                      static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                      format->name, surface->width, surface->height, resource->coded_extent.width, resource->coded_extent.height, resource->format, resource->va_fourcc,
                      resource->exportable, resource->export_resource.image != VK_NULL_HANDLE, static_cast<unsigned long long>(resource->allocation_size),
                      static_cast<unsigned long long>(export_memory_bytes(resource)), retained_stats.count, static_cast<unsigned long long>(retained_stats.bytes),
                      static_cast<unsigned long long>(retained_stats.accounted_bytes), retained_stats.accounting_valid ? 1U : 0U, retained_stats.count_limit,
                      static_cast<unsigned long long>(retained_stats.memory_budget), drain_reason);
    } else {
        std::snprintf(reason, reason_size,
                      "surface export resource ready: driver=%llu surface=%u stream=%llu codec=0x%x format=%s visible=%ux%u coded=%ux%u vk_format=%d va_fourcc=0x%x exportable=%u "
                      "shadow=%u decode_mem=%llu export_mem=%llu retained=%zu retained_mem=%llu retained_accounted=%llu retained_ok=%u retained_limit=%zu retained_budget=%llu",
                      static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                      format->name, surface->width, surface->height, resource->coded_extent.width, resource->coded_extent.height, resource->format, resource->va_fourcc,
                      resource->exportable, resource->export_resource.image != VK_NULL_HANDLE, static_cast<unsigned long long>(resource->allocation_size),
                      static_cast<unsigned long long>(export_memory_bytes(resource)), retained_stats.count, static_cast<unsigned long long>(retained_stats.bytes),
                      static_cast<unsigned long long>(retained_stats.accounted_bytes), retained_stats.accounting_valid ? 1U : 0U, retained_stats.count_limit,
                      static_cast<unsigned long long>(retained_stats.memory_budget));
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_refresh_surface_export(void* runtime_ptr, VkvvSurface* surface, bool refresh_export, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (runtime == nullptr || surface == nullptr || surface->vulkan == nullptr) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export refresh")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!surface->decoded) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    if (resource->export_resource.image == VK_NULL_HANDLE && resource->import.external && resource->import.fd.valid) {
        (void)attach_imported_export_resource_by_fd(runtime, resource);
    }
    const bool requires_published_visible_output = surface_resource_requires_visible_publication(resource, refresh_export);
    if (resource->export_resource.image == VK_NULL_HANDLE && requires_published_visible_output) {
        if (!ensure_export_resource(runtime, resource, reason, reason_size)) {
            VKVV_TRACE("visible-refresh-shadow-create-failed",
                       "surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u content_gen=%llu import_external=%u exported=%u shadow_exported=%u direct_import_ok=%u "
                       "retained=%zu retained_mem=%llu reason=\"%s\"",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       refresh_export ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation), resource->import.external ? 1U : 0U, resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, surface_resource_has_direct_import_output(resource) ? 1U : 0U, runtime_retained_export_count(runtime),
                       static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), reason != nullptr ? reason : "");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    if (resource->export_resource.image == VK_NULL_HANDLE) {
        if (refresh_export && resource->content_generation != 0) {
            resource->export_seed_generation = resource->content_generation;
            remember_export_seed_resource(runtime, resource);
        } else {
            resource->export_seed_generation                 = 0;
            resource->last_nondisplay_skip_generation        = resource->content_generation;
            resource->last_nondisplay_skip_shadow_generation = 0;
            resource->last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
            unregister_export_seed_resource(runtime, resource);
        }
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        VKVV_TRACE("export-refresh-no-backing",
                   "surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u content_gen=%llu seed_gen=%llu last_skip_gen=%llu last_display_gen=%llu retained=%zu "
                   "retained_mem=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   refresh_export ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_seed_generation),
                   static_cast<unsigned long long>(resource->last_nondisplay_skip_generation), static_cast<unsigned long long>(resource->last_display_refresh_generation),
                   runtime_retained_export_count(runtime), static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
        return VA_STATUS_SUCCESS;
    }

    const ExportFormatInfo* format = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    VAStatus drain_status = drain_pending_surface_work_before_sync_command(runtime, surface, reason, reason_size);
    if (drain_status != VA_STATUS_SUCCESS) {
        return drain_status;
    }
    VKVV_TRACE("export-refresh-pre",
               "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu", surface->id,
               static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation));

    if (!refresh_export) {
        resource->export_seed_generation = 0;
        unregister_export_seed_resource(runtime, resource);
        if (av1_non_display_export_refresh(resource, refresh_export)) {
            const bool retired_predecode_export = resource->export_resource.predecode_exported || resource->export_resource.predecode_seeded ||
                resource->export_resource.seed_source_surface_id != VA_INVALID_ID || resource->export_resource.seed_source_generation != 0;
            if (retired_predecode_export) {
                unregister_predecode_export_resource(runtime, &resource->export_resource);
                VKVV_TRACE("export-predecode-retire-av1-nondisplay",
                           "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu seeded=%u seed_surface=%u seed_gen=%llu", surface->id,
                           static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                           static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                           static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_seeded ? 1U : 0U,
                           resource->export_resource.seed_source_surface_id, static_cast<unsigned long long>(resource->export_resource.seed_source_generation));
                clear_predecode_export_state(&resource->export_resource);
            }
            resource->last_nondisplay_skip_generation        = resource->content_generation;
            resource->last_nondisplay_skip_shadow_generation = resource->export_resource.content_generation;
            resource->last_nondisplay_skip_shadow_memory     = resource->export_resource.memory;
            const bool skipped_shadow_stale                  = resource->content_generation != 0 && resource->export_resource.content_generation != resource->content_generation;
            VKVV_TRACE("export-refresh-skip-av1-nondisplay",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu seed_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_stale=%u exported=%u "
                       "shadow_exported=%u predecode=%u seeded=%u retired_predecode=%u last_display_gen=%llu",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_seed_generation),
                       vkvv_trace_handle(resource->export_resource.memory), static_cast<unsigned long long>(resource->export_resource.content_generation),
                       skipped_shadow_stale ? 1U : 0U, resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                       resource->export_resource.predecode_exported ? 1U : 0U, resource->export_resource.predecode_seeded ? 1U : 0U, retired_predecode_export ? 1U : 0U,
                       static_cast<unsigned long long>(resource->last_display_refresh_generation));
            std::snprintf(
                reason, reason_size,
                "preserved exported %s shadow image after AV1 non-display decode without predecode seeding: driver=%llu surface=%u stream=%llu codec=0x%x export_mem=%llu "
                "retained=%zu retained_mem=%llu source_generation=%llu shadow_generation=%llu",
                format->name, static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id),
                resource->codec_operation, static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
                static_cast<unsigned long long>(resource->export_resource.content_generation));
            return VA_STATUS_SUCCESS;
        }
        if (resource->export_resource.content_generation == 0) {
            (void)seed_predecode_export_from_last_good(runtime, resource, reason, reason_size);
        }
        const bool retired_predecode_export = resource->export_resource.predecode_exported || resource->export_resource.predecode_seeded ||
            resource->export_resource.seed_source_surface_id != VA_INVALID_ID || resource->export_resource.seed_source_generation != 0;
        if (retired_predecode_export) {
            unregister_predecode_export_resource(runtime, &resource->export_resource);
            VKVV_TRACE("export-predecode-retire-nondisplay",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu seeded=%u seed_surface=%u seed_gen=%llu", surface->id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_seeded ? 1U : 0U,
                       resource->export_resource.seed_source_surface_id, static_cast<unsigned long long>(resource->export_resource.seed_source_generation));
            resource->export_resource.predecode_exported     = false;
            resource->export_resource.predecode_seeded       = false;
            resource->export_resource.black_placeholder      = false;
            resource->export_resource.seed_source_surface_id = VA_INVALID_ID;
            resource->export_resource.seed_source_generation = 0;
        }
        resource->last_nondisplay_skip_generation        = resource->content_generation;
        resource->last_nondisplay_skip_shadow_generation = resource->export_resource.content_generation;
        resource->last_nondisplay_skip_shadow_memory     = resource->export_resource.memory;
        const bool skipped_shadow_stale                  = resource->content_generation != 0 && resource->export_resource.content_generation != resource->content_generation;
        bool       seeded_shadow                         = false;
        if (skipped_shadow_stale && resource->export_resource.exported && resource->last_display_refresh_generation != 0) {
            const uint64_t old_shadow_generation = resource->export_resource.content_generation;
            (void)seed_predecode_export_from_last_good(runtime, resource, reason, reason_size);
            seeded_shadow = resource->export_resource.predecode_seeded && resource->export_resource.seed_source_surface_id != VA_INVALID_ID;
            if (seeded_shadow) {
                VKVV_TRACE(
                    "export-nondisplay-shadow-seed",
                    "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx old_shadow_gen=%llu seed_surface=%u seed_gen=%llu last_display_gen=%llu",
                    surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                    static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                    static_cast<unsigned long long>(old_shadow_generation), resource->export_resource.seed_source_surface_id,
                    static_cast<unsigned long long>(resource->export_resource.seed_source_generation), static_cast<unsigned long long>(resource->last_display_refresh_generation));
            }
        }
        if (skipped_shadow_stale && !seeded_shadow && resource->export_resource.exported && resource->last_display_refresh_generation != 0) {
            VKVV_TRACE("export-stale-visible-nondisplay",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu last_display_gen=%llu exported=%u imported=%u fd_stat=%u "
                       "fd_dev=%llu fd_ino=%llu retained=%zu retained_mem=%llu",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), static_cast<unsigned long long>(resource->last_display_refresh_generation),
                       resource->exported ? 1U : 0U, resource->import.external ? 1U : 0U, resource->export_resource.fd_stat_valid ? 1U : 0U,
                       static_cast<unsigned long long>(resource->export_resource.fd_dev), static_cast<unsigned long long>(resource->export_resource.fd_ino),
                       runtime_retained_export_count(runtime), static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
        }
        VKVV_TRACE("export-refresh-skip-nondisplay",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu seed_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_stale=%u exported=%u shadow_exported=%u "
                   "predecode=%u seeded=%u retired_predecode=%u fd_stat=%u fd_dev=%llu fd_ino=%llu last_display_gen=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_seed_generation),
                   vkvv_trace_handle(resource->export_resource.memory), static_cast<unsigned long long>(resource->export_resource.content_generation),
                   skipped_shadow_stale ? 1U : 0U, resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                   resource->export_resource.predecode_exported ? 1U : 0U, resource->export_resource.predecode_seeded ? 1U : 0U, retired_predecode_export ? 1U : 0U,
                   resource->export_resource.fd_stat_valid ? 1U : 0U, static_cast<unsigned long long>(resource->export_resource.fd_dev),
                   static_cast<unsigned long long>(resource->export_resource.fd_ino), static_cast<unsigned long long>(resource->last_display_refresh_generation));
        std::snprintf(reason, reason_size,
                      "preserved exported %s shadow image after non-display decode: driver=%llu surface=%u stream=%llu codec=0x%x export_mem=%llu retained=%zu retained_mem=%llu "
                      "source_generation=%llu shadow_generation=%llu",
                      format->name, static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id),
                      resource->codec_operation, static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                      static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
                      static_cast<unsigned long long>(resource->export_resource.content_generation));
        return VA_STATUS_SUCCESS;
    }

    uint32_t seeded_predecode_exports = 0;
    if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (requires_published_visible_output && !surface_resource_has_published_visible_output(resource)) {
        VKVV_TRACE("visible-refresh-unpublished",
                   "surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_ok=%u import_external=%u "
                   "import_present_generation=%llu direct_import_ok=%u exported=%u shadow_exported=%u retained=%zu retained_mem=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   refresh_export ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                   static_cast<unsigned long long>(resource->export_resource.content_generation), surface_resource_has_current_export_shadow(resource) ? 1U : 0U,
                   resource->import.external ? 1U : 0U, static_cast<unsigned long long>(resource->import_present_generation),
                   surface_resource_has_direct_import_output(resource) ? 1U : 0U, resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                   runtime_retained_export_count(runtime), static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_OPERATION_FAILED,
                          "visible AV1 refresh did not publish decoded output: driver=%llu surface=%u stream=%llu codec=0x%x source_generation=%llu shadow_generation=%llu",
                          static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id),
                          resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                          static_cast<unsigned long long>(resource->export_resource.content_generation));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    resource->last_display_refresh_generation = resource->content_generation;
    VKVV_TRACE("export-refresh-post",
               "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu "
               "seeded_targets=%u last_skip_gen=%llu last_skip_shadow_gen=%llu retained=%zu retained_mem=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation), seeded_predecode_exports,
               static_cast<unsigned long long>(resource->last_nondisplay_skip_generation), static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation),
               runtime_retained_export_count(runtime), static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)));
    std::snprintf(reason, reason_size,
                  "refreshed exported %s shadow image after decode: driver=%llu surface=%u stream=%llu codec=0x%x export_mem=%llu retained=%zu retained_mem=%llu "
                  "source_generation=%llu shadow_generation=%llu seeded_predecode=%u",
                  format->name, static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id),
                  resource->codec_operation, static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                  static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
                  static_cast<unsigned long long>(resource->export_resource.content_generation), seeded_predecode_exports);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvv_vulkan_export_surface(void* runtime_ptr, const VkvvSurface* surface, uint32_t flags, VADRMPRIMESurfaceDescriptor* descriptor, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (!ensure_runtime_usable(runtime, reason, reason_size, "surface export")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!runtime->surface_export || runtime->get_memory_fd == nullptr) {
        std::snprintf(reason, reason_size, "Vulkan runtime does not support dma-buf surface export");
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (surface == nullptr || surface->vulkan == nullptr) {
        std::snprintf(reason, reason_size, "surface has no Vulkan image to export");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    const ExportFormatInfo* format = export_format_for_surface(surface, nullptr, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    const VAStatus flag_status = validate_export_flags(flags, reason, reason_size);
    if (flag_status != VA_STATUS_SUCCESS) {
        return flag_status;
    }
    if (descriptor == nullptr) {
        std::snprintf(reason, reason_size, "surface export descriptor is null");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    format         = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    VAStatus drain_status = drain_pending_surface_work_before_sync_command(runtime, const_cast<VkvvSurface*>(surface), reason, reason_size);
    if (drain_status != VA_STATUS_SUCCESS) {
        return drain_status;
    }
    const bool     shadow_exists_before         = resource->export_resource.image != VK_NULL_HANDLE && resource->export_resource.memory != VK_NULL_HANDLE;
    const uint64_t shadow_generation_before     = resource->export_resource.content_generation;
    const bool     export_after_nondisplay_skip = resource->last_nondisplay_skip_generation != 0 && resource->last_nondisplay_skip_generation == resource->content_generation;
    const bool     shadow_stale_before          = shadow_exists_before && resource->content_generation != 0 && shadow_generation_before != resource->content_generation;
    const bool     skip_shadow_was_stale =
        resource->last_nondisplay_skip_generation != 0 && resource->last_nondisplay_skip_shadow_generation != resource->last_nondisplay_skip_generation;
    VKVV_TRACE("export-request-state",
               "surface=%u driver=%llu stream=%llu codec=0x%x decoded=%u exportable=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_stale=%u exported=%u "
               "shadow_exported=%u predecode=%u after_skip=%u skip_gen=%llu skip_shadow_gen=%llu skip_shadow_mem=0x%llx skip_shadow_stale=%u last_display_gen=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               surface->decoded ? 1U : 0U, resource->exportable ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation),
               vkvv_trace_handle(resource->export_resource.memory), static_cast<unsigned long long>(shadow_generation_before), shadow_stale_before ? 1U : 0U,
               resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, resource->export_resource.predecode_exported ? 1U : 0U,
               export_after_nondisplay_skip ? 1U : 0U, static_cast<unsigned long long>(resource->last_nondisplay_skip_generation),
               static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation), vkvv_trace_handle(resource->last_nondisplay_skip_shadow_memory),
               skip_shadow_was_stale ? 1U : 0U, static_cast<unsigned long long>(resource->last_display_refresh_generation));
    VkDeviceMemory             export_memory          = resource->memory;
    VkDeviceSize               export_allocation_size = resource->allocation_size;
    const VkSubresourceLayout* export_plane_layouts   = resource->plane_layouts;
    uint32_t                   export_plane_count     = resource->plane_count;
    uint64_t                   export_modifier        = resource->drm_format_modifier;
    bool                       export_has_modifier    = resource->has_drm_format_modifier;
    bool                       copied_to_shadow       = false;
    ExportResource*            exported_shadow        = nullptr;

    if (!resource->exportable) {
        if (surface->decoded) {
            const bool force_visible_copy       = av1_visible_export_requires_copy(resource);
            const bool shadow_current           = resource->content_generation != 0 && resource->export_resource.content_generation == resource->content_generation;
            uint32_t   seeded_predecode_exports = 0;
            if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            copied_to_shadow = !shadow_current || force_visible_copy;
            VKVV_TRACE("export-late-refresh",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_before=%llu shadow_after=%llu copied=%u forced=%u after_skip=%u skip_gen=%llu "
                       "skip_shadow_gen=%llu skip_shadow_stale=%u seeded_targets=%u",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(shadow_generation_before),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), copied_to_shadow ? 1U : 0U, force_visible_copy ? 1U : 0U,
                       export_after_nondisplay_skip ? 1U : 0U, static_cast<unsigned long long>(resource->last_nondisplay_skip_generation),
                       static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation), skip_shadow_was_stale ? 1U : 0U, seeded_predecode_exports);
        } else if (!ensure_export_resource(runtime, resource, reason, reason_size)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        } else if (!seed_predecode_export_from_last_good(runtime, resource, reason, reason_size)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ExportResource* shadow = &resource->export_resource;
        export_memory          = shadow->memory;
        export_allocation_size = shadow->allocation_size;
        export_plane_layouts   = shadow->plane_layouts;
        export_plane_count     = shadow->plane_count;
        export_modifier        = shadow->drm_format_modifier;
        export_has_modifier    = shadow->has_drm_format_modifier;
        exported_shadow        = shadow;
    }
    if (export_after_nondisplay_skip) {
        const bool shadow_stale_after = exported_shadow != nullptr && resource->content_generation != 0 && exported_shadow->content_generation != resource->content_generation;
        VKVV_TRACE("export-after-nondisplay-skip",
                   "surface=%u driver=%llu stream=%llu codec=0x%x decoded=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_stale=%u copied=%u "
                   "skip_shadow_gen=%llu skip_shadow_mem=0x%llx skip_shadow_stale=%u last_display_gen=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   surface->decoded ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation),
                   vkvv_trace_handle(exported_shadow != nullptr ? exported_shadow->memory : resource->memory),
                   static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation), shadow_stale_after ? 1U : 0U,
                   copied_to_shadow ? 1U : 0U, static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation),
                   vkvv_trace_handle(resource->last_nondisplay_skip_shadow_memory), skip_shadow_was_stale ? 1U : 0U,
                   static_cast<unsigned long long>(resource->last_display_refresh_generation));
    }
    VKVV_TRACE("export-before-fd",
               "surface=%u driver=%llu stream=%llu codec=0x%x decoded=%u exportable=%u export_mem=0x%llx content_gen=%llu shadow_gen=%llu predecode=%u copied=%u after_skip=%u "
               "skip_gen=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               surface->decoded ? 1U : 0U, resource->exportable ? 1U : 0U, vkvv_trace_handle(export_memory), static_cast<unsigned long long>(resource->content_generation),
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
               exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, copied_to_shadow ? 1U : 0U, export_after_nondisplay_skip ? 1U : 0U,
               static_cast<unsigned long long>(resource->last_nondisplay_skip_generation));

    if (export_memory == VK_NULL_HANDLE) {
        std::snprintf(reason, reason_size, "Vulkan surface image has no exportable memory layout");
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory     = export_memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    int      fd     = -1;
    VkResult result = runtime->get_memory_fd(runtime->device, &fd_info, &fd);
    if (result != VK_SUCCESS || fd < 0) {
        if (fd >= 0) {
            close(fd);
        }
        record_vk_result(runtime, result, "vkGetMemoryFdKHR", "surface export", reason, reason_size);
        if (result == VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkGetMemoryFdKHR for surface export returned invalid fd=%d", fd);
        }
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VAStatus descriptor_status = fill_drm_prime_descriptor(surface, format, export_allocation_size, export_plane_layouts, export_plane_count, export_modifier,
                                                                 export_has_modifier, fd, descriptor, reason, reason_size);
    if (descriptor_status != VA_STATUS_SUCCESS) {
        close(fd);
        return descriptor_status;
    }

    resource->exported = true;
    if (exported_shadow != nullptr) {
        exported_shadow->exported = true;
        if (!surface->decoded) {
            exported_shadow->predecode_exported = true;
            register_predecode_export_resource(runtime, exported_shadow);
            VKVV_TRACE("export-predecode-register", "surface=%u driver=%llu stream=%llu codec=0x%x shadow_mem=0x%llx shadow_gen=%llu seeded=%u placeholder=%u", surface->id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       vkvv_trace_handle(exported_shadow->memory), static_cast<unsigned long long>(exported_shadow->content_generation),
                       exported_shadow->predecode_seeded ? 1U : 0U, exported_shadow->black_placeholder ? 1U : 0U);
        }
    }
    const VkvvFdIdentity fd_stat = vkvv_fd_identity_from_fd(fd);
    if (exported_shadow != nullptr) {
        exported_shadow->fd_stat_valid = fd_stat.valid;
        exported_shadow->fd_dev        = fd_stat.dev;
        exported_shadow->fd_ino        = fd_stat.ino;
    }
    VKVV_TRACE("export-fd",
               "surface=%u driver=%llu stream=%llu codec=0x%x fd=%d fd_stat=%u fd_dev=%llu fd_ino=%llu export_mem=0x%llx content_gen=%llu shadow_gen=%llu predecode=%u seeded=%u "
               "placeholder=%u seed_surface=%u seed_gen=%llu after_skip=%u skip_gen=%llu skip_shadow_gen=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, fd,
               fd_stat.valid ? 1U : 0U, static_cast<unsigned long long>(fd_stat.dev), static_cast<unsigned long long>(fd_stat.ino), vkvv_trace_handle(export_memory),
               static_cast<unsigned long long>(resource->content_generation),
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
               exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U,
               exported_shadow != nullptr && exported_shadow->black_placeholder ? 1U : 0U, exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->seed_source_generation : 0), export_after_nondisplay_skip ? 1U : 0U,
               static_cast<unsigned long long>(resource->last_nondisplay_skip_generation), static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation));

    VKVV_SUCCESS_REASON(
        reason, reason_size,
        "exported %s dma-buf%s: driver=%llu surface=%u stream=%llu codec=0x%x %ux%u fd=%d size=%u modifier=0x%llx y_pitch=%u uv_pitch=%u decode_mem=%llu export_mem=%llu "
        "retained=%zu retained_mem=%llu source_generation=%llu shadow_generation=%llu predecode=%u seeded=%u placeholder=%s seed_source=%u seed_generation=%llu",
        format->name, copied_to_shadow ? " via shadow copy" : "", static_cast<unsigned long long>(resource->driver_instance_id), surface->id,
        static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, surface->width, surface->height, fd, descriptor->objects[0].size,
        static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier), descriptor->layers[0].pitch[0], descriptor->layers[1].pitch[0],
        static_cast<unsigned long long>(resource->allocation_size), static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
        static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
        static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
        exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U,
        exported_shadow != nullptr && exported_shadow->black_placeholder ? "black" : "none", exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
        static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->seed_source_generation : 0));
    return VA_STATUS_SUCCESS;
}

void vkvv_vulkan_note_surface_created(void* runtime_ptr, const VkvvSurface* surface) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    if (runtime == nullptr || surface == nullptr) {
        return;
    }

    char                    reason[128] = {};
    const ExportFormatInfo* format      = export_format_for_surface(surface, nullptr, reason, sizeof(reason));
    const VkExtent2D        coded_extent{
        round_up_16(std::max(1u, surface->width)),
        round_up_16(std::max(1u, surface->height)),
    };
    prune_detached_exports_for_surface(runtime, surface->driver_instance_id, surface->id, surface->stream_id, static_cast<VkVideoCodecOperationFlagsKHR>(surface->codec_operation),
                                       surface->fourcc, format != nullptr ? format->vk_format : VK_FORMAT_UNDEFINED, coded_extent);
}

void vkvv_vulkan_prune_driver_exports(void* runtime_ptr, uint64_t driver_instance_id) {
    prune_detached_exports_for_driver(static_cast<VulkanRuntime*>(runtime_ptr), driver_instance_id);
}
