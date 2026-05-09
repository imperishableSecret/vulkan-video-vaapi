#include "vulkan/export/internal.h"
#include "telemetry.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <drm_fourcc.h>

namespace vkvv {

    VAStatus validate_export_flags(uint32_t flags, char* reason, size_t reason_size) {
        const uint32_t access = flags & VA_EXPORT_SURFACE_READ_WRITE;
        if (access != VA_EXPORT_SURFACE_READ_ONLY) {
            std::snprintf(reason, reason_size, "surface export requires read-only access flags");
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) == 0) {
            std::snprintf(reason, reason_size, "surface export currently requires separate layers");
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if ((flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0) {
            std::snprintf(reason, reason_size, "surface export does not support composed layers");
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        constexpr uint32_t supported_flags = VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS;
        if ((flags & ~supported_flags) != 0) {
            std::snprintf(reason, reason_size, "surface export has unsupported flags=0x%x", flags & ~supported_flags);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus fill_drm_prime_descriptor(const VkvvSurface* surface, const ExportFormatInfo* format, VkDeviceSize allocation_size, const VkSubresourceLayout* plane_layouts,
                                       uint32_t plane_count, uint64_t modifier, bool has_modifier, int fd, VADRMPRIMESurfaceDescriptor* descriptor, char* reason,
                                       size_t reason_size) {
        if (plane_count != format->layer_count) {
            std::snprintf(reason, reason_size, "exportable %s image has %u planes, expected %u", format->name, plane_count, format->layer_count);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (allocation_size > std::numeric_limits<uint32_t>::max()) {
            std::snprintf(reason, reason_size, "export allocation is too large for VADRMPRIMESurfaceDescriptor");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        for (uint32_t i = 0; i < format->layer_count; i++) {
            if (plane_layouts[i].offset > std::numeric_limits<uint32_t>::max() || plane_layouts[i].rowPitch > std::numeric_limits<uint32_t>::max()) {
                std::snprintf(reason, reason_size, "export plane layout is too large for VADRMPRIMESurfaceDescriptor");
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        }

        std::memset(descriptor, 0, sizeof(*descriptor));
        descriptor->fourcc                         = format->va_fourcc;
        descriptor->width                          = surface->width;
        descriptor->height                         = surface->height;
        descriptor->num_objects                    = 1;
        descriptor->objects[0].fd                  = fd;
        descriptor->objects[0].size                = static_cast<uint32_t>(allocation_size);
        descriptor->objects[0].drm_format_modifier = has_modifier ? modifier : DRM_FORMAT_MOD_INVALID;
        descriptor->num_layers                     = format->layer_count;
        vkvv_trace("export-descriptor-object", "surface=%u fourcc=0x%x format=%s fd=%d size=%u modifier=0x%llx has_modifier=%u layers=%u planes=%u visible=%ux%u",
                   surface != nullptr ? surface->id : VA_INVALID_ID, format->va_fourcc, format->name, fd, descriptor->objects[0].size,
                   static_cast<unsigned long long>(descriptor->objects[0].drm_format_modifier), has_modifier ? 1U : 0U, descriptor->num_layers, plane_count,
                   surface != nullptr ? surface->width : 0U, surface != nullptr ? surface->height : 0U);
        for (uint32_t i = 0; i < format->layer_count; i++) {
            descriptor->layers[i].drm_format      = format->layers[i].drm_format;
            descriptor->layers[i].num_planes      = 1;
            descriptor->layers[i].object_index[0] = 0;
            descriptor->layers[i].offset[0]       = static_cast<uint32_t>(plane_layouts[i].offset);
            descriptor->layers[i].pitch[0]        = static_cast<uint32_t>(plane_layouts[i].rowPitch);
            vkvv_trace("export-layer-spec",
                       "surface=%u layer=%u drm_format=0x%x aspect=0x%x planes=%u object=%u offset=%u pitch=%u layout_size=%llu row_pitch=%llu array_pitch=%llu depth_pitch=%llu "
                       "divisor=%ux%u",
                       surface != nullptr ? surface->id : VA_INVALID_ID, i, descriptor->layers[i].drm_format, format->layers[i].aspect, descriptor->layers[i].num_planes,
                       descriptor->layers[i].object_index[0], descriptor->layers[i].offset[0], descriptor->layers[i].pitch[0],
                       static_cast<unsigned long long>(plane_layouts[i].size), static_cast<unsigned long long>(plane_layouts[i].rowPitch),
                       static_cast<unsigned long long>(plane_layouts[i].arrayPitch), static_cast<unsigned long long>(plane_layouts[i].depthPitch), format->layers[i].width_divisor,
                       format->layers[i].height_divisor);
        }

        return VA_STATUS_SUCCESS;
    }

} // namespace vkvv
