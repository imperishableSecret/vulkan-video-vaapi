#include "vulkan/runtime_internal.h"

namespace vkvv {

    bool surface_resource_uses_av1_decode(const SurfaceResource* resource) {
        return resource != nullptr && resource->codec_operation == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    }

    bool av1_non_display_export_refresh(const SurfaceResource* resource, bool refresh_export) {
        return surface_resource_uses_av1_decode(resource) && !refresh_export;
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

} // namespace vkvv
