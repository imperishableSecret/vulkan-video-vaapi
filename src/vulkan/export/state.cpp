#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vkvv {

    const char* vkvv_export_copy_reason_name(VkvvExportCopyReason reason) {
        switch (reason) {
            case VkvvExportCopyReason::VisibleRefresh: return "visible-refresh";
            case VkvvExportCopyReason::PredecodePlaceholderSeed: return "predecode-placeholder-seed";
            case VkvvExportCopyReason::ImportOutput: return "import-output";
            case VkvvExportCopyReason::NondisplayCurrentRefresh: return "nondisplay-current-refresh";
            case VkvvExportCopyReason::NondisplayPrivateRefresh: return "nondisplay-private-refresh";
            default: return "unknown";
        }
    }

    const char* thumbnail_predecode_action_name(ThumbnailPredecodeAction action) {
        switch (action) {
            case ThumbnailPredecodeAction::NormalPredecodePolicy: return "normal";
            case ThumbnailPredecodeAction::DrainAndExport: return "drain";
            case ThumbnailPredecodeAction::StreamLocalSeed: return "seed";
            case ThumbnailPredecodeAction::FailExport: return "fail";
            case ThumbnailPredecodeAction::DebugPlaceholder: return "debug-placeholder";
            default: return "unknown";
        }
    }

    const char* vkvv_export_present_source_name(VkvvExportPresentSource source) {
        switch (source) {
            case VkvvExportPresentSource::None: return "none";
            case VkvvExportPresentSource::VisibleRefresh: return "visible-refresh";
            case VkvvExportPresentSource::ShowExisting: return "show-existing";
            case VkvvExportPresentSource::PredecodePlaceholder: return "predecode-placeholder";
            case VkvvExportPresentSource::PrivateNondisplay: return "private-nondisplay";
            default: return "unknown";
        }
    }

    const char* vkvv_external_release_mode_name(VkvvExternalReleaseMode mode) {
        switch (mode) {
            case VkvvExternalReleaseMode::NoneRequired: return "none-required";
            case VkvvExternalReleaseMode::ConcurrentSharing: return "concurrent-sharing-no-qfot";
            case VkvvExternalReleaseMode::QueueFamilyOwnershipTransfer: return "queue-family-ownership-transfer";
            case VkvvExternalReleaseMode::ImplicitSyncOnly: return "implicit-sync-only";
            case VkvvExternalReleaseMode::ExplicitSyncFile: return "explicit-sync-file";
            case VkvvExternalReleaseMode::ForceBarrierDebug: return "force-barrier";
            case VkvvExternalReleaseMode::NoSyncDebug: return "none";
            default: return "unknown";
        }
    }

    VkvvExternalReleaseMode configured_export_release_mode() {
        const char* value = std::getenv("VKVV_EXPORT_SYNC_MODE");
        if (value == nullptr || value[0] == '\0' || std::strcmp(value, "implicit") == 0) {
            return VkvvExternalReleaseMode::ImplicitSyncOnly;
        }
        if (std::strcmp(value, "force-barrier") == 0) {
            return VkvvExternalReleaseMode::ForceBarrierDebug;
        }
        if (std::strcmp(value, "explicit") == 0) {
            return VkvvExternalReleaseMode::ExplicitSyncFile;
        }
        if (std::strcmp(value, "none") == 0) {
            return VkvvExternalReleaseMode::NoSyncDebug;
        }
        return VkvvExternalReleaseMode::ImplicitSyncOnly;
    }

    bool av1_export_env_flag_enabled(const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0;
    }

    bool surface_resource_thumbnail_like(const SurfaceResource* resource) {
        if (resource == nullptr) {
            return false;
        }
        const uint32_t width  = resource->visible_extent.width != 0 ? resource->visible_extent.width : resource->coded_extent.width;
        const uint32_t height = resource->visible_extent.height != 0 ? resource->visible_extent.height : resource->coded_extent.height;
        return width != 0 && height != 0 && width <= 960 && height <= 540;
    }

    bool debug_allow_thumbnail_placeholder() {
        return av1_export_env_flag_enabled("VKVV_ALLOW_THUMBNAIL_PLACEHOLDER");
    }

    namespace {

        void set_thumbnail_reason(char* reason, size_t reason_size, const char* text) {
            if (reason != nullptr && reason_size > 0) {
                std::snprintf(reason, reason_size, "%s", text != nullptr ? text : "");
            }
        }

        bool surface_resource_has_pending_decode(VulkanRuntime* runtime, const SurfaceResource* resource) {
            if (runtime == nullptr || resource == nullptr) {
                return false;
            }
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            for (const CommandSlot& slot : runtime->command_slots) {
                if (slot.pending.surface != nullptr && slot.pending.surface->vulkan == resource) {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    ThumbnailPredecodeAction decide_thumbnail_predecode_export(VulkanRuntime* runtime, SurfaceResource* resource, char* reason, size_t reason_size) {
        if (!surface_resource_thumbnail_like(resource)) {
            set_thumbnail_reason(reason, reason_size, "not-thumbnail");
            return ThumbnailPredecodeAction::NormalPredecodePolicy;
        }
        if (resource->content_generation > 0) {
            set_thumbnail_reason(reason, reason_size, "decoded-content");
            return ThumbnailPredecodeAction::DrainAndExport;
        }
        if (surface_resource_has_pending_decode(runtime, resource)) {
            set_thumbnail_reason(reason, reason_size, "pending-decode");
            return ThumbnailPredecodeAction::DrainAndExport;
        }
        if (debug_allow_thumbnail_placeholder()) {
            set_thumbnail_reason(reason, reason_size, "debug-placeholder-enabled");
            return ThumbnailPredecodeAction::DebugPlaceholder;
        }
        set_thumbnail_reason(reason, reason_size, "no-valid-content");
        return ThumbnailPredecodeAction::FailExport;
    }

    bool surface_resource_uses_av1_decode(const SurfaceResource* resource) {
        return resource != nullptr && resource->codec_operation == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    }

    bool av1_non_display_export_refresh(const SurfaceResource* resource, bool refresh_export) {
        return surface_resource_uses_av1_decode(resource) && !refresh_export;
    }

    bool surface_resource_export_shadow_stale(const SurfaceResource* resource) {
        return resource != nullptr && resource->content_generation != 0 && resource->export_resource.content_generation != resource->content_generation;
    }

    bool surface_resource_decode_shadow_stale(const SurfaceResource* resource) {
        return resource != nullptr && resource->content_generation != 0 && !surface_resource_has_current_decode_shadow(resource);
    }

    bool surface_resource_has_current_decode_shadow(const SurfaceResource* resource) {
        return current_decode_shadow(resource) != nullptr;
    }

    bool surface_resource_has_current_export_shadow(const SurfaceResource* resource) {
        if (resource != nullptr && export_resource_fd_may_be_sampled_by_client(&resource->export_resource) &&
            resource->export_resource.exported_fd.fd_content_generation != resource->export_resource.content_generation) {
            return false;
        }
        return resource != nullptr && resource->content_generation != 0 && resource->export_resource.image != VK_NULL_HANDLE &&
            resource->export_resource.memory != VK_NULL_HANDLE && resource->export_resource.content_generation == resource->content_generation &&
            !surface_resource_export_shadow_stale(resource);
    }

    bool export_visible_release_satisfied(const ExportResource* resource) {
        if (resource == nullptr || resource->content_generation == 0) {
            return false;
        }
        const ExternalSyncState& sync = resource->external_sync;
        if (sync.released_generation != resource->content_generation) {
            return false;
        }
        return sync.external_release_done || !sync.external_release_required || sync.release_mode == VkvvExternalReleaseMode::NoneRequired ||
            sync.release_mode == VkvvExternalReleaseMode::ConcurrentSharing || sync.release_mode == VkvvExternalReleaseMode::ForceBarrierDebug ||
            sync.release_mode == VkvvExternalReleaseMode::NoSyncDebug;
    }

    bool export_resource_fd_may_be_sampled_by_client(const ExportResource* resource) {
        return resource != nullptr && resource->exported_fd.fd_exported && resource->exported_fd.may_be_sampled_by_client && !resource->exported_fd.detached_from_surface;
    }

    uint64_t export_resource_fd_content_generation(const ExportResource* resource) {
        return resource != nullptr && resource->exported_fd.fd_exported ? resource->exported_fd.fd_content_generation : 0;
    }

    bool export_resource_fd_fresh(const SurfaceResource* resource) {
        if (resource == nullptr || resource->content_generation == 0 || !export_resource_fd_may_be_sampled_by_client(&resource->export_resource)) {
            return true;
        }
        return resource->export_resource.exported_fd.fd_content_generation >= resource->content_generation &&
            resource->export_resource.content_generation == resource->content_generation;
    }

    bool surface_resource_visible_publish_ready(const SurfaceResource* resource, bool display_visible, bool copy_done, bool pixel_proof_required) {
        if (resource == nullptr) {
            return false;
        }
        const bool pixel_match_ok = !pixel_proof_required || (resource->decode_pixel_proof_valid && resource->present_pixel_proof_valid && resource->present_pixel_matches_decode);
        return display_visible && copy_done && surface_resource_has_current_export_shadow(resource) && export_visible_release_satisfied(&resource->export_resource) &&
            export_resource_fd_fresh(resource) && pixel_match_ok;
    }

    bool surface_resource_has_exported_shadow_output(const SurfaceResource* resource) {
        return surface_resource_has_current_export_shadow(resource) && resource->exported && resource->export_resource.exported &&
            !resource->export_resource.predecode_quarantined && resource->export_resource.presentable && resource->export_resource.present_pinned &&
            resource->export_resource.published_visible && resource->export_resource.present_generation == resource->content_generation &&
            export_visible_release_satisfied(&resource->export_resource) && export_resource_fd_fresh(resource);
    }

    bool surface_resource_has_direct_import_output(const SurfaceResource* resource) {
        if (av1_export_env_flag_enabled("VKVV_AV1_DISABLE_IMPORTED_OUTPUT") || av1_export_env_flag_enabled("VKVV_AV1_FORCE_EXPORTED_SHADOW")) {
            return false;
        }
        if (resource == nullptr || resource->content_generation == 0 || !resource->import.external || !resource->import.fd.valid || !resource->direct_import_presentable ||
            !resource->decode_image_is_imported_image || !resource->import_present_barrier_done || !resource->import_fd_stat_valid) {
            return false;
        }
        return resource->import_present_generation == resource->content_generation && resource->import_fd_dev == resource->import.fd.dev &&
            resource->import_fd_ino == resource->import.fd.ino && resource->import_driver_instance_id == resource->driver_instance_id &&
            resource->import_stream_id == resource->stream_id && resource->import_codec_operation == resource->codec_operation;
    }

    bool surface_resource_has_published_visible_output(const SurfaceResource* resource) {
        return surface_resource_has_exported_shadow_output(resource) || surface_resource_has_direct_import_output(resource);
    }

    bool surface_resource_requires_visible_publication(const SurfaceResource* resource, bool refresh_export) {
        return refresh_export && resource != nullptr && resource->content_generation != 0 && surface_resource_uses_av1_decode(resource) &&
            (resource->import.external || resource->exported || resource->export_resource.exported);
    }

    bool av1_visible_export_requires_copy(const SurfaceResource* resource) {
        return surface_resource_uses_av1_decode(resource) &&
            (resource->export_resource.predecode_exported || resource->export_resource.predecode_seeded || resource->export_resource.black_placeholder ||
             resource->export_retained_attached || resource->export_import_attached || surface_resource_export_shadow_stale(resource));
    }

    void clear_export_present_state(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->present_pinned            = false;
        resource->presentable               = false;
        resource->published_visible         = false;
        resource->present_generation        = 0;
        resource->present_fd_dev            = 0;
        resource->present_fd_ino            = 0;
        resource->present_surface_id        = VA_INVALID_ID;
        resource->present_stream_id         = 0;
        resource->present_codec_operation   = 0;
        resource->present_source            = VkvvExportPresentSource::None;
        resource->client_visible_shadow     = false;
        resource->private_nondisplay_shadow = false;
        resource->external_sync             = {};
    }

    void mark_export_fd_returned(ExportResource* resource, const VkvvFdIdentity& fd, uint64_t content_generation) {
        if (resource == nullptr) {
            return;
        }
        resource->fd_stat_valid                               = fd.valid;
        resource->fd_dev                                      = fd.dev;
        resource->fd_ino                                      = fd.ino;
        resource->exported_fd.fd_exported                     = fd.valid;
        resource->exported_fd.fd_dev                          = fd.dev;
        resource->exported_fd.fd_ino                          = fd.ino;
        resource->exported_fd.may_be_sampled_by_client        = fd.valid;
        resource->exported_fd.fd_content_generation           = content_generation;
        resource->exported_fd.last_written_content_generation = content_generation;
        resource->exported_fd.detached_from_surface           = false;
    }

    void mark_export_fd_written(ExportResource* resource, uint64_t content_generation) {
        if (resource == nullptr || !resource->exported_fd.fd_exported) {
            return;
        }
        resource->exported_fd.fd_content_generation           = content_generation;
        resource->exported_fd.last_written_content_generation = content_generation;
        resource->exported_fd.detached_from_surface           = false;
    }

    void mark_export_fd_detached(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->exported_fd.detached_from_surface    = true;
        resource->exported_fd.may_be_sampled_by_client = false;
    }

    void mark_export_predecode_nonpresentable(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        clear_export_present_state(resource);
        resource->present_source = VkvvExportPresentSource::PredecodePlaceholder;
    }

    void pin_export_visible_present(SurfaceResource* owner, ExportResource* resource, VkvvExportPresentSource source) {
        if (owner == nullptr || resource == nullptr) {
            return;
        }
        resource->present_pinned            = true;
        resource->presentable               = true;
        resource->published_visible         = true;
        resource->present_generation        = owner->content_generation;
        resource->present_fd_dev            = resource->fd_dev;
        resource->present_fd_ino            = resource->fd_ino;
        resource->present_surface_id        = owner->surface_id;
        resource->present_stream_id         = owner->stream_id;
        resource->present_codec_operation   = owner->codec_operation;
        resource->present_source            = source;
        resource->client_visible_shadow     = true;
        resource->private_nondisplay_shadow = false;
    }

    ExportResource* client_present_shadow(SurfaceResource* resource) {
        if (resource == nullptr || resource->export_resource.image == VK_NULL_HANDLE || resource->export_resource.memory == VK_NULL_HANDLE) {
            return nullptr;
        }
        return &resource->export_resource;
    }

    const ExportResource* client_present_shadow(const SurfaceResource* resource) {
        if (resource == nullptr || resource->export_resource.image == VK_NULL_HANDLE || resource->export_resource.memory == VK_NULL_HANDLE) {
            return nullptr;
        }
        return &resource->export_resource;
    }

    ExportResource* current_decode_shadow(SurfaceResource* resource) {
        if (resource == nullptr || resource->content_generation == 0) {
            return nullptr;
        }
        ExportResource& present        = resource->export_resource;
        ExportResource& private_shadow = resource->private_decode_shadow;
        if (present.decode_shadow_private_active && private_shadow.image != VK_NULL_HANDLE && private_shadow.memory != VK_NULL_HANDLE &&
            private_shadow.content_generation == resource->content_generation && present.decode_shadow_generation == resource->content_generation) {
            return &private_shadow;
        }
        if (present.image != VK_NULL_HANDLE && present.memory != VK_NULL_HANDLE && present.content_generation == resource->content_generation) {
            return &present;
        }
        return nullptr;
    }

    const ExportResource* current_decode_shadow(const SurfaceResource* resource) {
        if (resource == nullptr || resource->content_generation == 0) {
            return nullptr;
        }
        const ExportResource& present        = resource->export_resource;
        const ExportResource& private_shadow = resource->private_decode_shadow;
        if (present.decode_shadow_private_active && private_shadow.image != VK_NULL_HANDLE && private_shadow.memory != VK_NULL_HANDLE &&
            private_shadow.content_generation == resource->content_generation && present.decode_shadow_generation == resource->content_generation) {
            return &private_shadow;
        }
        if (present.image != VK_NULL_HANDLE && present.memory != VK_NULL_HANDLE && present.content_generation == resource->content_generation) {
            return &present;
        }
        return nullptr;
    }

    void clear_private_decode_shadow_state(SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->export_resource.decode_shadow_generation     = resource->export_resource.content_generation;
        resource->export_resource.decode_shadow_private_active = false;
        resource->private_decode_shadow.content_generation     = 0;
    }

    void clear_predecode_export_state(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->predecode_exported     = false;
        resource->predecode_seeded       = false;
        resource->black_placeholder      = false;
        resource->seed_source_surface_id = VA_INVALID_ID;
        resource->seed_source_generation = 0;
    }

    void clear_predecode_quarantine_state(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->predecode_quarantined = false;
        resource->predecode_fd_dev      = 0;
        resource->predecode_fd_ino      = 0;
        resource->predecode_generation  = 0;
    }

    void enter_predecode_quarantine(const SurfaceResource* owner, ExportResource* resource) {
        if (owner == nullptr || resource == nullptr) {
            return;
        }
        resource->predecode_quarantined = true;
        resource->predecode_fd_dev      = resource->fd_dev;
        resource->predecode_fd_ino      = resource->fd_ino;
        resource->predecode_generation  = resource->content_generation;
        VKVV_TRACE("predecode-quarantine-enter",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu presentable=0 published_visible=0 predecode_exported=%u "
                   "predecode_quarantined=1",
                   owner->surface_id, static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                   static_cast<unsigned long long>(resource->predecode_fd_dev), static_cast<unsigned long long>(resource->predecode_fd_ino),
                   static_cast<unsigned long long>(resource->predecode_generation), resource->predecode_exported ? 1U : 0U);
    }

    void exit_predecode_quarantine(const SurfaceResource* owner, ExportResource* resource, bool release_done) {
        if (owner == nullptr || resource == nullptr || !resource->predecode_quarantined) {
            return;
        }
        const uint64_t fd_dev = resource->predecode_fd_dev;
        const uint64_t fd_ino = resource->predecode_fd_ino;
        clear_predecode_quarantine_state(resource);
        VKVV_TRACE("predecode-quarantine-exit",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu present_gen=%llu release_done=%u predecode_quarantined=0",
                   owner->surface_id, static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                   static_cast<unsigned long long>(fd_dev), static_cast<unsigned long long>(fd_ino), static_cast<unsigned long long>(owner->content_generation),
                   static_cast<unsigned long long>(resource->present_generation), release_done ? 1U : 0U);
    }

    void mark_export_visible_acquire(const SurfaceResource* owner, ExportResource* resource) {
        if (owner == nullptr || resource == nullptr || resource->image == VK_NULL_HANDLE || resource->memory == VK_NULL_HANDLE) {
            return;
        }
        ExternalSyncState& sync        = resource->external_sync;
        sync.external_acquire_required = false;
        sync.external_acquire_done     = true;
        sync.acquired_generation       = resource->content_generation;
        sync.release_mode              = configured_export_release_mode();
        sync.src_queue_family          = invalid_queue_family;
        sync.dst_queue_family          = invalid_queue_family;
        VKVV_TRACE("export-visible-acquire",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu acquired_generation=%llu acquire_required=0 acquire_done=1 acquire_mode=%s "
                   "src_queue_family=%u dst_queue_family=%u",
                   owner->surface_id, static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                   static_cast<unsigned long long>(resource->fd_dev), static_cast<unsigned long long>(resource->fd_ino), static_cast<unsigned long long>(sync.acquired_generation),
                   vkvv_external_release_mode_name(sync.release_mode), sync.src_queue_family, sync.dst_queue_family);
    }

    void mark_export_visible_release(const SurfaceResource* owner, ExportResource* resource, VkImageLayout old_layout, VkImageLayout new_layout) {
        if (owner == nullptr || resource == nullptr || resource->image == VK_NULL_HANDLE || resource->memory == VK_NULL_HANDLE) {
            return;
        }
        const VkvvExternalReleaseMode mode = configured_export_release_mode();
        ExternalSyncState&            sync = resource->external_sync;
        sync.external_release_required     = mode == VkvvExternalReleaseMode::ExplicitSyncFile;
        sync.external_release_done         = mode != VkvvExternalReleaseMode::ExplicitSyncFile;
        sync.src_queue_family              = invalid_queue_family;
        sync.dst_queue_family              = invalid_queue_family;
        sync.last_internal_layout          = new_layout;
        sync.external_layout               = new_layout;
        sync.released_generation           = resource->content_generation;
        sync.release_mode                  = mode;
        resource->layout                   = new_layout;
        VKVV_TRACE("export-visible-release",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu present_gen=%llu old_layout=%d new_layout=%d src_queue_family=%u "
                   "dst_queue_family=%u release_required=%u release_done=%u release_mode=%s sync_fd=-1 semaphore_exported=0 fence_waited=1",
                   owner->surface_id, static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                   static_cast<unsigned long long>(resource->fd_dev), static_cast<unsigned long long>(resource->fd_ino), static_cast<unsigned long long>(owner->content_generation),
                   static_cast<unsigned long long>(resource->present_generation), old_layout, new_layout, sync.src_queue_family, sync.dst_queue_family,
                   sync.external_release_required ? 1U : 0U, sync.external_release_done ? 1U : 0U, vkvv_external_release_mode_name(sync.release_mode));
    }

    bool predecode_seed_source_safe_for_client(const SurfaceResource* source) {
        if (source == nullptr || source->content_generation == 0 || !surface_resource_has_exported_shadow_output(source)) {
            return false;
        }
        const ExportResource& export_resource = source->export_resource;
        return export_resource.present_generation != 0 && export_resource_fd_may_be_sampled_by_client(&export_resource) &&
            export_resource.exported_fd.fd_content_generation == export_resource.present_generation && export_visible_release_satisfied(&export_resource);
    }

    void clear_nondisplay_predecode_presentation_state(SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }

        clear_predecode_export_state(&resource->export_resource);
        resource->export_seed_generation                 = 0;
        resource->last_nondisplay_skip_generation        = resource->content_generation;
        resource->last_nondisplay_skip_shadow_generation = resource->export_resource.content_generation;
        resource->last_nondisplay_skip_shadow_memory     = resource->export_resource.memory;
    }

    void clear_surface_export_attach_state(SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->export_retained_attached = false;
        resource->export_import_attached   = false;
    }

    void clear_surface_direct_import_present_state(SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->direct_import_presentable      = false;
        resource->decode_image_is_imported_image = false;
        resource->import_present_barrier_done    = false;
        resource->import_fd_stat_valid           = false;
        resource->import_present_generation      = 0;
        resource->import_fd_dev                  = 0;
        resource->import_fd_ino                  = 0;
        resource->import_driver_instance_id      = 0;
        resource->import_stream_id               = 0;
        resource->import_codec_operation         = 0;
    }

    void clear_surface_av1_visible_output_trace(SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->av1_visible_output_trace_valid    = false;
        resource->av1_visible_show_frame            = false;
        resource->av1_visible_show_existing_frame   = false;
        resource->av1_visible_refresh_frame_flags   = 0;
        resource->av1_visible_frame_to_show_map_idx = -1;
    }

} // namespace vkvv
