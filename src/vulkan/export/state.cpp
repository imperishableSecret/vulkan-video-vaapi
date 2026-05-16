#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace vkvv {

    namespace {

        uint64_t monotonic_ms() {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

    } // namespace

    const char* vkvv_export_copy_reason_name(VkvvExportCopyReason reason) {
        switch (reason) {
            case VkvvExportCopyReason::VisibleRefresh: return "visible-refresh";
            case VkvvExportCopyReason::PredecodeBackingSeed: return "predecode-backing-seed";
            case VkvvExportCopyReason::ImportOutput: return "import-output";
            case VkvvExportCopyReason::NondisplayCurrentRefresh: return "nondisplay-current-refresh";
            case VkvvExportCopyReason::NondisplayPrivateRefresh: return "nondisplay-private-refresh";
            default: return "unknown";
        }
    }

    const char* vkvv_export_pixel_source_name(VkvvExportPixelSource source) {
        switch (source) {
            case VkvvExportPixelSource::None: return "none";
            case VkvvExportPixelSource::DecodedContent: return "decoded";
            case VkvvExportPixelSource::StreamLocalSeed: return "seed";
            case VkvvExportPixelSource::Placeholder: return "placeholder";
            case VkvvExportPixelSource::RetainedUnknown: return "retained-unknown";
            default: return "unknown";
        }
    }

    const char* vkvv_export_role_name(VkvvExportRole role) {
        switch (role) {
            case VkvvExportRole::None: return "none";
            case VkvvExportRole::PredecodeBacking: return "predecode-backing";
            case VkvvExportRole::DecodedPixels: return "decoded";
            case VkvvExportRole::PixelProvenSeed: return "pixel-proven-seed";
            case VkvvExportRole::TransitionHold: return "transition-hold";
            default: return "unknown";
        }
    }

    bool export_role_may_be_sampled_by_client(VkvvExportRole role) {
        return role == VkvvExportRole::DecodedPixels || role == VkvvExportRole::PixelProvenSeed || role == VkvvExportRole::TransitionHold;
    }

    const char* vkvv_export_present_source_name(VkvvExportPresentSource source) {
        switch (source) {
            case VkvvExportPresentSource::None: return "none";
            case VkvvExportPresentSource::VisibleRefresh: return "visible-refresh";
            case VkvvExportPresentSource::ShowExisting: return "show-existing";
            case VkvvExportPresentSource::PredecodeBacking: return "predecode-backing";
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
        return resource != nullptr && resource->exported_fd.fd_exported && resource->exported_fd.may_be_sampled_by_client &&
            export_role_may_be_sampled_by_client(resource->exported_fd.role) && !resource->exported_fd.detached_from_surface;
    }

    uint64_t export_resource_fd_content_generation(const ExportResource* resource) {
        return resource != nullptr && resource->exported_fd.fd_exported ? resource->exported_fd.fd_content_generation : 0;
    }

    VkvvExportRole export_resource_fd_role(const ExportResource* resource) {
        return resource != nullptr && resource->exported_fd.fd_exported ? resource->exported_fd.role : VkvvExportRole::None;
    }

    bool export_resource_has_valid_retained_presentation(const ExportResource* resource) {
        if (resource == nullptr || resource->content_generation == 0 || resource->private_nondisplay_shadow || resource->predecode_exported ||
            resource->predecode_seeded || resource->predecode_quarantined || resource->neutral_backing || resource->seed_source_generation != 0 ||
            resource->seed_source_surface_id != VA_INVALID_ID || resource->seed_pixel_proof_valid) {
            return false;
        }
        if (!resource->exported || !resource->client_visible_shadow || !resource->presentable || !resource->present_pinned || !resource->published_visible ||
            resource->present_generation == 0 || resource->present_generation != resource->content_generation || resource->present_stream_id != resource->stream_id ||
            resource->present_codec_operation != resource->codec_operation || !export_visible_release_satisfied(resource)) {
            return false;
        }
        return resource->exported_fd.fd_exported && resource->exported_fd.fd_content_generation == resource->content_generation &&
            resource->exported_fd.last_written_content_generation == resource->content_generation && resource->exported_fd.role == VkvvExportRole::DecodedPixels;
    }

    bool export_resource_is_transition_hold_for_surface(const SurfaceResource* owner, const ExportResource* resource) {
        return owner != nullptr && resource != nullptr && owner->content_generation == 0 && owner->export_retained_attached &&
            export_resource_has_valid_retained_presentation(resource) && resource->driver_instance_id == owner->driver_instance_id &&
            resource->stream_id == owner->stream_id && resource->codec_operation == owner->codec_operation && resource->format == owner->format &&
            resource->va_fourcc == owner->va_fourcc && resource->extent.width >= owner->coded_extent.width && resource->extent.height >= owner->coded_extent.height;
    }

    bool export_resource_fd_fresh(const SurfaceResource* resource) {
        if (resource == nullptr || resource->content_generation == 0 || !export_resource_fd_may_be_sampled_by_client(&resource->export_resource)) {
            return true;
        }
        return resource->export_resource.exported_fd.fd_content_generation >= resource->content_generation &&
            resource->export_resource.content_generation == resource->content_generation;
    }

    VkvvExportPixelSource export_pixel_source_for_resource(const SurfaceResource* owner, const ExportResource* resource) {
        if (resource == nullptr) {
            return VkvvExportPixelSource::None;
        }
        if (resource->neutral_backing || (resource->predecode_exported && !resource->predecode_seeded && resource->content_generation == 0 &&
                                                resource->seed_source_generation == 0)) {
            return VkvvExportPixelSource::Placeholder;
        }
        if (resource->predecode_seeded || resource->seed_source_generation != 0) {
            return VkvvExportPixelSource::StreamLocalSeed;
        }
        if (owner != nullptr && owner->content_generation != 0 && resource->content_generation == owner->content_generation) {
            return VkvvExportPixelSource::DecodedContent;
        }
        if (resource->content_generation != 0) {
            return VkvvExportPixelSource::RetainedUnknown;
        }
        return VkvvExportPixelSource::None;
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
        if (resource == nullptr) {
            return false;
        }

        const bool exported_shadow = resource->exported && resource->export_resource.exported;
        return surface_resource_has_current_export_shadow(resource) && exported_shadow &&
            !resource->export_resource.predecode_quarantined && resource->export_resource.presentable && resource->export_resource.present_pinned &&
            resource->export_resource.published_visible && resource->export_resource.present_generation == resource->content_generation &&
            export_visible_release_satisfied(&resource->export_resource) && export_resource_fd_fresh(resource);
    }

    bool surface_resource_has_direct_import_output(const SurfaceResource* resource) {
        const bool disabled_imported_output = av1_export_env_flag_enabled("VKVV_AV1_DISABLE_IMPORTED_OUTPUT");
        const bool forced_exported_shadow   = av1_export_env_flag_enabled("VKVV_AV1_FORCE_EXPORTED_SHADOW");
        if (disabled_imported_output || forced_exported_shadow) {
            return false;
        }
        if (resource == nullptr) {
            return false;
        }
        const bool content_ok        = resource->content_generation != 0;
        const bool import_ok         = resource->import.external && resource->import.fd.valid;
        const bool copy_ok           = resource->decode_image_is_imported_image || resource->import_output_copy_done;
        const bool base_state_ok     = content_ok && import_ok && resource->direct_import_presentable && copy_ok && resource->import_present_barrier_done && resource->import_fd_stat_valid;
        const bool generation_match  = resource->import_present_generation == resource->content_generation;
        const bool fd_match          = resource->import_fd_dev == resource->import.fd.dev && resource->import_fd_ino == resource->import.fd.ino;
        const bool driver_match      = resource->import_driver_instance_id == resource->driver_instance_id;
        const bool stream_match      = resource->import_stream_id == resource->stream_id;
        const bool codec_match       = resource->import_codec_operation == resource->codec_operation;
        const bool result            = base_state_ok && generation_match && fd_match && driver_match && stream_match && codec_match;
        if (surface_resource_uses_av1_decode(resource) && resource->import.external && resource->content_generation != 0) {
            VKVV_TRACE("direct-import-output-gate",
                       "surface=%u stream=%llu codec=0x%x content_gen=%llu result=%u disabled_imported_output=%u forced_exported_shadow=%u content_ok=%u import_ok=%u "
                       "direct_presentable=%u copy_ok=%u decode_image_imported=%u import_output_copy_done=%u barrier_done=%u fd_stat_valid=%u generation_match=%u fd_match=%u "
                       "driver_match=%u stream_match=%u codec_match=%u import_present_generation=%llu import_fd_dev=%llu import_fd_ino=%llu current_fd_dev=%llu current_fd_ino=%llu "
                       "import_driver=%llu current_driver=%llu import_stream=%llu current_stream=%llu import_codec=0x%x current_codec=0x%x",
                       resource->surface_id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), result ? 1U : 0U, disabled_imported_output ? 1U : 0U,
                       forced_exported_shadow ? 1U : 0U, content_ok ? 1U : 0U, import_ok ? 1U : 0U, resource->direct_import_presentable ? 1U : 0U,
                       copy_ok ? 1U : 0U, resource->decode_image_is_imported_image ? 1U : 0U, resource->import_output_copy_done ? 1U : 0U,
                       resource->import_present_barrier_done ? 1U : 0U, resource->import_fd_stat_valid ? 1U : 0U, generation_match ? 1U : 0U, fd_match ? 1U : 0U,
                       driver_match ? 1U : 0U, stream_match ? 1U : 0U, codec_match ? 1U : 0U,
                       static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import_fd_dev),
                       static_cast<unsigned long long>(resource->import_fd_ino), static_cast<unsigned long long>(resource->import.fd.dev),
                       static_cast<unsigned long long>(resource->import.fd.ino), static_cast<unsigned long long>(resource->import_driver_instance_id),
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->import_stream_id),
                       static_cast<unsigned long long>(resource->stream_id), resource->import_codec_operation, resource->codec_operation);
        }
        return result;
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
            (resource->export_resource.predecode_exported || resource->export_resource.predecode_seeded || resource->export_resource.neutral_backing ||
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

    void mark_export_fd_returned(ExportResource* resource, const VkvvFdIdentity& fd, uint64_t content_generation, VkvvExportRole role) {
        if (resource == nullptr) {
            return;
        }
        resource->fd_stat_valid                               = fd.valid;
        resource->fd_dev                                      = fd.dev;
        resource->fd_ino                                      = fd.ino;
        resource->exported_fd.fd_exported                     = fd.valid;
        resource->exported_fd.fd_dev                          = fd.dev;
        resource->exported_fd.fd_ino                          = fd.ino;
        resource->exported_fd.may_be_sampled_by_client        = fd.valid && content_generation != 0 && export_role_may_be_sampled_by_client(role);
        resource->exported_fd.fd_content_generation           = content_generation;
        resource->exported_fd.last_written_content_generation = content_generation;
        resource->exported_fd.detached_from_surface           = false;
        resource->exported_fd.role                            = fd.valid ? role : VkvvExportRole::None;
    }

    void mark_export_fd_written(ExportResource* resource, uint64_t content_generation, VkvvExportRole role) {
        if (resource == nullptr || !resource->exported_fd.fd_exported) {
            return;
        }
        resource->exported_fd.fd_content_generation           = content_generation;
        resource->exported_fd.last_written_content_generation = content_generation;
        resource->exported_fd.may_be_sampled_by_client        = content_generation != 0 && export_role_may_be_sampled_by_client(role);
        resource->exported_fd.detached_from_surface           = false;
        resource->exported_fd.role                            = content_generation != 0 ? role : VkvvExportRole::None;
    }

    void mark_export_fd_detached(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->exported_fd.detached_from_surface    = true;
        resource->exported_fd.may_be_sampled_by_client = false;
    }

    void trace_export_fd_lifetime(const SurfaceResource* owner, const ExportResource* resource, const char* action, uint64_t generation_at_action,
                                  bool may_be_sampled_by_client) {
        if (resource == nullptr) {
            return;
        }
        const VASurfaceID surface_id = owner != nullptr ? owner->surface_id : resource->owner_surface_id;
        const uint64_t    driver     = owner != nullptr ? owner->driver_instance_id : resource->driver_instance_id;
        const uint64_t    stream     = owner != nullptr ? owner->stream_id : resource->stream_id;
        const auto        codec      = owner != nullptr ? owner->codec_operation : resource->codec_operation;
        const uint64_t    content    = owner != nullptr ? owner->content_generation : resource->content_generation;
        const uint64_t    fd_dev     = resource->exported_fd.fd_exported ? resource->exported_fd.fd_dev : resource->fd_dev;
        const uint64_t    fd_ino     = resource->exported_fd.fd_exported ? resource->exported_fd.fd_ino : resource->fd_ino;
        VKVV_TRACE("export-fd-lifetime",
                   "event_action=%s surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu fd_content_gen=%llu present_gen=%llu "
                   "predecode_quarantined=%u may_be_sampled_by_client=%u generation_at_action=%llu export_role=%s",
                   action != nullptr ? action : "unknown", surface_id, static_cast<unsigned long long>(driver), static_cast<unsigned long long>(stream), codec,
                   static_cast<unsigned long long>(fd_dev), static_cast<unsigned long long>(fd_ino), static_cast<unsigned long long>(content),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(resource)), static_cast<unsigned long long>(resource->present_generation),
                   resource->predecode_quarantined ? 1U : 0U, may_be_sampled_by_client ? 1U : 0U, static_cast<unsigned long long>(generation_at_action),
                   vkvv_export_role_name(export_resource_fd_role(resource)));
    }

    void trace_export_role_lifecycle(const SurfaceResource* owner, const ExportResource* resource, const char* action, bool refresh_export) {
        if (resource == nullptr) {
            return;
        }
        const VASurfaceID surface_id = owner != nullptr ? owner->surface_id : resource->owner_surface_id;
        const uint64_t    driver     = owner != nullptr ? owner->driver_instance_id : resource->driver_instance_id;
        const uint64_t    stream     = owner != nullptr ? owner->stream_id : resource->stream_id;
        const auto        codec      = owner != nullptr ? owner->codec_operation : resource->codec_operation;
        const uint64_t    content    = owner != nullptr ? owner->content_generation : resource->content_generation;
        VKVV_TRACE("export-role-lifecycle",
                   "action=%s surface=%u driver=%llu stream=%llu codec=0x%x export_role=%s fd_exported=%u fd_dev=%llu fd_ino=%llu content_gen=%llu "
                   "fd_content_gen=%llu last_written_content_gen=%llu present_gen=%llu predecode_exported=%u predecode_quarantined=%u had_va_begin=%u "
                   "had_decode_submit=%u had_visible_decode=%u may_be_sampled_by_client=%u detached_from_surface=%u refresh_export=%u",
                   action != nullptr ? action : "unknown", surface_id, static_cast<unsigned long long>(driver), static_cast<unsigned long long>(stream), codec,
                   vkvv_export_role_name(export_resource_fd_role(resource)), resource->exported_fd.fd_exported ? 1U : 0U,
                   static_cast<unsigned long long>(resource->exported_fd.fd_exported ? resource->exported_fd.fd_dev : resource->fd_dev),
                   static_cast<unsigned long long>(resource->exported_fd.fd_exported ? resource->exported_fd.fd_ino : resource->fd_ino),
                   static_cast<unsigned long long>(content), static_cast<unsigned long long>(export_resource_fd_content_generation(resource)),
                   static_cast<unsigned long long>(resource->exported_fd.last_written_content_generation), static_cast<unsigned long long>(resource->present_generation),
                   resource->predecode_exported ? 1U : 0U, resource->predecode_quarantined ? 1U : 0U, resource->predecode_had_va_begin ? 1U : 0U,
                   resource->predecode_had_decode_submit ? 1U : 0U, resource->predecode_had_visible_decode ? 1U : 0U,
                   export_resource_fd_may_be_sampled_by_client(resource) ? 1U : 0U, resource->exported_fd.detached_from_surface ? 1U : 0U,
                   refresh_export ? 1U : 0U);
    }

    void trace_predecode_quarantine_outcome(const SurfaceResource* owner, const ExportResource* resource, const char* outcome, const char* reason, bool returned_fd) {
        if (resource == nullptr) {
            return;
        }
        const uint64_t now_ms  = monotonic_ms();
        const uint64_t age_ms  = resource->predecode_quarantine_enter_ms != 0 && now_ms >= resource->predecode_quarantine_enter_ms ?
             now_ms - resource->predecode_quarantine_enter_ms :
             0;
        const VASurfaceID surface_id = owner != nullptr ? owner->surface_id : resource->owner_surface_id;
        const uint64_t    stream     = owner != nullptr ? owner->stream_id : resource->stream_id;
        const auto        codec      = owner != nullptr ? owner->codec_operation : resource->codec_operation;
        const uint64_t    content    = owner != nullptr ? owner->content_generation : resource->content_generation;
        const bool        decoded    = content != 0;
        const uint64_t    fd_dev     = resource->predecode_fd_dev != 0 ? resource->predecode_fd_dev : resource->fd_dev;
        const uint64_t    fd_ino     = resource->predecode_fd_ino != 0 ? resource->predecode_fd_ino : resource->fd_ino;
        VKVV_TRACE("predecode-quarantine-outcome",
                   "surface=%u fd_dev=%llu fd_ino=%llu stream=%llu codec=0x%x age_ms=%llu content_gen=%llu fd_content_gen=%llu decoded=%u had_va_begin=%u "
                   "had_decode_submit=%u had_visible_decode=%u may_be_sampled_by_client=%u export_role=%s outcome=%s reason=%s returned_fd=%u",
                   surface_id, static_cast<unsigned long long>(fd_dev), static_cast<unsigned long long>(fd_ino), static_cast<unsigned long long>(stream), codec,
                   static_cast<unsigned long long>(age_ms), static_cast<unsigned long long>(content),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(resource)), decoded ? 1U : 0U, resource->predecode_had_va_begin ? 1U : 0U,
                   resource->predecode_had_decode_submit ? 1U : 0U, resource->predecode_had_visible_decode ? 1U : 0U,
                   export_resource_fd_may_be_sampled_by_client(resource) ? 1U : 0U, vkvv_export_role_name(export_resource_fd_role(resource)), outcome != nullptr ? outcome : "unknown",
                   reason != nullptr ? reason : "none", returned_fd ? 1U : 0U);
    }

    void mark_export_predecode_nonpresentable(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        clear_export_present_state(resource);
        resource->present_source = VkvvExportPresentSource::PredecodeBacking;
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
        resource->neutral_backing      = false;
        resource->seed_source_surface_id = VA_INVALID_ID;
        resource->seed_source_generation = 0;
        resource->seed_pixel_proof_valid = false;
    }

    void clear_predecode_quarantine_state(ExportResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->predecode_quarantined = false;
        resource->predecode_fd_dev      = 0;
        resource->predecode_fd_ino      = 0;
        resource->predecode_generation  = 0;
        resource->predecode_quarantine_enter_ms = 0;
        resource->predecode_had_va_begin        = false;
        resource->predecode_had_decode_submit   = false;
        resource->predecode_had_visible_decode  = false;
    }

    void enter_predecode_quarantine(const SurfaceResource* owner, ExportResource* resource) {
        if (owner == nullptr || resource == nullptr) {
            return;
        }
        resource->predecode_quarantined = true;
        resource->predecode_fd_dev      = resource->fd_dev;
        resource->predecode_fd_ino      = resource->fd_ino;
        resource->predecode_generation  = resource->content_generation;
        resource->predecode_quarantine_enter_ms = monotonic_ms();
        resource->predecode_had_va_begin        = false;
        resource->predecode_had_decode_submit   = false;
        resource->predecode_had_visible_decode  = false;
        VKVV_TRACE("predecode-quarantine-enter",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu presentable=0 published_visible=0 predecode_exported=%u "
                   "predecode_quarantined=1 export_role=%s",
                   owner->surface_id, static_cast<unsigned long long>(owner->driver_instance_id), static_cast<unsigned long long>(owner->stream_id), owner->codec_operation,
                   static_cast<unsigned long long>(resource->predecode_fd_dev), static_cast<unsigned long long>(resource->predecode_fd_ino),
                   static_cast<unsigned long long>(resource->predecode_generation), resource->predecode_exported ? 1U : 0U,
                   vkvv_export_role_name(export_resource_fd_role(resource)));
        trace_export_role_lifecycle(owner, resource, "quarantine-enter", false);
        trace_export_fd_lifetime(owner, resource, "quarantine-enter", resource->predecode_generation, export_resource_fd_may_be_sampled_by_client(resource));
    }

    void exit_predecode_quarantine(const SurfaceResource* owner, ExportResource* resource, bool release_done) {
        if (resource == nullptr || !resource->predecode_quarantined) {
            return;
        }
        const VASurfaceID surface_id = owner != nullptr ? owner->surface_id : resource->owner_surface_id;
        const uint64_t    driver     = owner != nullptr ? owner->driver_instance_id : resource->driver_instance_id;
        const uint64_t    stream     = owner != nullptr ? owner->stream_id : resource->stream_id;
        const auto        codec      = owner != nullptr ? owner->codec_operation : resource->codec_operation;
        const uint64_t    content    = owner != nullptr ? owner->content_generation : resource->content_generation;
        const uint64_t    fd_dev     = resource->predecode_fd_dev;
        const uint64_t    fd_ino     = resource->predecode_fd_ino;
        trace_predecode_quarantine_outcome(owner, resource, resource->predecode_seeded ? "seed-exit" : "decoded-exit", "valid-pixels", true);
        trace_export_role_lifecycle(owner, resource, "quarantine-exit", release_done);
        trace_export_fd_lifetime(owner, resource, "quarantine-exit", content, export_resource_fd_may_be_sampled_by_client(resource));
        clear_predecode_quarantine_state(resource);
        VKVV_TRACE("predecode-quarantine-exit",
                   "surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu present_gen=%llu release_done=%u predecode_quarantined=0 export_role=%s",
                   surface_id, static_cast<unsigned long long>(driver), static_cast<unsigned long long>(stream), codec, static_cast<unsigned long long>(fd_dev),
                   static_cast<unsigned long long>(fd_ino), static_cast<unsigned long long>(content), static_cast<unsigned long long>(resource->present_generation),
                   release_done ? 1U : 0U, vkvv_export_role_name(export_resource_fd_role(resource)));
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
        VKVV_TRACE("external-sync-proof",
                   "surface=%u fd_dev=%llu fd_ino=%llu stream=%llu codec=0x%x content_gen=%llu copy_done=%u fence_waited=1 release_mode=%s release_required=%u release_done=%u "
                   "acquire_required=%u acquire_done=%u old_layout=%d new_layout=%d src_queue_family=%u dst_queue_family=%u sync_fd=-1 semaphore_exported=0 present_crc_after_release=0x%llx",
                   owner->surface_id, static_cast<unsigned long long>(resource->fd_dev), static_cast<unsigned long long>(resource->fd_ino),
                   static_cast<unsigned long long>(owner->stream_id), owner->codec_operation, static_cast<unsigned long long>(owner->content_generation),
                   resource->content_generation == owner->content_generation ? 1U : 0U, vkvv_external_release_mode_name(sync.release_mode),
                   sync.external_release_required ? 1U : 0U, sync.external_release_done ? 1U : 0U, sync.external_acquire_required ? 1U : 0U,
                   sync.external_acquire_done ? 1U : 0U, old_layout, new_layout, sync.src_queue_family, sync.dst_queue_family,
                   static_cast<unsigned long long>(owner->present_pixel_proof_valid ? owner->present_pixel_crc : 0));
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
        resource->import_output_copy_target = false;
        resource->import_output_copy_done   = false;
    }

    void clear_surface_direct_import_present_state(SurfaceResource* resource) {
        if (resource == nullptr) {
            return;
        }
        resource->direct_import_presentable      = false;
        resource->decode_image_is_imported_image = false;
        resource->import_output_copy_done        = false;
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
