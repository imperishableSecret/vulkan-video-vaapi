#include "vulkan/runtime_internal.h"

#include <cstdlib>
#include <cstring>

namespace vkvv {

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

    bool surface_resource_has_current_export_shadow(const SurfaceResource* resource) {
        return resource != nullptr && resource->content_generation != 0 && resource->export_resource.image != VK_NULL_HANDLE &&
            resource->export_resource.memory != VK_NULL_HANDLE && resource->export_resource.content_generation == resource->content_generation &&
            !surface_resource_export_shadow_stale(resource);
    }

    bool surface_resource_has_exported_shadow_output(const SurfaceResource* resource) {
        return surface_resource_has_current_export_shadow(resource) && resource->exported && resource->export_resource.exported;
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
