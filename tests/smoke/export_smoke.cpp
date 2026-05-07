#include "vulkan_runtime_internal.h"

#include <cstdio>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    char reason[512] = {};
    void *runtime = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }
    void *session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VAStatus status = vkvv_vulkan_ensure_h264_session(runtime, session, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VkvvSurface surface{};
    surface.rt_format = VA_RT_FORMAT_YUV420;
    surface.width = 64;
    surface.height = 64;
    surface.fourcc = VA_FOURCC_NV12;
    surface.dpb_slot = -1;

    auto *typed_runtime = static_cast<vkvv::VulkanRuntime *>(runtime);
    std::printf("surface_export=%d picture_format=%d image_tiling=%d\n",
                typed_runtime->surface_export,
                typed_runtime->h264_picture_format,
                typed_runtime->h264_image_tiling);
    status = vkvv_vulkan_prepare_surface_export(runtime, &surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        std::printf("%s\n", reason);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VkvvSurface p010_surface{};
    p010_surface.rt_format = VA_RT_FORMAT_YUV420_10;
    p010_surface.width = 64;
    p010_surface.height = 64;
    p010_surface.fourcc = VA_FOURCC_P010;
    p010_surface.dpb_slot = -1;
    status = vkvv_vulkan_prepare_surface_export(runtime, &p010_surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT) {
        std::fprintf(stderr, "P010 export preparation should stay unsupported until a 10-bit decode path exists\n");
        vkvv_vulkan_surface_destroy(runtime, &p010_surface);
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VADRMPRIMESurfaceDescriptor invalid_descriptor{};
    status = vkvv_vulkan_export_surface(
        runtime, &surface, VA_EXPORT_SURFACE_READ_ONLY, &invalid_descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        std::fprintf(stderr, "export without separate layers should fail validation\n");
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    status = vkvv_vulkan_export_surface(
        runtime, &surface, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &invalid_descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        std::fprintf(stderr, "read-write export should fail validation\n");
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VADRMPRIMESurfaceDescriptor descriptor{};
    status = vkvv_vulkan_export_surface(
        runtime, &surface, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (descriptor.fourcc != VA_FOURCC_NV12 ||
        descriptor.num_objects != 1 ||
        descriptor.num_layers != 2 ||
        descriptor.layers[0].drm_format != DRM_FORMAT_R8 ||
        descriptor.layers[0].num_planes != 1 ||
        descriptor.layers[1].drm_format != DRM_FORMAT_GR88 ||
        descriptor.layers[1].num_planes != 1) {
        std::fprintf(stderr, "NV12 descriptor builder returned an unexpected DRM PRIME shape\n");
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    std::printf("objects=%u layers=%u fd=%d size=%u y_offset=%u y_pitch=%u uv_offset=%u uv_pitch=%u modifier=0x%llx\n",
                descriptor.num_objects,
                descriptor.num_layers,
                descriptor.objects[0].fd,
                descriptor.objects[0].size,
                descriptor.layers[0].offset[0],
                descriptor.layers[0].pitch[0],
                descriptor.layers[1].offset[0],
                descriptor.layers[1].pitch[0],
                static_cast<unsigned long long>(descriptor.objects[0].drm_format_modifier));

    auto *resource = static_cast<vkvv::SurfaceResource *>(surface.vulkan);
    if (resource == nullptr ||
        resource->coded_extent.width != 64 ||
        resource->coded_extent.height != 64 ||
        resource->visible_extent.width != 64 ||
        resource->visible_extent.height != 64 ||
        resource->va_fourcc != VA_FOURCC_NV12 ||
        resource->allocation_size == 0 ||
        resource->export_resource.allocation_size == 0 ||
        !resource->export_resource.exported) {
        std::fprintf(stderr, "surface resource tracking/accounting is incomplete\n");
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    VkDeviceMemory first_export_memory = resource->export_resource.memory;
    const int first_fd = descriptor.objects[0].fd;

    surface.decoded = true;
    status = vkvv_vulkan_refresh_surface_export(runtime, &surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (first_fd < 0 || fcntl(first_fd, F_GETFD) < 0 ||
        resource->retired_exports.size() != 1 ||
        resource->retired_exports[0].memory != first_export_memory ||
        resource->export_resource.memory == first_export_memory ||
        resource->export_resource.allocation_size == 0) {
        std::fprintf(stderr, "export refresh did not preserve the previously exported shadow image\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VADRMPRIMESurfaceDescriptor refreshed_descriptor{};
    status = vkvv_vulkan_export_surface(
        runtime, &surface, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &refreshed_descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    const int refreshed_fd = refreshed_descriptor.objects[0].fd;
    vkvv_vulkan_surface_destroy(runtime, &surface);
    if ((first_fd >= 0 && fcntl(first_fd, F_GETFD) < 0) ||
        (refreshed_fd >= 0 && fcntl(refreshed_fd, F_GETFD) < 0)) {
        std::fprintf(stderr, "destroying the VA surface closed an exported dma-buf fd\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        if (refreshed_fd >= 0) {
            close(refreshed_fd);
        }
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (first_fd >= 0) {
        close(first_fd);
    }
    if (refreshed_fd >= 0) {
        close(refreshed_fd);
    }

    vkvv_vulkan_h264_session_destroy(runtime, session);
    vkvv_vulkan_runtime_destroy(runtime);
    return 0;
}
