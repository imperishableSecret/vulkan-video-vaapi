#include "vulkan_runtime_internal.h"

#include <cstdio>
#include <unistd.h>

int main(void) {
    char reason[512] = {};
    void *runtime = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }

    VAStatus status = vkvv_vulkan_ensure_h264_session(runtime, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
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

    if (descriptor.objects[0].fd >= 0) {
        close(descriptor.objects[0].fd);
    }

    surface.decoded = true;
    status = vkvv_vulkan_refresh_surface_export(runtime, &surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    descriptor = {};
    status = vkvv_vulkan_export_surface(
        runtime, &surface, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (descriptor.objects[0].fd >= 0) {
        close(descriptor.objects[0].fd);
    }

    vkvv_vulkan_surface_destroy(runtime, &surface);
    vkvv_vulkan_runtime_destroy(runtime);
    return 0;
}
