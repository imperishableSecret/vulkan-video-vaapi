#include "vulkan/export/internal.h"
#include "va/surface_import.h"
#include "telemetry.h"

#include <algorithm>
#include <chrono>
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

    uint64_t monotonic_us() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    struct Av1VisiblePublishCadenceTrace {
        uint64_t publish_monotonic_us                 = 0;
        uint64_t delta_visible_us                     = 0;
        bool     previous_visible_valid               = false;
        uint64_t previous_visible_frame_sequence      = 0;
        uint32_t previous_visible_order_hint          = 0;
        VASurfaceID previous_visible_surface          = VA_INVALID_ID;
        uint64_t previous_visible_content_generation  = 0;
        uint64_t previous_visible_fd_dev              = 0;
        uint64_t previous_visible_fd_ino              = 0;
        uint64_t previous_visible_fd_content_gen      = 0;
        uint64_t previous_visible_present_generation  = 0;
        uint64_t previous_visible_pixel_crc           = 0;
        uint64_t visible_frame_sequence_gap           = 0;
        int64_t  visible_order_hint_delta             = 0;
        bool     same_fd_as_previous                  = false;
        bool     same_surface_as_previous             = false;
        uint64_t current_fd_dev                       = 0;
        uint64_t current_fd_ino                       = 0;
        uint64_t current_fd_content_gen               = 0;
        uint64_t current_present_generation           = 0;
    };

    uint64_t av1_visible_fd_dev(const SurfaceResource* resource, bool import_published) {
        if (resource == nullptr) {
            return 0;
        }
        if (import_published) {
            return resource->import_fd_dev != 0 ? resource->import_fd_dev : resource->import.fd.dev;
        }
        const ExportResource& exported = resource->export_resource;
        return exported.exported_fd.fd_exported ? exported.exported_fd.fd_dev : exported.fd_dev;
    }

    uint64_t av1_visible_fd_ino(const SurfaceResource* resource, bool import_published) {
        if (resource == nullptr) {
            return 0;
        }
        if (import_published) {
            return resource->import_fd_ino != 0 ? resource->import_fd_ino : resource->import.fd.ino;
        }
        const ExportResource& exported = resource->export_resource;
        return exported.exported_fd.fd_exported ? exported.exported_fd.fd_ino : exported.fd_ino;
    }

    uint64_t av1_visible_fd_content_generation(const SurfaceResource* resource, bool import_published) {
        if (resource == nullptr) {
            return 0;
        }
        if (import_published) {
            return resource->import_present_generation;
        }
        return export_resource_fd_content_generation(&resource->export_resource);
    }

    Av1VisiblePublishCadenceTrace record_av1_visible_publish_cadence(VulkanRuntime* runtime, const SurfaceResource* resource, bool import_published, bool published) {
        Av1VisiblePublishCadenceTrace trace{};
        trace.publish_monotonic_us       = monotonic_us();
        trace.current_fd_dev             = av1_visible_fd_dev(resource, import_published);
        trace.current_fd_ino             = av1_visible_fd_ino(resource, import_published);
        trace.current_fd_content_gen     = av1_visible_fd_content_generation(resource, import_published);
        trace.current_present_generation = resource != nullptr ? resource->export_resource.present_generation : 0;
        if (runtime == nullptr || resource == nullptr) {
            return trace;
        }

        const Av1VisiblePublishCadence& previous = runtime->av1_visible_publish_cadence;
        trace.previous_visible_valid             = previous.valid && previous.driver_instance_id == resource->driver_instance_id &&
            previous.stream_id == resource->stream_id && previous.codec_operation == resource->codec_operation;
        if (trace.previous_visible_valid) {
            trace.previous_visible_frame_sequence     = previous.frame_sequence;
            trace.previous_visible_order_hint         = previous.order_hint;
            trace.previous_visible_surface            = previous.surface_id;
            trace.previous_visible_content_generation = previous.content_generation;
            trace.previous_visible_fd_dev             = previous.fd_dev;
            trace.previous_visible_fd_ino             = previous.fd_ino;
            trace.previous_visible_fd_content_gen     = previous.fd_content_generation;
            trace.previous_visible_present_generation = previous.present_generation;
            trace.previous_visible_pixel_crc          = previous.pixel_crc;
            trace.delta_visible_us = trace.publish_monotonic_us >= previous.monotonic_us ? trace.publish_monotonic_us - previous.monotonic_us : 0;
            trace.visible_frame_sequence_gap =
                resource->av1_frame_sequence >= previous.frame_sequence ? resource->av1_frame_sequence - previous.frame_sequence : 0;
            trace.visible_order_hint_delta = static_cast<int64_t>(resource->av1_order_hint) - static_cast<int64_t>(previous.order_hint);
            trace.same_fd_as_previous      = trace.current_fd_dev != 0 && trace.current_fd_dev == previous.fd_dev && trace.current_fd_ino == previous.fd_ino;
            trace.same_surface_as_previous = resource->surface_id == previous.surface_id;
        }

        if (published) {
            Av1VisiblePublishCadence& current = runtime->av1_visible_publish_cadence;
            current.valid                     = true;
            current.monotonic_us              = trace.publish_monotonic_us;
            current.driver_instance_id        = resource->driver_instance_id;
            current.stream_id                 = resource->stream_id;
            current.codec_operation           = resource->codec_operation;
            current.surface_id                = resource->surface_id;
            current.frame_sequence            = resource->av1_frame_sequence;
            current.order_hint                = resource->av1_order_hint;
            current.content_generation        = resource->content_generation;
            current.fd_dev                    = trace.current_fd_dev;
            current.fd_ino                    = trace.current_fd_ino;
            current.fd_content_generation     = trace.current_fd_content_gen;
            current.present_generation        = trace.current_present_generation;
            current.pixel_crc                 = resource->present_pixel_crc;
        }
        return trace;
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

    struct ExportPendingSnapshot {
        bool       found          = false;
        CommandUse use            = CommandUse::Idle;
        VASurfaceID surface       = VA_INVALID_ID;
        bool       refresh_export = false;
        char       operation[64]{};
    };

    ExportPendingSnapshot pending_export_snapshot(VulkanRuntime* runtime, const VkvvSurface* surface) {
        ExportPendingSnapshot snapshot{};
        if (runtime == nullptr || surface == nullptr) {
            return snapshot;
        }
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        for (const CommandSlot& slot : runtime->command_slots) {
            if (slot.pending.surface != surface) {
                continue;
            }
            snapshot.found          = true;
            snapshot.use            = slot.pending.use;
            snapshot.surface        = slot.pending.surface != nullptr ? slot.pending.surface->id : VA_INVALID_ID;
            snapshot.refresh_export = slot.pending.refresh_export;
            std::snprintf(snapshot.operation, sizeof(snapshot.operation), "%s", slot.pending.operation[0] != '\0' ? slot.pending.operation : command_use_name(slot.pending.use));
            return snapshot;
        }
        return snapshot;
    }

    const char* export_decision_for_source(VkvvExportPixelSource source, bool drained) {
        if (drained) {
            return "drain-and-return";
        }
        switch (source) {
            case VkvvExportPixelSource::DecodedContent: return "return-decoded";
            case VkvvExportPixelSource::StreamLocalSeed: return "return-seed";
            case VkvvExportPixelSource::Placeholder: return "return-placeholder";
            case VkvvExportPixelSource::RetainedUnknown: return "return-retained";
            default: return "fail";
        }
    }

    bool export_source_has_decoded_pixels(const VkvvSurface* surface, const SurfaceResource* owner, const ExportResource* resource,
                                          VkvvExportPixelSource pixel_source) {
        if (surface == nullptr || !surface->decoded || owner == nullptr || owner->content_generation == 0 ||
            pixel_source != VkvvExportPixelSource::DecodedContent) {
            return false;
        }
        if (resource == nullptr) {
            return true;
        }
        return resource->content_generation == owner->content_generation;
    }

    bool export_source_has_seed_pixels(const ExportResource* resource, VkvvExportPixelSource pixel_source) {
        if (resource == nullptr || pixel_source != VkvvExportPixelSource::StreamLocalSeed) {
            return false;
        }
        const bool seed_proof_required = export_pixel_proof_enabled();
        if (resource->seed_source_generation == 0 || resource->seed_source_surface_id == VA_INVALID_ID ||
            (seed_proof_required && !resource->seed_pixel_proof_valid)) {
            return false;
        }
        const bool returned_fd_seed_current =
            resource->exported_fd.fd_exported && export_resource_fd_content_generation(resource) == resource->seed_source_generation;
        const bool fresh_unreturned_seed = !resource->exported_fd.fd_exported && resource->predecode_seeded;
        return returned_fd_seed_current || fresh_unreturned_seed;
    }

    bool export_source_is_placeholder(const ExportResource* resource, VkvvExportPixelSource pixel_source) {
        return pixel_source == VkvvExportPixelSource::Placeholder || (resource != nullptr && resource->neutral_backing);
    }

    bool active_domain_predecode_pool_export_candidate(const VkvvSurface* surface, const SurfaceResource* resource) {
        return surface != nullptr && resource != nullptr && !surface->decoded && resource->content_generation == 0 && surface->stream_id != 0 &&
            surface->codec_operation != 0 && resource->stream_id == surface->stream_id && resource->codec_operation == surface->codec_operation;
    }

    bool allow_exact_predecode_pool_backing_export(const VkvvSurface* surface, const SurfaceResource* resource, const ExportResource* exported_shadow,
                                                   VkvvExportPixelSource pixel_source) {
        return active_domain_predecode_pool_export_candidate(surface, resource) && exported_shadow == &resource->export_resource &&
            export_source_is_placeholder(exported_shadow, pixel_source) && exported_shadow->content_generation == 0 && !exported_shadow->predecode_seeded &&
            exported_shadow->seed_source_surface_id == VA_INVALID_ID && exported_shadow->seed_source_generation == 0 && !exported_shadow->seed_pixel_proof_valid &&
            exported_shadow->owner_surface_id == surface->id && exported_shadow->stream_id == resource->stream_id &&
            exported_shadow->codec_operation == resource->codec_operation;
    }

    bool predecode_no_seed_export_matches_compat_probe_policy(const VkvvSurface* surface, const SurfaceResource* resource) {
        if (surface == nullptr || resource == nullptr) {
            return false;
        }
        // Compatibility fallback for observed browser predecode probes that
        // immediately sample small allocation-only exports. This is not a
        // content-validity rule; replace it only with telemetry-backed state.
        return surface->width <= 960 && surface->height <= 540 && resource->coded_extent.width <= 960 && resource->coded_extent.height <= 544;
    }

    struct ExportAdmissionDecision {
        VkvvExportPixelSource pixel_source                    = VkvvExportPixelSource::None;
        VkvvExportRole        role                            = VkvvExportRole::None;
        bool                  valid_decoded_pixels_available  = false;
        bool                  valid_seed_available            = false;
        bool                  valid_transition_hold_available = false;
        bool                  placeholder_available           = false;
        bool                  active_predecode_candidate      = false;
        bool                  exported_shadow_is_owner        = false;
        bool                  exact_predecode_pool_backing  = false;
        bool                  predecode_backing_export        = false;
        bool                  no_seed_predecode_backing       = false;
        bool                  no_seed_predecode_probe_export  = false;
        bool                  sampleable_placeholder_export   = false;
        bool                  may_return_fd                   = true;
        VAStatus              status                          = VA_STATUS_SUCCESS;
        const char*           failure_decision                = nullptr;
        const char*           failure_reason                  = nullptr;
    };

    ExportAdmissionDecision decide_export_admission(const VkvvSurface* surface, const SurfaceResource* resource, const ExportResource* exported_shadow,
                                                    bool export_request_readable) {
        ExportAdmissionDecision decision{};
        decision.pixel_source =
            exported_shadow != nullptr ? export_pixel_source_for_resource(resource, exported_shadow) :
                                         (surface != nullptr && resource != nullptr && surface->decoded && resource->content_generation != 0 ?
                                              VkvvExportPixelSource::DecodedContent :
                                              VkvvExportPixelSource::None);
        decision.valid_decoded_pixels_available = export_source_has_decoded_pixels(surface, resource, exported_shadow, decision.pixel_source);
        decision.valid_seed_available           = export_source_has_seed_pixels(exported_shadow, decision.pixel_source);
        decision.valid_transition_hold_available = export_resource_is_transition_hold_for_surface(resource, exported_shadow) &&
            decision.pixel_source == VkvvExportPixelSource::RetainedUnknown;
        decision.placeholder_available = export_source_is_placeholder(exported_shadow, decision.pixel_source);
        decision.active_predecode_candidate = active_domain_predecode_pool_export_candidate(surface, resource);
        decision.exported_shadow_is_owner   = resource != nullptr && exported_shadow == &resource->export_resource;
        decision.exact_predecode_pool_backing =
            allow_exact_predecode_pool_backing_export(surface, resource, exported_shadow, decision.pixel_source);
        decision.predecode_backing_export = decision.exact_predecode_pool_backing;
        decision.no_seed_predecode_backing =
            export_request_readable && decision.predecode_backing_export && decision.placeholder_available && !decision.valid_decoded_pixels_available &&
            !decision.valid_seed_available;
        decision.no_seed_predecode_probe_export = decision.no_seed_predecode_backing && predecode_no_seed_export_matches_compat_probe_policy(surface, resource);
        decision.sampleable_placeholder_export  = export_request_readable && decision.placeholder_available && !decision.predecode_backing_export;
        decision.role = decision.predecode_backing_export ? VkvvExportRole::PredecodeBacking :
            (decision.valid_transition_hold_available ? VkvvExportRole::TransitionHold :
                 (decision.valid_seed_available ? VkvvExportRole::PixelProvenSeed :
                      (decision.valid_decoded_pixels_available ||
                               (surface != nullptr && resource != nullptr && surface->decoded && resource->content_generation != 0) ?
                           VkvvExportRole::DecodedPixels :
                           VkvvExportRole::None)));

        if (decision.no_seed_predecode_probe_export) {
            decision.may_return_fd    = false;
            decision.status           = VA_STATUS_ERROR_OPERATION_FAILED;
            decision.failure_decision = "fail";
            decision.failure_reason   = "sampleable-no-seed-predecode-probe";
        } else if (decision.sampleable_placeholder_export) {
            decision.may_return_fd    = false;
            decision.status           = VA_STATUS_ERROR_OPERATION_FAILED;
            decision.failure_decision = "fail";
            decision.failure_reason   = "sampleable-placeholder-not-presentable";
        } else if (!decision.valid_decoded_pixels_available && !decision.valid_seed_available && !decision.valid_transition_hold_available &&
                   !decision.predecode_backing_export) {
            decision.may_return_fd    = false;
            decision.status           = VA_STATUS_ERROR_OPERATION_FAILED;
            decision.failure_decision = "fail";
            decision.failure_reason   = "no-valid-decoded-or-seed-pixels";
        }
        return decision;
    }

    void trace_direct_export_fd_lifetime(const VkvvSurface* surface, const SurfaceResource* resource, const VkvvFdIdentity& fd_stat, const char* action,
                                         uint64_t generation_at_action, bool may_be_sampled_by_client) {
        if (surface == nullptr || resource == nullptr) {
            return;
        }
        const VkvvExportRole role = may_be_sampled_by_client && resource->content_generation != 0 ? VkvvExportRole::DecodedPixels : VkvvExportRole::None;
        VKVV_TRACE("export-fd-lifetime",
                   "event_action=%s surface=%u driver=%llu stream=%llu codec=0x%x fd_dev=%llu fd_ino=%llu content_gen=%llu fd_content_gen=%llu present_gen=0 "
                   "predecode_quarantined=0 may_be_sampled_by_client=%u generation_at_action=%llu export_role=%s",
                   action != nullptr ? action : "unknown", surface->id, static_cast<unsigned long long>(resource->driver_instance_id),
                   static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(fd_stat.dev),
                   static_cast<unsigned long long>(fd_stat.ino), static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(resource->content_generation), may_be_sampled_by_client ? 1U : 0U,
                   static_cast<unsigned long long>(generation_at_action), vkvv_export_role_name(role));
    }

    bool trace_decode_shadow_coherence_check(const SurfaceResource* resource, bool refresh_export, bool display_visible, const char* action) {
        if (resource == nullptr || resource->export_resource.image == VK_NULL_HANDLE) {
            return true;
        }

        const bool needs_decode_shadow = resource->content_generation != 0 && resource->export_resource.exported;
        const bool coherent            = !needs_decode_shadow || surface_resource_has_current_decode_shadow(resource);
        VKVV_TRACE("decode-shadow-coherence-check",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu refresh_export=%u display_visible=%u present_pinned=%u present_shadow_gen=%llu "
                   "private_shadow_gen=%llu decode_shadow_gen=%llu coherent=%u action=%s",
                   resource->surface_id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                   resource->codec_operation, static_cast<unsigned long long>(resource->content_generation), refresh_export ? 1U : 0U, display_visible ? 1U : 0U,
                   resource->export_resource.present_pinned ? 1U : 0U, static_cast<unsigned long long>(resource->export_resource.content_generation),
                   static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                   static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation), coherent ? 1U : 0U, action != nullptr ? action : "unknown");
        if (!coherent) {
            VKVV_TRACE("invalid-stale-private-decode-shadow",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu decode_shadow_gen=%llu exported=%u "
                       "shadow_exported=%u action=%s",
                       resource->surface_id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                       resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation),
                       static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                       static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation), resource->exported ? 1U : 0U,
                       resource->export_resource.exported ? 1U : 0U, action != nullptr ? action : "unknown");
        }
        return coherent;
    }

    VkvvExportPresentSource visible_present_source(const SurfaceResource* resource) {
        if (surface_resource_uses_av1_decode(resource) && resource->av1_visible_output_trace_valid && resource->av1_visible_show_existing_frame) {
            return VkvvExportPresentSource::ShowExisting;
        }
        return VkvvExportPresentSource::VisibleRefresh;
    }

    void trace_av1_publication_fingerprint(VkvvSurface* surface, SurfaceResource* resource, bool refresh_export, bool shadow_published, bool import_published, bool published,
                                           const Av1VisiblePublishCadenceTrace& cadence) {
        if (surface == nullptr || resource == nullptr || !surface_resource_uses_av1_decode(resource) || !refresh_export || !av1_trace_publication_enabled()) {
            return;
        }

        const uint32_t fingerprint_level  = av1_fingerprint_level();
        const bool     crc_valid          = fingerprint_level > 0;
        const uint64_t published_crc      = crc_valid ? av1_publish_metadata_fingerprint(resource) : 0;
        const bool     matches_decode     = crc_valid && resource->av1_decode_fingerprint != 0 && published_crc == resource->av1_decode_fingerprint;
        const bool     matches_previous   = crc_valid && resource->av1_previous_visible_fingerprint != 0 && published_crc == resource->av1_previous_visible_fingerprint;
        const bool     pixel_proof_valid  = resource->decode_pixel_proof_valid && resource->present_pixel_proof_valid;
        const bool     pixel_matches_decode   = pixel_proof_valid && resource->present_pixel_matches_decode;
        const bool     pixel_matches_previous = pixel_proof_valid && resource->present_pixel_matches_previous;
        const bool     effective_matches_decode = pixel_proof_valid ? pixel_matches_decode : matches_decode;
        const bool     effective_matches_previous = pixel_proof_valid ? pixel_matches_previous : matches_previous;
        const char*    proof_source = pixel_proof_valid ? "pixel" : (crc_valid ? "metadata" : "none");
        const char*    published_path     = av1_published_path(resource, shadow_published, import_published);
        resource->av1_publish_fingerprint = published_crc;

        VKVV_TRACE("av1-publish-fingerprint",
                   "frame_seq=%llu surface=%u content_generation=%llu published_path=%s shadow_image_handle=0x%llx import_image_handle=0x0 exported=%u shadow_exported=%u "
                   "import_external=%u import_present_generation=%llu published_crc_valid=%u published_y_tl_crc=0 published_y_center_crc=0 published_y_br_crc=0 "
                   "published_uv_center_crc=0 published_combined_crc=0x%llx published_matches_decode=%u published_matches_previous_visible=%u fingerprint_level=%u "
                   "fingerprint_kind=metadata pixel_proof_valid=%u pixel_matches_decode=%u pixel_matches_previous_visible=%u decode_pixel_crc=0x%llx present_pixel_crc=0x%llx "
                   "metadata_matches_decode=%u metadata_matches_previous_visible=%u proof_source=%s",
                   static_cast<unsigned long long>(resource->av1_frame_sequence), surface->id, static_cast<unsigned long long>(resource->content_generation), published_path,
                   vkvv_trace_handle(resource->export_resource.image), resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                   resource->import.external ? 1U : 0U, static_cast<unsigned long long>(resource->import_present_generation), crc_valid ? 1U : 0U,
                   static_cast<unsigned long long>(published_crc), effective_matches_decode ? 1U : 0U, effective_matches_previous ? 1U : 0U, fingerprint_level,
                   pixel_proof_valid ? 1U : 0U, pixel_matches_decode ? 1U : 0U, pixel_matches_previous ? 1U : 0U,
                   static_cast<unsigned long long>(resource->decode_pixel_crc), static_cast<unsigned long long>(resource->present_pixel_crc), matches_decode ? 1U : 0U,
                   matches_previous ? 1U : 0U, proof_source);

        VKVV_TRACE("av1-visible-frame-identity",
                   "frame_seq=%llu surface=%u stream=%llu codec=0x%x content_generation=%llu order_hint=%u frame_type=%u tile_source=%s decode_crc=0x%llx published_crc=0x%llx "
                   "published_matches_decode=%u published_matches_previous_visible=%u output_published=%u proof_source=%s pixel_proof_valid=%u pixel_matches_decode=%u "
                   "pixel_matches_previous_visible=%u metadata_matches_decode=%u metadata_matches_previous_visible=%u decode_pixel_crc=0x%llx present_pixel_crc=0x%llx "
                   "publish_monotonic_us=%llu delta_visible_us=%llu fd_dev=%llu fd_ino=%llu fd_content_gen=%llu present_gen=%llu visible_frame_seq_gap=%llu "
                   "previous_visible_valid=%u previous_visible_frame_seq=%llu previous_visible_order_hint=%u previous_visible_surface=%u previous_visible_content_gen=%llu "
                   "previous_visible_fd_dev=%llu previous_visible_fd_ino=%llu previous_visible_fd_content_gen=%llu previous_visible_present_gen=%llu "
                   "previous_visible_pixel_crc=0x%llx same_fd_as_previous=%u same_surface_as_previous=%u",
                   static_cast<unsigned long long>(resource->av1_frame_sequence), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), resource->av1_order_hint, resource->av1_frame_type,
                   resource->av1_tile_source != nullptr ? resource->av1_tile_source : "unknown", static_cast<unsigned long long>(resource->av1_decode_fingerprint),
                   static_cast<unsigned long long>(published_crc), effective_matches_decode ? 1U : 0U, effective_matches_previous ? 1U : 0U, published ? 1U : 0U,
                   proof_source, pixel_proof_valid ? 1U : 0U, pixel_matches_decode ? 1U : 0U, pixel_matches_previous ? 1U : 0U, matches_decode ? 1U : 0U,
                   matches_previous ? 1U : 0U, static_cast<unsigned long long>(resource->decode_pixel_crc), static_cast<unsigned long long>(resource->present_pixel_crc),
                   static_cast<unsigned long long>(cadence.publish_monotonic_us), static_cast<unsigned long long>(cadence.delta_visible_us),
                   static_cast<unsigned long long>(cadence.current_fd_dev), static_cast<unsigned long long>(cadence.current_fd_ino),
                   static_cast<unsigned long long>(cadence.current_fd_content_gen), static_cast<unsigned long long>(cadence.current_present_generation),
                   static_cast<unsigned long long>(cadence.visible_frame_sequence_gap), cadence.previous_visible_valid ? 1U : 0U,
                   static_cast<unsigned long long>(cadence.previous_visible_frame_sequence), cadence.previous_visible_order_hint, cadence.previous_visible_surface,
                   static_cast<unsigned long long>(cadence.previous_visible_content_generation), static_cast<unsigned long long>(cadence.previous_visible_fd_dev),
                   static_cast<unsigned long long>(cadence.previous_visible_fd_ino), static_cast<unsigned long long>(cadence.previous_visible_fd_content_gen),
                   static_cast<unsigned long long>(cadence.previous_visible_present_generation), static_cast<unsigned long long>(cadence.previous_visible_pixel_crc),
                   cadence.same_fd_as_previous ? 1U : 0U, cadence.same_surface_as_previous ? 1U : 0U);

        const char* failure_stage  = "none";
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
        } else if (pixel_proof_valid && !pixel_matches_decode) {
            failure_stage  = "publish";
            failure_reason = "published-pixel-mismatch";
        } else if (!pixel_proof_valid && crc_valid && !matches_decode) {
            failure_stage  = "publish";
            failure_reason = "published-metadata-fingerprint-mismatch";
        }
        VKVV_TRACE("av1-visible-frame-audit",
                   "frame_seq=%llu surface=%u stream=%llu order_hint=%u frame_type=%u show_frame=%u show_existing_frame=%u refresh_frame_flags=0x%02x content_generation=%llu "
                   "tile_source=%s tile_count=%u tile_ranges_valid=%u tile_sum_size=%u setup_slot=%d target_dpb_slot=%d references_valid=%u reference_count=%u "
                   "decode_crc_valid=%u decode_crc=0x%llx published_path=%s published_crc_valid=%u published_crc=0x%llx published_matches_decode=%u "
                   "published_matches_previous_visible=%u output_published=%u proof_source=%s pixel_proof_valid=%u pixel_matches_decode=%u pixel_matches_previous_visible=%u "
                   "decode_pixel_crc=0x%llx present_pixel_crc=0x%llx metadata_matches_decode=%u metadata_matches_previous_visible=%u publish_monotonic_us=%llu "
                   "delta_visible_us=%llu fd_dev=%llu fd_ino=%llu fd_content_gen=%llu present_gen=%llu visible_frame_seq_gap=%llu visible_order_hint_delta=%lld "
                   "previous_visible_valid=%u previous_visible_frame_seq=%llu previous_visible_order_hint=%u previous_visible_surface=%u previous_visible_content_gen=%llu "
                   "previous_visible_fd_dev=%llu previous_visible_fd_ino=%llu previous_visible_fd_content_gen=%llu previous_visible_present_gen=%llu "
                   "previous_visible_pixel_crc=0x%llx same_fd_as_previous=%u same_surface_as_previous=%u failure_stage=%s failure_reason=%s",
                   static_cast<unsigned long long>(resource->av1_frame_sequence), surface->id, static_cast<unsigned long long>(resource->stream_id), resource->av1_order_hint,
                   resource->av1_frame_type, resource->av1_visible_show_frame ? 1U : 0U, resource->av1_visible_show_existing_frame ? 1U : 0U,
                   resource->av1_visible_refresh_frame_flags, static_cast<unsigned long long>(resource->content_generation),
                   resource->av1_tile_source != nullptr ? resource->av1_tile_source : "unknown", resource->av1_tile_count, resource->av1_tile_ranges_valid ? 1U : 0U,
                   resource->av1_tile_sum_size, resource->av1_setup_slot, resource->av1_target_dpb_slot, resource->av1_references_valid ? 1U : 0U, resource->av1_reference_count,
                   crc_valid ? 1U : 0U, static_cast<unsigned long long>(resource->av1_decode_fingerprint), published_path, crc_valid ? 1U : 0U,
                   static_cast<unsigned long long>(published_crc), effective_matches_decode ? 1U : 0U, effective_matches_previous ? 1U : 0U, published ? 1U : 0U,
                   proof_source, pixel_proof_valid ? 1U : 0U, pixel_matches_decode ? 1U : 0U, pixel_matches_previous ? 1U : 0U,
                   static_cast<unsigned long long>(resource->decode_pixel_crc), static_cast<unsigned long long>(resource->present_pixel_crc), matches_decode ? 1U : 0U,
                   matches_previous ? 1U : 0U, static_cast<unsigned long long>(cadence.publish_monotonic_us),
                   static_cast<unsigned long long>(cadence.delta_visible_us), static_cast<unsigned long long>(cadence.current_fd_dev),
                   static_cast<unsigned long long>(cadence.current_fd_ino), static_cast<unsigned long long>(cadence.current_fd_content_gen),
                   static_cast<unsigned long long>(cadence.current_present_generation), static_cast<unsigned long long>(cadence.visible_frame_sequence_gap),
                   static_cast<long long>(cadence.visible_order_hint_delta), cadence.previous_visible_valid ? 1U : 0U,
                   static_cast<unsigned long long>(cadence.previous_visible_frame_sequence), cadence.previous_visible_order_hint, cadence.previous_visible_surface,
                   static_cast<unsigned long long>(cadence.previous_visible_content_generation), static_cast<unsigned long long>(cadence.previous_visible_fd_dev),
                   static_cast<unsigned long long>(cadence.previous_visible_fd_ino), static_cast<unsigned long long>(cadence.previous_visible_fd_content_gen),
                   static_cast<unsigned long long>(cadence.previous_visible_present_generation), static_cast<unsigned long long>(cadence.previous_visible_pixel_crc),
                   cadence.same_fd_as_previous ? 1U : 0U, cadence.same_surface_as_previous ? 1U : 0U, failure_stage, failure_reason);

        if (published && crc_valid) {
            resource->av1_previous_visible_fingerprint = published_crc;
        }
    }

    void trace_av1_visible_output_check(VulkanRuntime* runtime, VkvvSurface* surface, SurfaceResource* resource, bool refresh_export) {
        if (surface == nullptr || resource == nullptr || !surface_resource_uses_av1_decode(resource) || !refresh_export || resource->content_generation == 0) {
            return;
        }

        const bool trace_valid      = resource->av1_visible_output_trace_valid;
        const bool shadow_current   = surface_resource_has_current_export_shadow(resource);
        const bool shadow_published = surface_resource_has_exported_shadow_output(resource);
        const bool import_published = surface_resource_has_direct_import_output(resource);
        const bool published        = shadow_published || import_published;
        const Av1VisiblePublishCadenceTrace cadence = record_av1_visible_publish_cadence(runtime, resource, import_published, published);
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
            VKVV_TRACE("av1-visible-frame-cadence",
                       "surface=%u stream=%llu codec=0x%x frame_seq=%llu order_hint=%u content_gen=%llu fd_dev=%llu fd_ino=%llu fd_content_gen=%llu present_gen=%llu "
                       "present_pixel_crc=0x%llx publish_monotonic_us=%llu delta_visible_us=%llu previous_visible_valid=%u previous_visible_frame_seq=%llu "
                       "previous_visible_order_hint=%u previous_visible_surface=%u previous_visible_content_gen=%llu previous_visible_fd_dev=%llu previous_visible_fd_ino=%llu "
                       "previous_visible_fd_content_gen=%llu previous_visible_present_gen=%llu previous_visible_pixel_crc=0x%llx visible_frame_seq_gap=%llu "
                       "visible_order_hint_delta=%lld same_fd_as_previous=%u same_surface_as_previous=%u published_path=%s",
                       surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->av1_frame_sequence), resource->av1_order_hint,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(cadence.current_fd_dev),
                       static_cast<unsigned long long>(cadence.current_fd_ino), static_cast<unsigned long long>(cadence.current_fd_content_gen),
                       static_cast<unsigned long long>(cadence.current_present_generation), static_cast<unsigned long long>(resource->present_pixel_crc),
                       static_cast<unsigned long long>(cadence.publish_monotonic_us), static_cast<unsigned long long>(cadence.delta_visible_us),
                       cadence.previous_visible_valid ? 1U : 0U, static_cast<unsigned long long>(cadence.previous_visible_frame_sequence),
                       cadence.previous_visible_order_hint, cadence.previous_visible_surface,
                       static_cast<unsigned long long>(cadence.previous_visible_content_generation), static_cast<unsigned long long>(cadence.previous_visible_fd_dev),
                       static_cast<unsigned long long>(cadence.previous_visible_fd_ino), static_cast<unsigned long long>(cadence.previous_visible_fd_content_gen),
                       static_cast<unsigned long long>(cadence.previous_visible_present_generation), static_cast<unsigned long long>(cadence.previous_visible_pixel_crc),
                       static_cast<unsigned long long>(cadence.visible_frame_sequence_gap), static_cast<long long>(cadence.visible_order_hint_delta),
                       cadence.same_fd_as_previous ? 1U : 0U, cadence.same_surface_as_previous ? 1U : 0U,
                       av1_published_path(resource, shadow_published, import_published));
            if (resource->import.external && import_published) {
                VKVV_TRACE("import-output-copy-enter",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_external=1 import_fd_valid=%u import_fd_dev=%llu import_fd_ino=%llu "
                           "import_fourcc=0x%x import_modifier_valid=%u import_modifier=0x%llx import_image_handle=0x%llx import_memory_handle=0x%llx decode_image_handle=0x%llx "
                           "shadow_image_handle=0x%llx copy_required=%u",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                           static_cast<unsigned long long>(resource->content_generation), resource->import.fd.valid ? 1U : 0U,
                           static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino), resource->va_fourcc,
                           resource->has_drm_format_modifier ? 1U : 0U, static_cast<unsigned long long>(resource->drm_format_modifier),
                           vkvv_trace_handle(resource->export_resource.image), vkvv_trace_handle(resource->export_resource.memory), vkvv_trace_handle(resource->image),
                           vkvv_trace_handle(resource->export_resource.image), resource->decode_image_is_imported_image ? 0U : 1U);
                VKVV_TRACE("import-output-copy-done",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_external=1 import_present_generation=%llu copy_done=%u layout_before=%d layout_after=%d",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                           static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->import_present_generation),
                           resource->decode_image_is_imported_image || resource->import_output_copy_done ? 1U : 0U, resource->layout, resource->layout);
                VKVV_TRACE("export-copy-proof",
                           "codec=0x%x surface=%u source_surface=%u target_surface=%u source_content_gen=%llu target_content_gen_before=%llu target_content_gen_after=%llu "
                           "source_shadow_gen=%llu target_shadow_gen_before=%llu target_shadow_gen_after=%llu copy_reason=%s refresh_export=1",
                           resource->codec_operation, surface->id, surface->id, surface->id, static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import_present_generation),
                           static_cast<unsigned long long>(resource->export_resource.content_generation), static_cast<unsigned long long>(resource->import_present_generation),
                           static_cast<unsigned long long>(resource->import_present_generation), vkvv_export_copy_reason_name(VkvvExportCopyReason::ImportOutput));
                VKVV_TRACE("import-output-release-barrier",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu release_barrier_done=%u queue_family_released=%u layout_before=%d layout_after=%d", surface->id,
                           static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           resource->import_present_barrier_done ? 1U : 0U, resource->import_present_barrier_done ? 1U : 0U, resource->layout, resource->layout);
                VKVV_TRACE("import-present-mark", "surface=%u stream=%llu codec=0x%x content_gen=%llu import_present_generation=%llu import_fd_dev=%llu import_fd_ino=%llu",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                           static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->import_present_generation),
                           static_cast<unsigned long long>(resource->import_fd_dev), static_cast<unsigned long long>(resource->import_fd_ino));
            }
            VKVV_TRACE("av1-visible-output-published",
                       "surface=%u stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu shadow_published=%u import_published=%u exported=%u shadow_exported=%u "
                       "import_external=%u direct_import_ok=%u import_present_generation=%llu import_fd_dev=%llu import_fd_ino=%llu decode_image=0x%llx import_image=0x%llx "
                       "copy_done=%u layout_released=%u queue_family_released=%u publish_monotonic_us=%llu delta_visible_us=%llu fd_dev=%llu fd_ino=%llu fd_content_gen=%llu "
                       "present_gen=%llu present_pixel_crc=0x%llx visible_frame_seq_gap=%llu previous_visible_valid=%u previous_visible_frame_seq=%llu "
                       "previous_visible_order_hint=%u previous_visible_surface=%u same_fd_as_previous=%u same_surface_as_previous=%u",
                       surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), shadow_published ? 1U : 0U, import_published ? 1U : 0U,
                       resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, resource->import.external ? 1U : 0U, import_published ? 1U : 0U,
                       static_cast<unsigned long long>(resource->import_present_generation), static_cast<unsigned long long>(resource->import.fd.dev),
                       static_cast<unsigned long long>(resource->import.fd.ino), vkvv_trace_handle(resource->image), vkvv_trace_handle(resource->export_resource.image),
                       resource->decode_image_is_imported_image || resource->import_output_copy_done ? 1U : 0U, import_published ? 1U : 0U, import_published ? 1U : 0U,
                       static_cast<unsigned long long>(cadence.publish_monotonic_us), static_cast<unsigned long long>(cadence.delta_visible_us),
                       static_cast<unsigned long long>(cadence.current_fd_dev), static_cast<unsigned long long>(cadence.current_fd_ino),
                       static_cast<unsigned long long>(cadence.current_fd_content_gen), static_cast<unsigned long long>(cadence.current_present_generation),
                       static_cast<unsigned long long>(resource->present_pixel_crc), static_cast<unsigned long long>(cadence.visible_frame_sequence_gap),
                       cadence.previous_visible_valid ? 1U : 0U, static_cast<unsigned long long>(cadence.previous_visible_frame_sequence),
                       cadence.previous_visible_order_hint, cadence.previous_visible_surface, cadence.same_fd_as_previous ? 1U : 0U,
                       cadence.same_surface_as_previous ? 1U : 0U);
        } else {
            if (resource->import.external) {
                VKVV_TRACE("import-output-copy-enter",
                           "surface=%u stream=%llu codec=0x%x content_gen=%llu import_external=1 import_fd_valid=%u import_fd_dev=%llu import_fd_ino=%llu "
                           "import_fourcc=0x%x import_modifier_valid=%u import_modifier=0x%llx import_image_handle=0x0 import_memory_handle=0x0 decode_image_handle=0x%llx "
                           "shadow_image_handle=0x%llx copy_required=1",
                           surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                           static_cast<unsigned long long>(resource->content_generation), resource->import.fd.valid ? 1U : 0U,
                           static_cast<unsigned long long>(resource->import.fd.dev), static_cast<unsigned long long>(resource->import.fd.ino), resource->va_fourcc,
                           resource->has_drm_format_modifier ? 1U : 0U, static_cast<unsigned long long>(resource->drm_format_modifier), vkvv_trace_handle(resource->image),
                           vkvv_trace_handle(resource->export_resource.image));
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
        trace_av1_publication_fingerprint(surface, resource, refresh_export, shadow_published, import_published, published, cadence);
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
    const uint64_t refresh_start_us = monotonic_us();
    if (resource->export_resource.image == VK_NULL_HANDLE && resource->import.external && resource->import.fd.valid) {
        (void)attach_imported_export_resource_by_fd(runtime, resource);
        if (resource->export_resource.image == VK_NULL_HANDLE && surface_resource_uses_av1_decode(resource) && resource->content_generation != 0) {
            (void)ensure_imported_output_resource(runtime, resource, reason, reason_size);
        }
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
            trace_av1_visible_output_check(runtime, surface, resource, refresh_export);
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
        const uint64_t refresh_no_backing_done_us = monotonic_us();
        VKVV_TRACE("export-refresh-no-backing",
                   "surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u content_gen=%llu seed_gen=%llu last_skip_gen=%llu last_display_gen=%llu retained=%zu "
                   "retained_mem=%llu refresh_start_monotonic_us=%llu refresh_total_us=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   refresh_export ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_seed_generation),
                   static_cast<unsigned long long>(resource->last_nondisplay_skip_generation), static_cast<unsigned long long>(resource->last_display_refresh_generation),
                   runtime_retained_export_count(runtime), static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)),
                   static_cast<unsigned long long>(refresh_start_us),
                   static_cast<unsigned long long>(refresh_no_backing_done_us >= refresh_start_us ? refresh_no_backing_done_us - refresh_start_us : 0));
        trace_av1_visible_output_check(runtime, surface, resource, refresh_export);
        return VA_STATUS_SUCCESS;
    }

    const ExportFormatInfo* format = export_format_for_surface(surface, resource, reason, reason_size);
    if (format == nullptr) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    const uint64_t drain_start_us = monotonic_us();
    VAStatus drain_status = drain_pending_surface_work_before_sync_command(runtime, surface, reason, reason_size);
    const uint64_t drain_done_us = monotonic_us();
    if (drain_status != VA_STATUS_SUCCESS) {
        return drain_status;
    }
    VKVV_TRACE("export-refresh-pre",
               "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu "
               "refresh_start_monotonic_us=%llu drain_us=%llu",
               surface->id,
               static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation), static_cast<unsigned long long>(refresh_start_us),
               static_cast<unsigned long long>(drain_done_us >= drain_start_us ? drain_done_us - drain_start_us : 0));

    if (!refresh_export) {
        const bool        predecode_before   = resource->export_resource.predecode_exported;
        const bool        seeded_before      = resource->export_resource.predecode_seeded;
        const VASurfaceID seed_source_before = resource->export_resource.seed_source_surface_id;
        const uint64_t    old_shadow_gen     = resource->export_resource.content_generation;
        resource->export_seed_generation     = 0;
        unregister_export_seed_resource(runtime, resource);
        const bool retired_predecode_export = resource->export_resource.predecode_exported || resource->export_resource.predecode_seeded ||
            resource->export_resource.seed_source_surface_id != VA_INVALID_ID || resource->export_resource.seed_source_generation != 0;
        const bool predecode_quarantined = resource->export_resource.predecode_quarantined;
        if (retired_predecode_export && !predecode_quarantined) {
            unregister_predecode_export_resource(runtime, &resource->export_resource);
            VKVV_TRACE("export-predecode-retire-nondisplay",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu seeded=%u seed_surface=%u seed_gen=%llu", surface->id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_seeded ? 1U : 0U,
                       resource->export_resource.seed_source_surface_id, static_cast<unsigned long long>(resource->export_resource.seed_source_generation));
        }
        if (predecode_quarantined) {
            mark_export_predecode_nonpresentable(&resource->export_resource);
        } else {
            clear_nondisplay_predecode_presentation_state(resource);
        }
        const bool has_exported_backing =
            resource->export_resource.image != VK_NULL_HANDLE && (resource->export_resource.exported || resource->import_output_copy_target);
        const bool fd_may_be_sampled    = export_resource_fd_may_be_sampled_by_client(&resource->export_resource);
        const bool already_current      = resource->content_generation != 0 && surface_resource_has_current_decode_shadow(resource);
        const bool exported_fd_stale    = fd_may_be_sampled && resource->content_generation != 0 && !export_resource_fd_fresh(resource);
        const bool predecode_quarantine_fd_needs_upgrade = predecode_quarantined && resource->content_generation != 0 &&
            resource->export_resource.exported_fd.fd_exported && resource->export_resource.exported_fd.fd_content_generation != resource->content_generation;
        const bool av1_imported_output_refresh = surface_resource_uses_av1_decode(resource) && resource->import_output_copy_target;
        const bool present_pinned_private_refresh =
            has_exported_backing && !already_current && !fd_may_be_sampled && !av1_imported_output_refresh && resource->export_resource.present_pinned &&
            resource->export_resource.client_visible_shadow;
        const bool  predecode_quarantine_private_refresh =
            has_exported_backing && !already_current && !fd_may_be_sampled && predecode_quarantined && !predecode_quarantine_fd_needs_upgrade;
        const char* action                               = "no-backing";
        bool        attempted_copy                       = false;
        bool        attempted_private_copy               = false;
        auto        trace_nondisplay_post_check          = [&](const char* check_action) {
            const bool shadow_stale = surface_resource_decode_shadow_stale(resource);
            VKVV_TRACE(
                "nondisplay-export-post-check",
                "codec=0x%x surface=%u refresh_export=0 content_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu decode_shadow_gen=%llu shadow_stale=%u exported=%u "
                "shadow_exported=%u predecode=%u seeded=%u present_pinned=%u presentable=%u present_gen=%llu decode_shadow_private_active=%u action=%s",
                resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->content_generation),
                static_cast<unsigned long long>(resource->export_resource.content_generation), static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation), shadow_stale ? 1U : 0U, resource->exported ? 1U : 0U,
                resource->export_resource.exported ? 1U : 0U, resource->export_resource.predecode_exported ? 1U : 0U, resource->export_resource.predecode_seeded ? 1U : 0U,
                resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U,
                static_cast<unsigned long long>(resource->export_resource.present_generation), resource->export_resource.decode_shadow_private_active ? 1U : 0U, check_action);
        };
        if (has_exported_backing) {
            if (predecode_quarantine_fd_needs_upgrade) {
                action         = "predecode-bootstrap-fd-upgrade";
                attempted_copy = true;
            } else if (already_current && !exported_fd_stale) {
                action = "already-current";
            } else if (exported_fd_stale) {
                action         = "exported-fd-refresh";
                attempted_copy = true;
            } else if (present_pinned_private_refresh || predecode_quarantine_private_refresh) {
                action                 = "private-shadow-refresh";
                attempted_private_copy = true;
            } else {
                action         = resource->export_resource.present_pinned ? "current-refresh-pinned-stabilize" : "current-refresh-unpinned";
                attempted_copy = true;
            }
        }
        VKVV_TRACE(
            "nondisplay-export-guard",
            "codec=0x%x surface=%u driver=%llu stream=%llu content_gen=%llu shadow_gen=%llu refresh_export=0 exported=%u shadow_exported=%u predecode_before=%u "
            "seeded_before=%u seed_source_before=%u action=%s attempted_seed=0 attempted_copy=%u attempted_private_copy=%u present_pinned=%u presentable=%u present_gen=%llu "
            "private_shadow_gen=%llu decode_shadow_gen=%llu",
            resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
            static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
            resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, predecode_before ? 1U : 0U, seeded_before ? 1U : 0U, seed_source_before, action,
            attempted_copy ? 1U : 0U, attempted_private_copy ? 1U : 0U, resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U,
            static_cast<unsigned long long>(resource->export_resource.present_generation), static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
            static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation));
        trace_exported_fd_freshness_check(resource, &resource->export_resource, false, false, attempted_copy ? "needs-refresh" : action);
        constexpr bool attempted_seed_or_copy = false;
        if (attempted_seed_or_copy) {
            VKVV_TRACE("invalid-nondisplay-export-mutation",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu exported=%u shadow_exported=%u predecode=%u seeded=%u reason=%s",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                       resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U, resource->export_resource.predecode_exported ? 1U : 0U,
                       resource->export_resource.predecode_seeded ? 1U : 0U, "nondisplay-refresh");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (attempted_copy) {
            const uint64_t fd_content_gen_before = export_resource_fd_content_generation(&resource->export_resource);
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
                       resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->driver_instance_id),
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(old_shadow_gen),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                       predecode_before ? 1U : 0U, seeded_before ? 1U : 0U, resource->export_resource.predecode_exported ? 1U : 0U,
                       resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U);
            VKVV_TRACE("nondisplay-exported-fd-refresh",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu fd_content_gen_before=%llu fd_content_gen_after=%llu refresh_export=0 "
                       "display_published=0 may_be_sampled_by_client=%u present_gen=%llu",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(fd_content_gen_before),
                       static_cast<unsigned long long>(export_resource_fd_content_generation(&resource->export_resource)),
                       export_resource_fd_may_be_sampled_by_client(&resource->export_resource) ? 1U : 0U,
                       static_cast<unsigned long long>(resource->export_resource.present_generation));
            trace_export_present_state(resource, &resource->export_resource, "nondisplay-current-refresh-unpinned", false, false);
            clear_private_decode_shadow_state(resource);
        } else if (attempted_private_copy) {
            const uint64_t present_shadow_before = resource->export_resource.content_generation;
            if (!copy_decode_to_private_shadow(runtime, resource, reason, reason_size)) {
                VKVV_TRACE(
                    "nondisplay-private-shadow-refresh",
                    "surface=%u codec=0x%x stream=%llu driver=%llu content_gen=%llu present_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu decode_shadow_gen=%llu "
                    "refresh_export=0 exported=%u shadow_exported=%u present_pinned=%u presentable=%u predecode_quarantined=%u client_visible_shadow_mutated=0 copy_status=failed",
                    surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->driver_instance_id),
                    static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.present_generation),
                    static_cast<unsigned long long>(resource->export_resource.content_generation),
                    static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                    static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation), resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                    resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U, resource->export_resource.predecode_quarantined ? 1U : 0U);
                trace_nondisplay_post_check("failed-private-shadow-refresh");
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            resource->export_seed_generation                 = 0;
            resource->last_nondisplay_skip_generation        = resource->content_generation;
            resource->last_nondisplay_skip_shadow_generation = resource->private_decode_shadow.content_generation;
            resource->last_nondisplay_skip_shadow_memory     = resource->private_decode_shadow.memory;
            if (resource->export_resource.content_generation != present_shadow_before) {
                VKVV_TRACE("invalid-nondisplay-present-mutation",
                           "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu present_gen=%llu exported=%u shadow_exported=%u "
                           "copy_reason=%s refresh_export=0",
                           surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                           resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(resource->export_resource.content_generation),
                           static_cast<unsigned long long>(resource->export_resource.present_generation), resource->exported ? 1U : 0U,
                           resource->export_resource.exported ? 1U : 0U, vkvv_export_copy_reason_name(VkvvExportCopyReason::NondisplayPrivateRefresh));
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            (void)trace_private_shadow_pixel_proof(runtime, resource, reason, reason_size);
            VKVV_TRACE(
                "nondisplay-private-shadow-refresh",
                "surface=%u codec=0x%x stream=%llu driver=%llu content_gen=%llu present_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu decode_shadow_gen=%llu "
                "refresh_export=0 exported=%u shadow_exported=%u present_pinned=%u presentable=%u predecode_quarantined=%u client_visible_shadow_mutated=0 copy_status=success",
                surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->driver_instance_id),
                static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.present_generation),
                static_cast<unsigned long long>(resource->export_resource.content_generation), static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation), resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                resource->export_resource.present_pinned ? 1U : 0U, resource->export_resource.presentable ? 1U : 0U, resource->export_resource.predecode_quarantined ? 1U : 0U);
            trace_export_present_state(resource, &resource->export_resource, "nondisplay-private-shadow-refresh", false, false);
        } else if (present_pinned_private_refresh) {
            VKVV_TRACE("nondisplay-present-pinned-skip",
                       "surface=%u codec=0x%x stream=%llu driver=%llu content_gen=%llu shadow_gen=%llu present_gen=%llu refresh_export=0 exported=%u shadow_exported=%u "
                       "present_pinned=1 presentable=%u mutation_action=skipped-client-shadow",
                       surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->driver_instance_id),
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                       static_cast<unsigned long long>(resource->export_resource.present_generation), resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
                       resource->export_resource.presentable ? 1U : 0U);
            trace_export_present_state(resource, &resource->export_resource, "nondisplay-present-pinned-skip", false, false);
        }
        trace_nondisplay_post_check(action);
        if (!trace_decode_shadow_coherence_check(resource, false, false, action)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        VKVV_TRACE("export-refresh-skip-nondisplay",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu seed_gen=%llu shadow_mem=0x%llx shadow_gen=%llu shadow_stale=%u exported=%u shadow_exported=%u "
                   "predecode=%u seeded=%u retired_predecode=%u fd_stat=%u fd_dev=%llu fd_ino=%llu last_display_gen=%llu",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_seed_generation),
                   vkvv_trace_handle(resource->export_resource.memory), static_cast<unsigned long long>(resource->export_resource.content_generation),
                   surface_resource_decode_shadow_stale(resource) ? 1U : 0U, resource->exported ? 1U : 0U, resource->export_resource.exported ? 1U : 0U,
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
    const uint64_t copy_start_us = monotonic_us();
    if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    const uint64_t copy_done_us = monotonic_us();
    clear_private_decode_shadow_state(resource);
    const uint64_t pixel_proof_start_us = monotonic_us();
    (void)trace_visible_pixel_proof(runtime, resource, reason, reason_size);
    const uint64_t pixel_proof_done_us = monotonic_us();
    const bool                    display_visible = refresh_has_visible_display(resource, refresh_export);
    const VkvvExportPresentSource present_source  = visible_present_source(resource);
    const bool copied_to_exported_shadow =
        surface_resource_has_current_export_shadow(resource) && resource->exported && resource->export_resource.exported && !surface_resource_export_shadow_stale(resource);
    const bool copied_to_imported_output = surface_resource_has_direct_import_output(resource);
    const bool internal_import_shadow_current = surface_resource_uses_av1_decode(resource) && resource->import.external &&
        surface_resource_has_current_export_shadow(resource) && !resource->exported && !resource->export_resource.exported &&
        resource->export_resource.owner_surface_id == resource->surface_id && resource->export_resource.stream_id == resource->stream_id &&
        resource->export_resource.codec_operation == resource->codec_operation && !export_resource_fd_may_be_sampled_by_client(&resource->export_resource) &&
        !resource->import_output_copy_target && !surface_resource_export_shadow_stale(resource);
    const bool copy_done = copied_to_exported_shadow || copied_to_imported_output;
    const bool external_release_ok  = export_visible_release_satisfied(&resource->export_resource);
    const bool pixel_proof_required = export_pixel_proof_enabled();
    const bool pixel_match_ok     = !pixel_proof_required || (resource->decode_pixel_proof_valid && resource->present_pixel_proof_valid && resource->present_pixel_matches_decode);
    const bool visible_publish_ok = surface_resource_visible_publish_ready(resource, display_visible, copy_done, pixel_proof_required);
    const uint64_t publish_gate_us = monotonic_us();
    if (internal_import_shadow_current) {
        const bool direct_import_ok = surface_resource_has_direct_import_output(resource);
        VKVV_TRACE("internal-import-shadow-publish-blocked",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu shadow_gen=%llu import_external=1 import_present_generation=%llu direct_import_ok=%u "
                   "exported=%u shadow_exported=%u shadow_published_would_be_internal=1 may_be_sampled_by_client=%u action=block-private-shadow",
                   surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id),
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                   static_cast<unsigned long long>(resource->import_present_generation), direct_import_ok ? 1U : 0U, resource->exported ? 1U : 0U,
                   resource->export_resource.exported ? 1U : 0U, export_resource_fd_may_be_sampled_by_client(&resource->export_resource) ? 1U : 0U);
    }
    VKVV_TRACE("visible-publish-gate",
               "surface=%u codec=0x%x stream=%llu content_gen=%llu display_visible=%u copy_done=%u present_shadow_gen=%llu external_release_ok=%u pixel_match_ok=%u "
               "pixel_proof_required=%u publish_ok=%u",
               surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->content_generation),
               display_visible ? 1U : 0U, copy_done ? 1U : 0U, static_cast<unsigned long long>(resource->export_resource.content_generation), external_release_ok ? 1U : 0U,
               pixel_match_ok ? 1U : 0U, pixel_proof_required ? 1U : 0U, visible_publish_ok ? 1U : 0U);
    if (visible_publish_ok) {
        pin_export_visible_present(resource, &resource->export_resource, present_source);
        exit_predecode_quarantine(resource, &resource->export_resource, external_release_ok);
        trace_export_present_state(resource, &resource->export_resource, "visible-present-pin", refresh_export, display_visible);
    } else {
        const char* block_reason = !display_visible ? "not-visible" :
            !copy_done                              ? "copy-not-ready" :
            !external_release_ok                    ? "external-release" :
            !pixel_match_ok                         ? "pixel-mismatch" :
                                                      "unknown";
        VKVV_TRACE("visible-publish-blocked",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu display_visible=%u copy_done=%u present_shadow_gen=%llu external_release_ok=%u pixel_match_ok=%u "
                   "pixel_proof_required=%u reason=%s",
                   surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->content_generation),
                   display_visible ? 1U : 0U, copy_done ? 1U : 0U, static_cast<unsigned long long>(resource->export_resource.content_generation), external_release_ok ? 1U : 0U,
                   pixel_match_ok ? 1U : 0U, pixel_proof_required ? 1U : 0U, block_reason);
    }
    const uint64_t publish_done_us = monotonic_us();
    trace_av1_visible_output_check(runtime, surface, resource, refresh_export);
    const bool visible_shadow_published = surface_resource_has_exported_shadow_output(resource);
    const bool visible_import_published = surface_resource_has_direct_import_output(resource);
    if (visible_shadow_published && (!resource->export_resource.present_pinned || resource->export_resource.present_generation != resource->content_generation)) {
        VKVV_TRACE("invalid-visible-present-state",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu decode_shadow_gen=%llu present_gen=%llu "
                   "present_pinned=%u published_path=exported-shadow",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                   static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                   static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation),
                   static_cast<unsigned long long>(resource->export_resource.present_generation), resource->export_resource.present_pinned ? 1U : 0U);
        if (!resource->export_resource.present_pinned) {
            VKVV_TRACE("invalid-visible-without-present-pin",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_gen=%llu present_gen=%llu published_path=exported-shadow", surface->id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(resource->export_resource.content_generation),
                       static_cast<unsigned long long>(resource->export_resource.present_generation));
        }
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!trace_decode_shadow_coherence_check(resource, refresh_export, display_visible, "visible-refresh")) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    VKVV_TRACE("visible-output-proof",
               "codec=0x%x surface=%u content_gen=%llu order_hint_or_frame_num=%llu published_path=%s published_gen=%llu previous_visible_surface=%u previous_visible_gen=%llu "
               "published_matches_previous=%u",
               resource->codec_operation, surface->id, static_cast<unsigned long long>(resource->content_generation),
               surface_resource_uses_av1_decode(resource) ? static_cast<unsigned long long>(resource->av1_order_hint) :
                                                            static_cast<unsigned long long>(resource->content_generation),
               av1_published_path(resource, visible_shadow_published, visible_import_published),
               visible_shadow_published ? static_cast<unsigned long long>(resource->export_resource.content_generation) :
                                          static_cast<unsigned long long>(resource->import_present_generation),
               resource->last_display_refresh_generation != 0 ? surface->id : VA_INVALID_ID, static_cast<unsigned long long>(resource->last_display_refresh_generation),
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
    const uint64_t refresh_done_us = monotonic_us();
    VKVV_TRACE("export-refresh-post",
               "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u seeded=%u seed_surface=%u seed_gen=%llu "
               "seeded_targets=%u last_skip_gen=%llu last_skip_shadow_gen=%llu retained=%zu retained_mem=%llu refresh_start_monotonic_us=%llu refresh_total_us=%llu drain_us=%llu "
               "copy_us=%llu pixel_proof_us=%llu publish_us=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
               static_cast<unsigned long long>(resource->content_generation), vkvv_trace_handle(resource->export_resource.memory),
               static_cast<unsigned long long>(resource->export_resource.content_generation), resource->export_resource.predecode_exported ? 1U : 0U,
               resource->export_resource.predecode_seeded ? 1U : 0U, resource->export_resource.seed_source_surface_id,
               static_cast<unsigned long long>(resource->export_resource.seed_source_generation), seeded_predecode_exports,
               static_cast<unsigned long long>(resource->last_nondisplay_skip_generation), static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation),
               runtime_retained_export_count(runtime), static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)),
               static_cast<unsigned long long>(refresh_start_us), static_cast<unsigned long long>(refresh_done_us >= refresh_start_us ? refresh_done_us - refresh_start_us : 0),
               static_cast<unsigned long long>(drain_done_us >= drain_start_us ? drain_done_us - drain_start_us : 0),
               static_cast<unsigned long long>(copy_done_us >= copy_start_us ? copy_done_us - copy_start_us : 0),
               static_cast<unsigned long long>(pixel_proof_done_us >= pixel_proof_start_us ? pixel_proof_done_us - pixel_proof_start_us : 0),
               static_cast<unsigned long long>(publish_done_us >= publish_gate_us ? publish_done_us - publish_gate_us : 0));
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
    const uint32_t export_access_flags     = flags & VA_EXPORT_SURFACE_READ_WRITE;
    const bool     export_request_readable = (export_access_flags & VA_EXPORT_SURFACE_READ_ONLY) != 0;
    const bool     export_request_writable = (export_access_flags & VA_EXPORT_SURFACE_WRITE_ONLY) != 0;
    const bool     export_request_read_only = export_access_flags == VA_EXPORT_SURFACE_READ_ONLY;
    const bool     export_request_read_write = export_access_flags == VA_EXPORT_SURFACE_READ_WRITE;
    const bool     export_request_separate_layers = (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) != 0;
    const bool     export_request_composed_layers = (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;
    VKVV_TRACE("export-request-flags",
               "surface=%u export_flags=0x%x access_flags=0x%x readable=%u writable=%u read_only=%u read_write=%u separate_layers=%u composed_layers=%u sampleable_export=%u",
               surface->id, flags, export_access_flags, export_request_readable ? 1U : 0U, export_request_writable ? 1U : 0U,
               export_request_read_only ? 1U : 0U, export_request_read_write ? 1U : 0U, export_request_separate_layers ? 1U : 0U,
               export_request_composed_layers ? 1U : 0U, export_request_readable ? 1U : 0U);
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
    const ExportPendingSnapshot pending_before_export         = pending_export_snapshot(runtime, surface);
    const bool                  pending_decode_before_export = pending_before_export.found;
    const bool                  decoded_before_export_drain  = surface->decoded;
    const uint64_t              content_generation_before_drain = resource->content_generation;
    const bool                  fd_already_exported_before         = resource->export_resource.exported_fd.fd_exported;
    auto trace_drain_attempt = [&](VAStatus status) {
        VKVV_TRACE("export-drain-attempt",
                   "surface=%u stream=%llu codec=0x%x content_gen_before=%llu pending_decode_found=%u pending_operation=%s pending_surface=%u pending_refresh_export=%u "
                   "pending_display_visible=%u drain_attempted=%u drain_status=%d content_gen_after=%llu fd_content_gen_after=%llu can_return_decoded_after_drain=%u",
                   surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(content_generation_before_drain), pending_before_export.found ? 1U : 0U,
                   pending_before_export.operation[0] != '\0' ? pending_before_export.operation : "none", pending_before_export.surface,
                   pending_before_export.refresh_export ? 1U : 0U, pending_before_export.refresh_export ? 1U : 0U, pending_before_export.found ? 1U : 0U, status,
                   static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(export_resource_fd_content_generation(&resource->export_resource)),
                   surface->decoded && resource->content_generation != 0 ? 1U : 0U);
    };
    auto trace_export_summary = [&](ExportResource* returned_resource, const VkvvReturnedFdProof* proof, bool returned_fd, const VkvvFdIdentity& fd_stat,
                                    const char* decision, const char* trace_reason, VAStatus status) {
        const VkvvExportPixelSource pixel_source = returned_resource != nullptr ? export_pixel_source_for_resource(resource, returned_resource) :
            (returned_fd && surface->decoded && resource->content_generation != 0 ? VkvvExportPixelSource::DecodedContent : VkvvExportPixelSource::None);
        const uint64_t fd_content_gen = returned_resource != nullptr ? export_resource_fd_content_generation(returned_resource) : (returned_fd ? resource->content_generation : 0);
        const VkvvExportRole export_role = returned_resource != nullptr ? export_resource_fd_role(returned_resource) :
            (returned_fd && surface->decoded && resource->content_generation != 0 ? VkvvExportRole::DecodedPixels : VkvvExportRole::None);
        const bool may_sample = returned_resource != nullptr ? export_resource_fd_may_be_sampled_by_client(returned_resource) :
                                                           returned_fd && export_role == VkvvExportRole::DecodedPixels;
        const bool     proof_black = proof != nullptr && proof->pixel_crc != 0 && proof->black_crc != 0 && proof->pixel_crc == proof->black_crc;
        const bool     proof_zero  = proof != nullptr && proof->pixel_crc != 0 && proof->zero_crc != 0 && proof->pixel_crc == proof->zero_crc;
        const bool     placeholder =
            pixel_source == VkvvExportPixelSource::Placeholder || (returned_resource != nullptr && returned_resource->neutral_backing) || proof_black || proof_zero;
        const bool valid_decoded_pixels_available = surface->decoded && resource->content_generation != 0 &&
            (returned_resource == nullptr || returned_resource->content_generation == resource->content_generation || pixel_source == VkvvExportPixelSource::DecodedContent);
        const bool seed_proof_required = export_pixel_proof_enabled();
        const bool valid_seed_available = pixel_source == VkvvExportPixelSource::StreamLocalSeed && fd_content_gen != 0 && returned_resource != nullptr &&
            (!seed_proof_required || returned_resource->seed_pixel_proof_valid) && (proof == nullptr || proof->seed_pixels_valid);
        VKVV_TRACE("export-validity-gate",
                   "surface=%u driver=%llu stream=%llu codec=0x%x profile=0 width=%u height=%u fourcc=0x%x content_gen=%llu decoded=%u pending_decode=%u refresh_export=%u "
                   "display_visible=%u export_flags=0x%x access_flags=0x%x sampleable_export=%u fd_already_exported=%u fd_dev=%llu fd_ino=%llu fd_content_gen=%llu "
                   "may_be_sampled_by_client=%u valid_decoded_pixels_available=%u "
                   "valid_seed_available=%u placeholder_available=%u retained_candidate_available=%u export_role=%s decision=%s reason=%s returned_fd=%u status=%d",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, surface->width,
                   surface->height, resource->va_fourcc, static_cast<unsigned long long>(resource->content_generation), surface->decoded ? 1U : 0U,
                   pending_before_export.found ? 1U : 0U, surface->decoded ? 1U : 0U, surface->decoded ? 1U : 0U, flags, export_access_flags,
                   export_request_readable ? 1U : 0U, fd_already_exported_before ? 1U : 0U,
                   static_cast<unsigned long long>(fd_stat.valid ? fd_stat.dev : (returned_resource != nullptr ? returned_resource->exported_fd.fd_dev : 0)),
                   static_cast<unsigned long long>(fd_stat.valid ? fd_stat.ino : (returned_resource != nullptr ? returned_resource->exported_fd.fd_ino : 0)),
                   static_cast<unsigned long long>(fd_content_gen), may_sample ? 1U : 0U, valid_decoded_pixels_available ? 1U : 0U, valid_seed_available ? 1U : 0U,
                   placeholder ? 1U : 0U, resource->export_retained_attached ? 1U : 0U, vkvv_export_role_name(export_role), decision != nullptr ? decision : "fail",
                   trace_reason != nullptr ? trace_reason : "unknown", returned_fd ? 1U : 0U, status);
        VKVV_TRACE("generic-export-summary",
                   "surface=%u stream=%llu codec=0x%x width=%u height=%u fourcc=0x%x content_gen=%llu fd_content_gen=%llu returned_fd=%u decision=%s pixel_source=%s "
                   "pixel_proof_valid=%u is_black=%u is_zero=%u pending_decode=%u valid_seed_available=%u quarantine_outcome=%s external_release_mode=%s status=%d "
                   "may_be_sampled_by_client=%u export_role=%s export_flags=0x%x access_flags=0x%x sampleable_export=%u",
                   surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, surface->width, surface->height, resource->va_fourcc,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(fd_content_gen), returned_fd ? 1U : 0U,
                   decision != nullptr ? decision : "fail", vkvv_export_pixel_source_name(pixel_source), proof != nullptr && proof->pixel_proof_valid ? 1U : 0U,
                   proof_black || pixel_source == VkvvExportPixelSource::Placeholder ? 1U : 0U, proof_zero ? 1U : 0U, pending_before_export.found ? 1U : 0U,
                   valid_seed_available ? 1U : 0U,
                   returned_resource != nullptr && returned_resource->predecode_quarantined ? "pending" : "none",
                   returned_resource != nullptr ? vkvv_external_release_mode_name(returned_resource->external_sync.release_mode) : "none", status, may_sample ? 1U : 0U,
                   vkvv_export_role_name(export_role), flags, export_access_flags, export_request_readable ? 1U : 0U);
    };
    VAStatus drain_status = drain_pending_surface_work_before_sync_command(runtime, const_cast<VkvvSurface*>(surface), reason, reason_size);
    trace_drain_attempt(drain_status);
    if (drain_status != VA_STATUS_SUCCESS) {
        VkvvFdIdentity no_fd{};
        trace_export_summary(nullptr, nullptr, false, no_fd, "fail", "drain-failed", drain_status);
        return drain_status;
    }
    if (!decoded_before_export_drain && surface->decoded) {
        VKVV_TRACE("predecode-export-policy",
                   "surface=%u codec=0x%x stream=%llu content_gen=%llu pending_decode=%u policy=delay-if-pending action=drained-and-exported source_surface=%u "
                   "source_present_gen=0 source_external_release_ok=0",
                   surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->content_generation),
                   pending_decode_before_export ? 1U : 0U, surface->id);
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
    bool                       predecode_seed_attempted_before_backing = false;
    bool                       predecode_seeded_before_backing         = false;

    if (!resource->exportable) {
        if (surface->decoded) {
            const bool private_decode_shadow_current = !export_resource_fd_may_be_sampled_by_client(&resource->export_resource) && resource->export_resource.present_pinned &&
                current_decode_shadow(resource) == &resource->private_decode_shadow;
            const bool force_visible_copy       = av1_visible_export_requires_copy(resource) && !private_decode_shadow_current;
            const bool shadow_current           = resource->content_generation != 0 && resource->export_resource.content_generation == resource->content_generation;
            uint32_t   seeded_predecode_exports = 0;
            if (private_decode_shadow_current) {
                VKVV_TRACE("export-late-present-shadow-preserved",
                           "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu present_gen=%llu present_shadow_gen=%llu private_shadow_gen=%llu "
                           "decode_shadow_gen=%llu present_pinned=1 presentable=%u copied=0",
                           surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                           resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                           static_cast<unsigned long long>(resource->export_resource.present_generation),
                           static_cast<unsigned long long>(resource->export_resource.content_generation),
                           static_cast<unsigned long long>(resource->private_decode_shadow.content_generation),
                           static_cast<unsigned long long>(resource->export_resource.decode_shadow_generation), resource->export_resource.presentable ? 1U : 0U);
            } else {
                if (!copy_surface_to_export_resource(runtime, resource, &seeded_predecode_exports, reason, reason_size)) {
                    VkvvFdIdentity no_fd{};
                    trace_export_summary(&resource->export_resource, nullptr, false, no_fd, "fail", "copy-shadow-failed", VA_STATUS_ERROR_OPERATION_FAILED);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                clear_private_decode_shadow_state(resource);
            }
            copied_to_shadow = !private_decode_shadow_current && (!shadow_current || force_visible_copy);
            VKVV_TRACE("export-late-refresh",
                       "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu shadow_before=%llu shadow_after=%llu copied=%u forced=%u after_skip=%u skip_gen=%llu "
                       "skip_shadow_gen=%llu skip_shadow_stale=%u seeded_targets=%u",
                       surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(shadow_generation_before),
                       static_cast<unsigned long long>(resource->export_resource.content_generation), copied_to_shadow ? 1U : 0U, force_visible_copy ? 1U : 0U,
                       export_after_nondisplay_skip ? 1U : 0U, static_cast<unsigned long long>(resource->last_nondisplay_skip_generation),
                       static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation), skip_shadow_was_stale ? 1U : 0U, seeded_predecode_exports);
        } else if (!ensure_export_resource(runtime, resource, reason, reason_size)) {
            VkvvFdIdentity no_fd{};
            trace_export_summary(&resource->export_resource, nullptr, false, no_fd, "fail", "ensure-export-resource-failed", VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        } else if (active_domain_predecode_pool_export_candidate(surface, resource)) {
            predecode_seed_attempted_before_backing = true;
            if (!seed_predecode_export_from_last_good(runtime, resource, reason, reason_size)) {
                VkvvFdIdentity no_fd{};
                trace_export_summary(&resource->export_resource, nullptr, false, no_fd, "fail", "seed-export-failed", VA_STATUS_ERROR_OPERATION_FAILED);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            const VkvvExportPixelSource seeded_pixel_source = export_pixel_source_for_resource(resource, &resource->export_resource);
            predecode_seeded_before_backing                 = export_source_has_seed_pixels(&resource->export_resource, seeded_pixel_source);
            if (!predecode_seeded_before_backing) {
                mark_export_predecode_nonpresentable(&resource->export_resource);
            }
            VKVV_TRACE("predecode-export-policy",
                       "surface=%u codec=0x%x stream=%llu content_gen=0 pending_decode=0 policy=exact-surface-predecode action=%s "
                       "source_surface=%u source_present_gen=0 source_external_release_ok=0 seeded_before_return=%u fd_content_gen=%llu",
                       surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id),
                       predecode_seeded_before_backing ? "stream-local-seed-before-return" : "exact-surface-backing", surface->id,
                       predecode_seeded_before_backing ? 1U : 0U,
                       static_cast<unsigned long long>(export_resource_fd_content_generation(&resource->export_resource)));
        } else if (!seed_predecode_export_from_last_good(runtime, resource, reason, reason_size)) {
            VkvvFdIdentity no_fd{};
            trace_export_summary(&resource->export_resource, nullptr, false, no_fd, "fail", "seed-export-failed", VA_STATUS_ERROR_OPERATION_FAILED);
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
        VkvvFdIdentity no_fd{};
        trace_export_summary(exported_shadow, nullptr, false, no_fd, "fail", "missing-export-memory", VA_STATUS_ERROR_INVALID_SURFACE);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    const ExportAdmissionDecision admission = decide_export_admission(surface, resource, exported_shadow, export_request_readable);
    const VkvvExportPixelSource   pre_fd_pixel_source              = admission.pixel_source;
    const bool                    valid_decoded_pixels_available   = admission.valid_decoded_pixels_available;
    const bool                    valid_seed_available             = admission.valid_seed_available;
    const bool                    valid_transition_hold_available  = admission.valid_transition_hold_available;
    const bool                    placeholder_available            = admission.placeholder_available;
    const bool                    active_predecode_candidate       = admission.active_predecode_candidate;
    const bool                    exported_shadow_is_owner         = admission.exported_shadow_is_owner;
    const bool                    exact_predecode_pool_backing  = admission.exact_predecode_pool_backing;
    const bool                    predecode_backing_export         = admission.predecode_backing_export;
    const bool                    no_seed_predecode_backing        = admission.no_seed_predecode_backing;
    const bool                    no_seed_predecode_probe_export   = admission.no_seed_predecode_probe_export;
    const bool                    sampleable_placeholder_export    = admission.sampleable_placeholder_export;
    if (export_request_readable && placeholder_available) {
        const uint64_t    shadow_content_generation = exported_shadow != nullptr ? exported_shadow->content_generation : 0;
        const VASurfaceID shadow_owner_surface      = exported_shadow != nullptr ? exported_shadow->owner_surface_id : VA_INVALID_ID;
        const uint64_t    shadow_stream             = exported_shadow != nullptr ? exported_shadow->stream_id : 0;
        const uint32_t    shadow_codec              = exported_shadow != nullptr ? exported_shadow->codec_operation : 0;
        const VASurfaceID shadow_seed_surface       = exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID;
        const uint64_t    shadow_seed_generation    = exported_shadow != nullptr ? exported_shadow->seed_source_generation : 0;
        VKVV_TRACE("predecode-backing-gate",
                   "surface=%u driver=%llu stream=%llu codec=0x%x active_domain_candidate=%u exact_predecode_pool_backing=%u predecode_backing_export=%u "
                   "exported_shadow_is_owner=%u pixel_source=%s placeholder_available=%u content_gen=%llu shadow_gen=%llu shadow_owner=%u shadow_stream=%llu "
                   "shadow_codec=0x%x shadow_predecode_seeded=%u shadow_seed_surface=%u shadow_seed_gen=%llu shadow_seed_proof=%u shadow_neutral_backing=%u "
                   "resource_stream=%llu resource_codec=0x%x export_flags=0x%x sampleable_export=%u seed_attempted_before_backing=%u seeded_before_return=%u "
                   "no_seed_predecode_backing=%u probe_sized_no_seed=%u decision=%s",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                   resource->codec_operation, active_predecode_candidate ? 1U : 0U, exact_predecode_pool_backing ? 1U : 0U,
                   predecode_backing_export ? 1U : 0U, exported_shadow_is_owner ? 1U : 0U, vkvv_export_pixel_source_name(pre_fd_pixel_source),
                   placeholder_available ? 1U : 0U, static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(shadow_content_generation), shadow_owner_surface, static_cast<unsigned long long>(shadow_stream), shadow_codec,
                   exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U, shadow_seed_surface,
                   static_cast<unsigned long long>(shadow_seed_generation), exported_shadow != nullptr && exported_shadow->seed_pixel_proof_valid ? 1U : 0U,
                   exported_shadow != nullptr && exported_shadow->neutral_backing ? 1U : 0U, static_cast<unsigned long long>(resource->stream_id),
                   resource->codec_operation, flags, export_request_readable ? 1U : 0U, predecode_seed_attempted_before_backing ? 1U : 0U,
                   predecode_seeded_before_backing ? 1U : 0U, no_seed_predecode_backing ? 1U : 0U, no_seed_predecode_probe_export ? 1U : 0U,
                   no_seed_predecode_probe_export ? "reject-no-seed-probe" : (predecode_backing_export ? "return-predecode-backing" : "sampleable-placeholder-gate"));
    }
    if (no_seed_predecode_probe_export) {
        VkvvFdIdentity no_fd{};
        trace_predecode_quarantine_outcome(resource, exported_shadow, "export-failed", "no-seed-probe-sized-predecode", false);
        VKVV_TRACE("predecode-backing-no-seed-probe-reject",
                   "surface=%u driver=%llu stream=%llu codec=0x%x width=%u height=%u coded_width=%u coded_height=%u fourcc=0x%x export_flags=0x%x "
                   "pixel_source=%s placeholder_available=%u seed_attempted_before_backing=%u seeded_before_return=%u fd_content_gen=0 returned_fd=0 "
                   "decision=reject-no-seed-probe status=%d",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                   resource->codec_operation, surface->width, surface->height, resource->coded_extent.width, resource->coded_extent.height, surface->fourcc, flags,
                   vkvv_export_pixel_source_name(pre_fd_pixel_source), placeholder_available ? 1U : 0U,
                   predecode_seed_attempted_before_backing ? 1U : 0U, predecode_seeded_before_backing ? 1U : 0U, VA_STATUS_ERROR_OPERATION_FAILED);
        trace_export_summary(exported_shadow, nullptr, false, no_fd, admission.failure_decision, admission.failure_reason, admission.status);
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_OPERATION_FAILED,
                          "surface export refused sampleable no-seed predecode probe fd: surface=%u stream=%llu codec=0x%x %ux%u content_gen=%llu",
                          surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, surface->width, surface->height,
                          static_cast<unsigned long long>(resource->content_generation));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (sampleable_placeholder_export) {
        VkvvFdIdentity no_fd{};
        if (exported_shadow != nullptr) {
            trace_predecode_quarantine_outcome(resource, exported_shadow, "export-failed", "sampleable-placeholder", false);
        }
        trace_export_summary(exported_shadow, nullptr, false, no_fd, admission.failure_decision, admission.failure_reason, admission.status);
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_OPERATION_FAILED,
                          "surface export refused sampleable placeholder fd: surface=%u stream=%llu codec=0x%x content_gen=%llu pixel_source=%s",
                          surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                          static_cast<unsigned long long>(resource->content_generation), vkvv_export_pixel_source_name(pre_fd_pixel_source));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!valid_decoded_pixels_available && !valid_seed_available && !valid_transition_hold_available && !predecode_backing_export) {
        VkvvFdIdentity no_fd{};
        if (placeholder_available && exported_shadow != nullptr) {
            trace_predecode_quarantine_outcome(resource, exported_shadow, "export-failed", "no-valid-pixels", false);
        }
        trace_export_summary(exported_shadow, nullptr, false, no_fd, admission.failure_decision, admission.failure_reason, admission.status);
        VKVV_ERROR_REASON(reason, reason_size, VA_STATUS_ERROR_OPERATION_FAILED,
                          "surface export refused sampleable fd without decoded or valid seed pixels: surface=%u stream=%llu codec=0x%x content_gen=%llu pixel_source=%s",
                          surface->id, static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                          static_cast<unsigned long long>(resource->content_generation), vkvv_export_pixel_source_name(pre_fd_pixel_source));
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (predecode_backing_export) {
        VKVV_TRACE("predecode-backing-export",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=0 decision=return-predecode-backing returned_fd=1 exact_surface=1 seed_surface=%u "
                   "seed_gen=0 presentable=0 published_visible=0 predecode_quarantined=1 fd_content_gen=0 may_be_sampled_by_client=0 export_role=predecode-backing "
                   "pixel_source=%s seed_attempted_before_backing=%u seeded_before_return=%u no_seed_predecode_backing=%u width=%u height=%u coded_width=%u coded_height=%u fourcc=0x%x",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   VA_INVALID_ID, vkvv_export_pixel_source_name(pre_fd_pixel_source), predecode_seed_attempted_before_backing ? 1U : 0U,
                   predecode_seeded_before_backing ? 1U : 0U, no_seed_predecode_backing ? 1U : 0U, surface->width, surface->height, resource->coded_extent.width,
                   resource->coded_extent.height, surface->fourcc);
    } else if (valid_transition_hold_available) {
        VKVV_TRACE("transition-hold-export",
                   "surface=%u driver=%llu stream=%llu codec=0x%x content_gen=%llu decision=return-transition-hold returned_fd=1 fd_content_gen=%llu "
                   "present_gen=%llu present_surface=%u may_be_sampled_by_client=1 export_role=transition-hold",
                   surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                   static_cast<unsigned long long>(resource->content_generation), static_cast<unsigned long long>(exported_shadow->content_generation),
                   static_cast<unsigned long long>(exported_shadow->present_generation), exported_shadow->present_surface_id);
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
        VkvvFdIdentity no_fd{};
        trace_export_summary(exported_shadow, nullptr, false, no_fd, "fail", "get-fd-failed", VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const VAStatus descriptor_status = fill_drm_prime_descriptor(surface, format, export_allocation_size, export_plane_layouts, export_plane_count, export_modifier,
                                                                 export_has_modifier, fd, descriptor, reason, reason_size);
    if (descriptor_status != VA_STATUS_SUCCESS) {
        close(fd);
        VkvvFdIdentity no_fd{};
        trace_export_summary(exported_shadow, nullptr, false, no_fd, "fail", "descriptor-fill-failed", descriptor_status);
        return descriptor_status;
    }

    resource->exported = true;
    if (exported_shadow != nullptr) {
        exported_shadow->exported = true;
        if (!surface->decoded && !valid_transition_hold_available) {
            exported_shadow->predecode_exported = true;
            register_predecode_export_resource(runtime, exported_shadow);
            VKVV_TRACE("export-predecode-register", "surface=%u driver=%llu stream=%llu codec=0x%x shadow_mem=0x%llx shadow_gen=%llu seeded=%u neutral_backing=%u", surface->id,
                       static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation,
                       vkvv_trace_handle(exported_shadow->memory), static_cast<unsigned long long>(exported_shadow->content_generation),
                       exported_shadow->predecode_seeded ? 1U : 0U, exported_shadow->neutral_backing ? 1U : 0U);
        }
    }
    const VkvvFdIdentity fd_stat = vkvv_fd_identity_from_fd(fd);
    const VkvvExportRole returned_export_role = admission.role;
    if (exported_shadow != nullptr) {
        exported_shadow->fd_stat_valid       = fd_stat.valid;
        exported_shadow->fd_dev              = fd_stat.dev;
        exported_shadow->fd_ino              = fd_stat.ino;
        const uint64_t fd_content_generation = exported_shadow->predecode_seeded ? exported_shadow->seed_source_generation : exported_shadow->content_generation;
        mark_export_fd_returned(exported_shadow, fd_stat, fd_content_generation, returned_export_role);
        trace_export_role_lifecycle(resource, exported_shadow, "return", surface->decoded);
        if (!surface->decoded && !valid_transition_hold_available) {
            mark_export_predecode_nonpresentable(exported_shadow);
            enter_predecode_quarantine(resource, exported_shadow);
            trace_export_present_state(resource, exported_shadow, "predecode-export", false, false);
            const bool seeded_predecode = exported_shadow->predecode_seeded && exported_shadow->seed_source_generation != 0;
            VKVV_TRACE("predecode-export-policy",
                       "surface=%u codec=0x%x stream=%llu content_gen=%llu pending_decode=%u policy=stream-local-last-visible action=%s source_surface=%u "
                       "source_present_gen=%llu source_external_release_ok=%u",
                       surface->id, resource->codec_operation, static_cast<unsigned long long>(resource->stream_id), static_cast<unsigned long long>(resource->content_generation),
                       pending_decode_before_export ? 1U : 0U, seeded_predecode ? "stream-local-seed" : "allocation-only-backing",
                       seeded_predecode ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
                       static_cast<unsigned long long>(seeded_predecode ? exported_shadow->seed_source_generation : 0), seeded_predecode ? 1U : 0U);
            if (!seeded_predecode) {
                if (predecode_backing_export && no_seed_predecode_backing) {
                    VKVV_TRACE("predecode-backing-no-seed-return",
                               "surface=%u driver=%llu stream=%llu codec=0x%x width=%u height=%u coded_width=%u coded_height=%u fourcc=0x%x export_flags=0x%x "
                               "pixel_source=%s placeholder_available=%u seed_attempted_before_backing=%u seeded_before_return=%u fd_dev=%llu fd_ino=%llu "
                               "fd_content_gen=%llu presentable=0 published_visible=0 predecode_quarantined=%u had_va_begin=%u had_decode_submit=%u "
                               "had_visible_decode=%u may_be_sampled_by_client=%u export_role=predecode-backing decision=return-quarantined-allocation returned_fd=1",
                               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id),
                               resource->codec_operation, surface->width, surface->height, resource->coded_extent.width, resource->coded_extent.height, surface->fourcc, flags,
                               vkvv_export_pixel_source_name(pre_fd_pixel_source), placeholder_available ? 1U : 0U,
                               predecode_seed_attempted_before_backing ? 1U : 0U, predecode_seeded_before_backing ? 1U : 0U,
                               static_cast<unsigned long long>(exported_shadow->exported_fd.fd_dev),
                               static_cast<unsigned long long>(exported_shadow->exported_fd.fd_ino),
                               static_cast<unsigned long long>(export_resource_fd_content_generation(exported_shadow)),
                               exported_shadow->predecode_quarantined ? 1U : 0U, exported_shadow->predecode_had_va_begin ? 1U : 0U,
                               exported_shadow->predecode_had_decode_submit ? 1U : 0U, exported_shadow->predecode_had_visible_decode ? 1U : 0U,
                               export_resource_fd_may_be_sampled_by_client(exported_shadow) ? 1U : 0U);
                }
                trace_predecode_quarantine_outcome(resource, exported_shadow, "backing-returned",
                                                   no_seed_predecode_backing ? "predecode-backing-no-seed-return" : "predecode-backing-export",
                                                   true);
            }
        }
        trace_exported_fd_freshness_check(resource, exported_shadow, surface->decoded, surface->decoded,
                                          surface->decoded ? "export-fd-current" :
                                                             (valid_transition_hold_available ? "transition-hold" :
                                                                 (exported_shadow->predecode_seeded ? "stream-local-seed" : "allocation-only-backing")));
    }
    VKVV_TRACE("export-fd",
               "surface=%u driver=%llu stream=%llu codec=0x%x fd=%d fd_stat=%u fd_dev=%llu fd_ino=%llu export_mem=0x%llx content_gen=%llu shadow_gen=%llu predecode=%u seeded=%u "
               "neutral_backing=%u seed_surface=%u seed_gen=%llu fd_content_gen=%llu may_be_sampled_by_client=%u export_role=%s after_skip=%u skip_gen=%llu skip_shadow_gen=%llu",
               surface->id, static_cast<unsigned long long>(resource->driver_instance_id), static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, fd,
               fd_stat.valid ? 1U : 0U, static_cast<unsigned long long>(fd_stat.dev), static_cast<unsigned long long>(fd_stat.ino), vkvv_trace_handle(export_memory),
               static_cast<unsigned long long>(resource->content_generation),
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
               exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U,
               exported_shadow != nullptr && exported_shadow->neutral_backing ? 1U : 0U, exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
               static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->seed_source_generation : 0),
               static_cast<unsigned long long>(exported_shadow != nullptr ? export_resource_fd_content_generation(exported_shadow) : resource->content_generation),
               exported_shadow != nullptr && export_resource_fd_may_be_sampled_by_client(exported_shadow) ? 1U : 0U, vkvv_export_role_name(returned_export_role),
               export_after_nondisplay_skip ? 1U : 0U,
               static_cast<unsigned long long>(resource->last_nondisplay_skip_generation), static_cast<unsigned long long>(resource->last_nondisplay_skip_shadow_generation));

    VkvvReturnedFdProof         returned_proof{};
    const bool                  drained_to_decoded = pending_decode_before_export && !decoded_before_export_drain && surface->decoded && resource->content_generation != 0;
    const VkvvExportPixelSource pixel_source =
        exported_shadow != nullptr ? export_pixel_source_for_resource(resource, exported_shadow) : VkvvExportPixelSource::DecodedContent;
    const bool returned_predecode_backing = exported_shadow != nullptr && exported_shadow->predecode_quarantined &&
        pixel_source == VkvvExportPixelSource::Placeholder && export_resource_fd_content_generation(exported_shadow) == 0 &&
        !export_resource_fd_may_be_sampled_by_client(exported_shadow);
    const char* decision =
        returned_predecode_backing ? "return-predecode-backing" : (valid_transition_hold_available ? "return-transition-hold" : export_decision_for_source(pixel_source, drained_to_decoded));
    if (exported_shadow != nullptr) {
        (void)trace_returned_fd_pixel_proof(runtime, resource, exported_shadow, fd_stat, pixel_source, &returned_proof, reason, reason_size);
        trace_export_fd_lifetime(resource, exported_shadow, "return", export_resource_fd_content_generation(exported_shadow),
                                 export_resource_fd_may_be_sampled_by_client(exported_shadow));
    } else {
        returned_proof.returned_fd              = fd_stat.valid;
        returned_proof.fd                       = fd;
        returned_proof.fd_dev                   = fd_stat.dev;
        returned_proof.fd_ino                   = fd_stat.ino;
        returned_proof.may_be_sampled_by_client = fd_stat.valid;
        returned_proof.surface_id               = surface->id;
        returned_proof.stream_id                = resource->stream_id;
        returned_proof.codec_operation          = resource->codec_operation;
        returned_proof.content_generation       = resource->content_generation;
        returned_proof.fd_content_generation    = resource->content_generation;
        returned_proof.pixel_source             = pixel_source;
        returned_proof.export_role              = returned_export_role;
        returned_proof.decoded_pixels_valid     = surface->decoded && resource->content_generation != 0;
        VKVV_TRACE("returned-fd-pixel-proof",
                   "surface=%u fd_dev=%llu fd_ino=%llu stream=%llu codec=0x%x content_gen=%llu fd_content_gen=%llu pixel_source=%s returned_crc=0x0 black_crc=0x0 zero_crc=0x0 "
                   "is_black=0 is_zero=0 pixel_proof_valid=0 may_be_sampled_by_client=%u export_role=%s returned_fd=1 sample_bytes=0 proof_enabled=0",
                   surface->id, static_cast<unsigned long long>(fd_stat.dev), static_cast<unsigned long long>(fd_stat.ino),
                   static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, static_cast<unsigned long long>(resource->content_generation),
                   static_cast<unsigned long long>(resource->content_generation), vkvv_export_pixel_source_name(pixel_source), fd_stat.valid ? 1U : 0U,
                   vkvv_export_role_name(returned_export_role));
        trace_direct_export_fd_lifetime(surface, resource, fd_stat, "return", resource->content_generation, fd_stat.valid);
    }
    trace_export_summary(exported_shadow, &returned_proof, true, fd_stat, decision,
                         returned_predecode_backing ? "allocation-only-active-domain" : (valid_transition_hold_available ? "transition-hold" : "success"), VA_STATUS_SUCCESS);

    VKVV_SUCCESS_REASON(
        reason, reason_size,
        "exported %s dma-buf%s: driver=%llu surface=%u stream=%llu codec=0x%x %ux%u fd=%d size=%u modifier=0x%llx y_pitch=%u uv_pitch=%u decode_mem=%llu export_mem=%llu "
        "retained=%zu retained_mem=%llu source_generation=%llu shadow_generation=%llu predecode=%u seeded=%u neutral_backing=%s seed_source=%u seed_generation=%llu",
        format->name, copied_to_shadow ? " via shadow copy" : "", static_cast<unsigned long long>(resource->driver_instance_id), surface->id,
        static_cast<unsigned long long>(resource->stream_id), resource->codec_operation, surface->width, surface->height, fd, descriptor->objects[0].size,
        static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier), descriptor->layers[0].pitch[0], descriptor->layers[1].pitch[0],
        static_cast<unsigned long long>(resource->allocation_size), static_cast<unsigned long long>(export_memory_bytes(resource)), runtime_retained_export_count(runtime),
        static_cast<unsigned long long>(runtime_retained_export_memory_bytes(runtime)), static_cast<unsigned long long>(resource->content_generation),
        static_cast<unsigned long long>(exported_shadow != nullptr ? exported_shadow->content_generation : resource->content_generation),
        exported_shadow != nullptr && exported_shadow->predecode_exported ? 1U : 0U, exported_shadow != nullptr && exported_shadow->predecode_seeded ? 1U : 0U,
        exported_shadow != nullptr && exported_shadow->neutral_backing ? "black" : "none", exported_shadow != nullptr ? exported_shadow->seed_source_surface_id : VA_INVALID_ID,
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

void vkvv_vulkan_note_surface_decode_begin(const VkvvSurface* surface) {
    if (surface == nullptr || surface->vulkan == nullptr) {
        return;
    }
    auto* resource = static_cast<SurfaceResource*>(surface->vulkan);
    if (resource->export_resource.image == VK_NULL_HANDLE || !resource->export_resource.predecode_quarantined ||
        export_resource_fd_role(&resource->export_resource) != VkvvExportRole::PredecodeBacking) {
        return;
    }
    resource->export_resource.predecode_had_va_begin = true;
    trace_export_role_lifecycle(resource, &resource->export_resource, "predecode-backing-claimed-begin", false);
}

void vkvv_vulkan_prune_driver_exports(void* runtime_ptr, uint64_t driver_instance_id) {
    prune_detached_exports_for_driver(static_cast<VulkanRuntime*>(runtime_ptr), driver_instance_id);
}
