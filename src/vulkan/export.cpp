#include "vulkan/export/internal.h"
#include "va/surface_import.h"
#include "telemetry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace vkvv;

namespace {

    bool av1_env_flag_enabled(const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0;
    }

    bool av1_trace_publication_enabled() {
        return vkvv_trace_deep_enabled() || av1_env_flag_enabled("VKVV_AV1_TRACE_PUBLICATION");
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

    uint64_t av1_publish_metadata_fingerprint(const SurfaceResource* resource) {
        uint64_t hash = 1469598103934665603ULL;
        hash          = fnv1a64(hash, resource != nullptr ? resource->av1_frame_sequence : 0);
        hash          = fnv1a64(hash, resource != nullptr ? resource->surface_id : VA_INVALID_ID);
        hash          = fnv1a64(hash, resource != nullptr ? resource->content_generation : 0);
        hash          = fnv1a64(hash, resource != nullptr ? resource->av1_order_hint : 0);
        hash          = fnv1a64(hash, resource != nullptr ? vkvv_trace_handle(resource->image) : 0);
        hash          = fnv1a64(hash, resource != nullptr ? vkvv_trace_handle(resource->export_resource.image) : 0);
        hash          = fnv1a64(hash, resource != nullptr ? resource->import_present_generation : 0);
        return hash;
    }

    const char* av1_published_path(const SurfaceResource* resource, bool shadow_published, bool import_published) {
        if (shadow_published) {
            return "exported-shadow";
        }
        if (import_published) {
            return "imported-output";
        }
        if (resource != nullptr && resource->import.external) {
            return "imported-output";
        }
        return "none";
    }

    bool refresh_has_visible_display(const SurfaceResource* resource, bool refresh_export) {
        if (!refresh_export) {
            return false;
        }
        if (surface_resource_uses_av1_decode(resource) && resource->av1_visible_output_trace_valid) {
            return resource->av1_visible_show_frame || resource->av1_visible_show_existing_frame;
        }
        return true;
    }

    VkvvExportPresentSource visible_present_source(const SurfaceResource* resource) {
        if (surface_resource_uses_av1_decode(resource) && resource->av1_visible_output_trace_valid && resource->av1_visible_show_existing_frame) {
            return VkvvExportPresentSource::ShowExisting;
        }
        return VkvvExportPresentSource::VisibleRefresh;
    }

    void trace_av1_publication_fingerprint(VkvvSurface* surface, SurfaceResource* resource, bool refresh_export, bool shadow_published, bool import_published,
                                           bool published) {
        if (surface == nullptr || resource == nullptr || !surface_resource_uses_av1_decode(resource) || !refresh_export || !av1_trace_publication_enabled()) {
            return;
        }

        const uint32_t fingerprint_level = av1_fingerprint_level();
        const bool     crc_valid         = fingerprint_level > 0;
        const uint64_t published_crc     = crc_valid ? av1_publish_metadata_fingerprint(resource) : 0;
        const bool     matches_decode    = crc_valid && resource->av1_decode_fingerprint != 0 && published_crc == resource->av1_decode_fingerprint;
        const bool     matches_previous  = crc_valid && resource->av1_previous_visible_fingerprint != 0 && published_crc == resource->av1_previous_visible_fingerprint;
        const char*    published_path    = av1_published_path(resource, shadow_published, import_published);
        resource->av1_publish_fingerprint = published_crc;

        VKVV_TRACE("av1-publish-fingerprint",
                   "frame_seq=%llu surface=%u content_generation=%llu published_path=%s shadow_image_handle=0x%llx import_image_handle=0x0 exported=%u shadow_exported=%u "
                   "import_external=%u import_present_generation=%llu published_crc_valid=%u published_y_tl_crc=0 published_y_center_crc=0 published_y_br_crc=0 "
                   "published_uv_center_crc=0 published_combined_crc=0x%llx published_matches_decode=%u published_matches_previous_visible=%u fingerprint_level=%u "
                   "fingerprint_kind=metadata",
                   static_cast<unsigned long long>(resource->av1_frame_sequence), surface->id, static_cast<unsigned long long>(resource->content_generation), published_path,
                   vkvv_trace_handle(resource->export_resource.image), resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                   resource->import.external ? 1U : 0U, static_cast<unsigned long long>(resource->import_present_generation), crc_valid ? 1U : 0U,
                   static_cast<unsigned long long>(published_crc), matches_decode ? 1U : 0U, matches_previous ? 1U : 0U, fingerprint_level);

        VKVV_TRACE("av1-visible-frame-identity",
                   "frame_seq=%llu surface=%u stream=%llu codec=0x%x content_generation=%llu order_hint=%u frame_type=%u tile_source=%s decode_crc=0x%llx published_crc=0x%llx "
                   "published_matches_decode=%u published_matches_previous_visible=%u output_published=%u",
                   static_cast<unsigned long long>(resource->av1_frame_sequence), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), resource->av1_order_hint, resource->av1_frame_type,
                   resource->av1_tile_source != nullptr ? resource->av1_tile_source : "unknown", static_cast<unsigned long long>(resource->av1_decode_fingerprint),
                   static_cast<unsigned long long>(published_crc), matches_decode ? 1U : 0U, matches_previous ? 1U : 0U, published ? 1U : 0U);

        const char* failure_stage = "none";
        const char* failure_reason = "none";
        if (!resource->av1_tile_ranges_valid) {
            failure_stage  = "tile";
            failure_reason = "invalid-tile-ranges";
        } else if (!resource->av1_references_valid) {
            failure_stage  = "dpb";
            failure_reason = "invalid-references";
        } else if (!published) {
            failure_stage  = resource->import.external ? "import" : "publish";
            failure_reason = resource->import.external ? "unsupported-import-image" : "unpublished-output";
        } else if (crc_valid && !matches_decode) {
            failure_stage  = "publish";
            failure_reason = "published-fingerprint-mismatch";
        }
        VKVV_TRACE("av1-visible-frame-audit",
                   "frame_seq=%llu surface=%u stream=%llu order_hint=%u frame_type=%u show_frame=%u show_existing_frame=%u refresh_frame_flags=0x%02x content_generation=%llu "
                   "tile_source=%s tile_count=%u tile_ranges_valid=%u tile_sum_size=%u setup_slot=%d target_dpb_slot=%d references_valid=%u reference_count=%u "
                   "decode_crc_valid=%u decode_crc=0x%llx published_path=%s published_crc_valid=%u published_crc=0x%llx published_matches_decode=%u "
                   "published_matches_previous_visible=%u output_published=%u failure_stage=%s failure_reason=%s",
                   static_cast<unsigned long long>(resource->av1_frame_sequence), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->av1_order_hint,
                   resource->av1_frame_type, resource->av1_visible_show_frame ? 1U : 0U, resource->av1_visible_show_existing_frame ? 1U : 0U,
                   resource->av1_visible_refresh_frame_flags, static_cast<unsigned long long>(resource->content_generation),
                   resource->av1_tile_source != nullptr ? resource->av1_tile_source : "unknown", resource->av1_tile_count, resource->av1_tile_ranges_valid ? 1U : 0U,
                   resource->av1_tile_sum_size, resource->av1_setup_slot, resource->av1_target_dpb_slot, resource->av1_references_valid ? 1U : 0U,
                   resource->av1_reference_count, crc_valid ? 1U : 0U, static_cast<unsigned long long>(resource->av1_decode_fingerprint), published_path, crc_valid ? 1U : 0U,
                   static_cast<unsigned long long>(published_crc), matches_decode ? 1U : 0U, matches_previous ? 1U : 0U, published ? 1U : 0U, failure_stage, failure_reason);

        if (published && crc_valid) {
            resource->av1_previous_visible_fingerprint = published_crc;
        }
    }

    void trace_av1_visible_output_check(VkvvSurface* surface, SurfaceResource* resource, bool refresh_export) {
        if (surface == nullptr || resource == nullptr || !surface_resource_uses_av1_decode(resource) || !refresh_export || resource->content_generation == 0) {
            return;
        }

        const bool trace_valid      = resource->av1_visible_output_trace_valid;
        const bool shadow_current   = surface_resource_has_current_export_shadow(resource);
        const bool shadow_published = surface_resource_has_exported_shadow_output(resource);
        const bool import_published = surface_resource_has_direct_import_output(resource);
        const bool published        = shadow_published || import_published;
        VKVV_TRACE("av1-visible-output-check",
                   "surface=%u show_frame=%u show_existing_frame=%u refresh_frame_flags=0x%02x frame_to_show_map_idx=%d refresh_export=%u content_gen=%llu shadow_mem=0x%llx "
                   "shadow_gen=%llu shadow_ok=%u shadow_published=%u import_external=%u import_present_generation=%llu direct_import_ok=%u import_published=%u exported=%u "
                   "shadow_exported=%u result=%s",
                   surface->id, trace_valid && resource->av1_visible_show_frame ? 1U : 0U, trace_valid && resource->av1_visible_show_existing_frame ? 1U : 0U,
                   trace_valid ? resource->av1_visible_refresh_frame_flags : 0U, trace_valid ? resource->av1_visible_frame_to_show_map_idx : -1, refresh_export ? 1U : 0U,
                   static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                   static_cast<unsigned long long>(resource->export_resource.content_generation), shadow_current ? 1U : 0U, shadow_published ? 1U : 0U,
                   resource->import.external ? 1U : 0U, static_cast<unsigned long long>(resource->import_present_generation), import_published ? 1U : 0U,
                   import_published ? 1U : 0U, resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, published ? "published" : "unpublished");
        if (published) {
            if (resource->import.external && import_published) {
                VKVV_TRACE("import-output-copy-enter",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_external=1 import_fd_valid=%u import_fd_dev=%llu import_fd_ino=%llu "
                           "import_fourcc=0x%x import_modifier_valid=%u import_modifier=0x%llx import_image_handle=0x0 import_memory_handle=0x0 decode_image_handle=0x%llx "
                           "shadow_image_handle=0x%llx copy_required=%u",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           resource->import.fd.valid ? 1U : 0U, static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino),
                           resource->va_fourcc, resource->has_drm_format_modifier ? 1U : 0U, static_cast<unsigned long long>(resource->drm_format_modifier),
                           vkvv_trace_handle(resource->image), vkvv_trace_handle(resource->export_resource.image), resource->decode_image_is_imported_image ? 0U : 1U);
                VKVV_TRACE("import-output-copy-done",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_external=1 import_present_generation=%llu copy_done=%u layout_before=%d layout_after=%d",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(resource->import_present_generation), resource->decode_image_is_imported_image ? 1U : 0U, resource->layout, resource->layout);
                VKVV_TRACE("export-copy-proof",
                           "codec=0x%x surface=%u source_surface=%u target_surface=%u source_content_gen=%llu target_content_gen_before=%llu target_content_gen_after=%llu "
                           "source_shadow_gen=%llu target_shadow_gen_before=%llu target_shadow_gen_after=%llu copy_reason=%s refresh_export=1",
                           resource->codec_operation, surface->id, surface->id, surface->id, static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import_present_generation),
                           static_cast<unsigned long long>(resource->export_resource.content_generation),
                           static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import_present_generation),
                           vkvv_export_copy_reason_name(VkvvExportCopyReason::ImportOutput));
                VKVV_TRACE("import-output-release-barrier",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu release_barrier_done=%u queue_family_released=%u layout_before=%d layout_after=%d",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           resource->import_present_barrier_done ? 1U : 0U, resource->import_present_barrier_done ? 1U : 0U, resource->layout, resource->layout);
                VKVV_TRACE("import-present-mark",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_present_generation=%llu import_fd_dev=%llu import_fd_ino=%llu",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import_fd_dev),
                           static_cast<unsigned long long>(resource->import_fd_ino));
            }
            VKVV_TRACE("av1-visible-output-published",
                       "surface=%u stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu shadow_published=%u import_published=%u exported=%u shadow_exported=%u "
                       "import_external=%u direct_import_ok=%u import_present_generation=%llu import_fd_dev=%llu import_fd_ino=%llu decode_image=0x%llx import_image=0x0 "
                       "copy_done=%u layout_released=%u queue_family_released=%u",
                       surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), shadow_published ? 1U : 0U, import_published ? 1U : 0U,
                       resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, resource->import.external ? 1U : 0U, import_published ? 1U : 0U,
                       static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import.fd.dev),
                       static_cast<unsigned long long>(resource->import.fd.ino), vkvv_trace_handle(resource->image), import_published ? 1U : 0U, import_published ? 1U : 0U,
                       import_published ? 1U : 0U);
        } else {
            if (resource->import.external) {
                VKVV_TRACE("import-output-copy-enter",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_external=1 import_fd_valid=%u import_fd_dev=%llu import_fd_ino=%llu "
                           "import_fourcc=0x%x import_modifier_valid=%u import_modifier=0x%llx import_image_handle=0x0 import_memory_handle=0x0 decode_image_handle=0x%llx "
                           "shadow_image_handle=0x%llx copy_required=1",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           resource->import.fd.valid ? 1U : 0U, static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino),
                           resource->va_fourcc, resource->has_drm_format_modifier ? 1U : 0U, static_cast<unsigned long long>(resource->drm_format_modifier),
                           vkvv_trace_handle(resource->image), vkvv_trace_handle(resource->export_resource.image));
                VKVV_TRACE("import-output-copy-failed",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu import_external=%u direct_import_ok=%u import_present_generation=%llu "
                           "import_fd_dev=%llu import_fd_ino=%llu decode_image=0x%llx import_image=0x0 copy_done=0 layout_released=0 queue_family_released=0 "
                           "reason=unsupported-import-image",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                           static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                           resource->import.external ? 1U : 0U, import_published ? 1U : 0U, static_cast<unsigned long long>(resource->import_present_generation),
                           static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino), vkvv_trace_handle(resource->image));
            }
            VKVV_TRACE("av1-visible-output-not-published",
                       "surface=%u stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu shadow_published=%u import_published=%u exported=%u shadow_exported=%u "
                       "import_external=%u direct_import_ok=%u import_present_generation=%llu import_fd_dev=%llu import_fd_ino=%llu decode_image=0x%llx import_image=0x0 "
                       "copy_done=0 layout_released=0 queue_family_released=0",
                       surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), shadow_published ? 1U : 0U, import_published ? 1U : 0U,
                       resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, resource->import.external ? 1U : 0U, import_published ? 1U : 0U,
                       static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import.fd.dev),
                       static_cast<unsigned long long>(resource->import.fd.ino), vkvv_trace_handle(resource->image));
        }
        trace_av1_publication_fingerprint(surface, resource, refresh_export, shadow_published, import_published, published);
        clear_surface_av1_visible_output_trace(resource);
    }

} // namespace

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
            trace_av1_visible_output_check(surface, resource, refresh_export);
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
        trace_av1_visible_output_check(surface, resource, refresh_export);
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
        const bool        predecode_before   = resource->export_resource.predecode_exported;
        const bool        seeded_before      = resource->export_resource.predecode_seeded;
        const VASurfaceID seed_source_before = resource->export_resource.seed_source_surface_id;
        const uint64_t    old_shadow_gen     = resource->export_resource.content_generation;
        resource->export_seed_generation     = 0;
        unregister_export_seed_resource(runtime, resource);
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
        }
        clear_nondisplay_predecode_presentation_state(resource);
        constexpr bool private_decode_shadow_supported = false;
        const bool     has_exported_backing            = resource->export_resource.exported && resource->export_resource.image != VK_NULL_HANDLE;
        const bool     already_current                 = resource->content_generation != 0 && resource->export_resource.content_generation == resource->content_generation;
        const bool     present_pinned_skip =
            private_decode_shadow_supported && has_exported_backing && !already_current && resource->export_resource.present_pinned;
        const char* action               = "no-backing";
        bool        attempted_copy       = false;
        auto trace_nondisplay_post_check = [&](const char* check_action) {
            const bool shadow_stale = resource->content_generation != 0 && resource->export_resource.content_generation != resource->content_generation;
            VKVV_TRACE("nondisplay-export-post-check",
                       "codec=0x%x surface=%u refresh_export=0 content_gen=%llu shadow_gen=%llu shadow_stale=%u exported=%u shadow_exported=%u predecode=%u seeded=%u "
                       "present_pinned=%u presentable=%u present_gen=%llu action=%s",
                       resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), shadow_stale ? 1U : 0U, resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, resource->export_resource.predecode_exported ? 1U : 0U,
                       resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.present_pinned ? 1U : 0U,
                       resource->export_resource.presentable ? 1U : 0U, static_cast<unsigned long long>(resource->export_resource.present_generation), check_action);
        };
        if (has_exported_backing) {
            if (already_current) {
                action = "already-current";
            } else if (present_pinned_skip) {
                action = "present-pinned-skip";
            } else {
                action = resource->export_resource.present_pinned ? "current-refresh-pinned-stabilize" : "current-refresh-unpinned";
                attempted_copy = true;
            }
        }
        VKVV_TRACE("nondisplay-export-guard",
                   "codec=0x%x surface=%u driver=%llu stream=%llu content_gen=%llu shadow_gen=%llu refresh_export=0 exported=%u shadow_exported=%u predecode_before=%u "
                   "seeded_before=%u seed_source_before=%u action=%s attempted_seed=0 attempted_copy=%u present_pinned=%u presentable=%u present_gen=%llu",
                   resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->driver_instance_id),
                   static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(resource->export_resource.content_generation), resource->exported ? 1U : 0U,
                   resource->export_resource.exported ? 1U : 0U, predecode_before ? 1U : 0U, seeded_before ? 1U : 0U, seed_source_before,
                   action, attempted_copy ? 1U : 0U, resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U,
                   static_cast<unsigned long long>(resource->export_resource.present_generation));
        constexpr bool attempted_seed_or_copy = false;
        if (attempted_seed_or_copy) {
            VKVV_TRACE("invalid-nondisplay-export-mutation",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu exported=%u shadow_exported=%u predecode=%u seeded=%u reason=%s",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                       resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, resource->export_resource.predecode_exported ? 1U : 0U,
                       resource->export_resource.predecode_seeded ? 1U : 0U, "nondisplay-refresh");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (attempted_copy && resource->export_resource.present_pinned && private_decode_shadow_supported) {
            VKVV_TRACE("invalid-nondisplay-present-mutation",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu present_gen=%llu exported=%u shadow_exported=%u "
                       "copy_reason=%s refresh_export=0",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                       resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation),
                       static_cast<unsigned long long>(resource->export_resource.present_generation), resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, vkvv_export_copy_reason_name(VkvvExportCopyReason::NondisplayCurrentRefresh));
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (attempted_copy) {
            if (!refresh_nondisplay_export_resource(runtime, resource, reason, reason_size)) {
                VKVV_TRACE("nondisplay-export-current-refresh",
                           "codec=0x%x surface=%u stream=%llu driver=%llu content_gen=%llu old_shadow_gen=%llu new_shadow_gen=%llu exported=%u shadow_exported=%u "
                           "predecode_before=%u seeded_before=%u predecode_after=%u seeded_after=%u attempted_seed=0 attempted_copy=1 display_published=0 present_pinned=%u "
                           "presentable=%u copy_status=failed",
                           resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->stream_id),
                           static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(old_shadow_gen), static_cast<unsigned long long>(resource->export_resource.content_generation),
                           resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, predecode_before ? 1U : 0U, seeded_before ? 1U : 0U,
                           resource->export_resource.predecode_exported ? 1U : 0U, resource->export_resource.predecode_seeded ? 1U : 0U,
                           resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U);
                trace_nondisplay_post_check("failed");
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            VKVV_TRACE("nondisplay-export-current-refresh",
                       "codec=0x%x surface=%u stream=%llu driver=%llu content_gen=%llu old_shadow_gen=%llu new_shadow_gen=%llu exported=%u shadow_exported=%u "
                       "predecode_before=%u seeded_before=%u predecode_after=%u seeded_after=%u attempted_seed=0 attempted_copy=1 display_published=0 present_pinned=%u "
                       "presentable=%u copy_status=success",
                       resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->stream_id),
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(old_shadow_gen), static_cast<unsigned long long>(resource->export_resource.content_generation),
                       resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, predecode_before ? 1U : 0U, seeded_before ? 1U : 0U,
                       resource->export_resource.predecode_exported ? 1U : 0U, resource->export_resource.predecode_seeded ? 1U : 0U,
                       resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U);
            trace_export_present_state(resource, &resource->export_resource, "nondisplay-current-refresh-unpinned", false, false);
        } else if (present_pinned_skip) {
            VKVV_TRACE("nondisplay-present-pinned-skip",
                       "surface=%u codec=0x%x stream=%llu driver=%llu content_gen=%llu shadow_gen=%llu present_gen=%llu refresh_export=0 exported=%u shadow_exported=%u "
                       "present_pinned=1 presentable=%u mutation_action=skipped-client-shadow",
                       surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id),
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation),
                       static_cast<unsigned long long>(resource->export_resource.present_generation), resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U);
            trace_export_present_state(resource, &resource->export_resource, "nondisplay-present-pinned-skip", false, false);
        }
        trace_nondisplay_post_check(action);
        if (resource->export_resource.exported && resource->export_resource.image != VK_NULL_HANDLE && resource->content_generation != 0 &&
            resource->export_resource.content_generation != resource->content_generation && !resource->export_resource.present_pinned) {
            VKVV_TRACE("invalid-nondisplay-stale-export-shadow",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu exported=%u shadow_exported=%u action=%s",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                       resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, action);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        VKVV_TRACE("export-refresh-skip-nondisplay",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu seed_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_stale=%u exported=%u shadow_exported=%u "
                   "predecode=%u seeded=%u retired_predecode=%u fd_stat=%u fd_dev=%llu fd_ino=%llu last_display_gen=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_seed_generation),
                   vkvv_trace_handle(resource->export_resource.memory), static_cast<unsigned long long>(resource->export_resource.content_generation),
                   resource->content_generation != 0 && resource->export_resource.content_generation != resource->content_generation ? 1U : 0U, resource->exported ? 1U : 0U,
                   resource->export_resource.exported ? 1U : 0U,
                   resource->export_resource.predecode_exported ? 1U : 0U, resource->export_resource.predecode_seeded ? 1U : 0U, retired_predecode_export ? 1U : 0U,
                   resource->export_resource.fd_stat_valid ? 1U : 0U, static_cast<unsigned long long>(resource->export_resource.fd_dev),
                   static_cast<unsigned long long>(resource->export_resource.fd_ino), static_cast<unsigned long long>(resource->last_display_refresh_generation));
        std::snprintf(reason, reason_size,
                      "handled non-display %s export refresh as %s: driver=%llu surface=%u stream=%llu codec=0x%x export_mem=%llu retained=%zu retained_mem=%llu "
                      "source_generation=%llu shadow_generation=%llu",
                      format->name, action, static_cast<unsigned long long>(resource->driver_instance_id), surface->id, static_cast<unsigned long long>(resource->stream_id),
                      resource->codec_operation, static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
                      static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
                      static_cast<unsigned long long>(resource->export_resource.content_generation));
        return VA_STATUS_SUCCESS;
    }

    uint32_t seeded_predecode_exports = 0;
    if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    const bool                    display_visible      = refresh_has_visible_display(resource, refresh_export);
    const VkvvExportPresentSource present_source       = visible_present_source(resource);
    const bool                    visible_shadow_ready = display_visible && surface_resource_has_exported_shadow_output(resource);
    if (visible_shadow_ready) {
        pin_export_visible_present(resource, &resource->export_resource, present_source);
        trace_export_present_state(resource, &resource->export_resource, "visible-present-pin", refresh_export, display_visible);
    }
    trace_av1_visible_output_check(surface, resource, refresh_export);
    const bool visible_shadow_published = surface_resource_has_exported_shadow_output(resource);
    const bool visible_import_published = surface_resource_has_direct_import_output(resource);
    if (visible_shadow_published && !resource->export_resource.present_pinned) {
        VKVV_TRACE("invalid-visible-without-present-pin",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu present_gen=%llu published_path=exported-shadow",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                   resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(resource->export_resource.content_generation),
                   static_cast<unsigned long long>(resource->export_resource.present_generation));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    VKVV_TRACE("visible-output-proof",
               "codec=0x%x surface=%u content_gen=%llu order_hint_or_frame_num=%llu published_path=%s published_gen=%llu previous_visible_surface=%u previous_visible_gen=%llu "
               "published_matches_previous=%u",
               resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->content_generation),
               surface_resource_uses_av1_decode(resource) ? static_cast<unsigned long long>(resource->av1_order_hint) : static_cast<unsigned long long>(resource->content_generation),
               av1_published_path(resource, visible_shadow_published, visible_import_published),
               visible_shadow_published ? static_cast<unsigned long long>(resource->export_resource.content_generation) :
                                          static_cast<unsigned long long>(resource->import_present_generation),
               resource->last_display_refresh_generation != 0 ? surface->id : VA_INVALID_ID,
               static_cast<unsigned long long>(resource->last_display_refresh_generation),
               resource->last_display_refresh_generation != 0 && resource->last_display_refresh_generation == resource->content_generation ? 1U : 0U);
    if (requires_published_visible_output && !surface_resource_has_published_visible_output(resource)) {
        VKVV_TRACE("visible-refresh-unpublished",
                   "surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_ok=%u shadow_published=%u "
                   "import_external=%u import_present_generation=%llu direct_import_ok=%u import_published=%u exported=%u shadow_exported=%u retained=%zu retained_mem=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   refresh_export ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                   static_cast<unsigned long long>(resource->export_resource.content_generation), surface_resource_has_current_export_shadow(resource) ? 1U : 0U,
                   surface_resource_has_exported_shadow_output(resource) ? 1U : 0U, resource->import.external ? 1U : 0U,
                   static_cast<unsigned long long>(resource->import_present_generation), surface_resource_has_direct_import_output(resource) ? 1U : 0U,
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
        if (!surface->decoded) {
            mark_export_predecode_nonpresentable(exported_shadow);
            trace_export_present_state(resource, exported_shadow, "predecode-export", false, false);
        }
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
