#include "vulkan/export/internal.h"

#include <cstdio>
#include <drm_fourcc.h>

namespace vkvv {

constexpr ExportFormatInfo nv12_export_format = {
    VA_FOURCC_NV12,
    VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
    "NV12",
    2,
    {
        {VK_IMAGE_ASPECT_PLANE_0_BIT, DRM_FORMAT_R8, 1, 1},
        {VK_IMAGE_ASPECT_PLANE_1_BIT, DRM_FORMAT_GR88, 2, 2},
    },
};

constexpr ExportFormatInfo p010_export_format = {
    VA_FOURCC_P010,
    VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
    "P010",
    2,
    {
        {VK_IMAGE_ASPECT_PLANE_0_BIT, DRM_FORMAT_R16, 1, 1},
        {VK_IMAGE_ASPECT_PLANE_1_BIT, DRM_FORMAT_GR1616, 2, 2},
    },
};

const ExportFormatInfo *export_format_for_fourcc(unsigned int fourcc) {
    switch (fourcc) {
        case VA_FOURCC_NV12:
            return &nv12_export_format;
        case VA_FOURCC_P010:
            return &p010_export_format;
        default:
            return nullptr;
    }
}

const ExportFormatInfo *export_format_for_surface(
        const VkvvSurface *surface,
        const SurfaceResource *resource,
        char *reason,
        size_t reason_size) {
    if (surface == nullptr && resource == nullptr) {
        std::snprintf(reason, reason_size, "missing surface export format source");
        return nullptr;
    }
    const unsigned int fourcc = resource != nullptr ? resource->va_fourcc : surface->fourcc;
    const ExportFormatInfo *format = export_format_for_fourcc(fourcc);
    if (format == nullptr) {
        std::snprintf(reason, reason_size, "surface export does not support VA fourcc=0x%x", fourcc);
        return nullptr;
    }
    if (resource != nullptr && resource->format != format->vk_format) {
        std::snprintf(reason, reason_size,
                      "surface export format mismatch: va_fourcc=0x%x resource_format=%d expected_format=%d",
                      fourcc, resource->format, format->vk_format);
        return nullptr;
    }
    return format;
}

VkExtent3D export_layer_extent(VkExtent2D coded_extent, const ExportLayerInfo &layer) {
    return {
        (coded_extent.width + layer.width_divisor - 1) / layer.width_divisor,
        (coded_extent.height + layer.height_divisor - 1) / layer.height_divisor,
        1,
    };
}

} // namespace vkvv
