#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/h264/internal.h"

#include <cstdint>
#include <cstdio>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

vkvv::DecodeImageKey h264_decode_key(
        const vkvv::H264VideoSession *session,
        const VkvvSurface *surface,
        VkExtent2D coded_extent) {
    return {
        .codec_operation = session->video.key.codec_operation,
        .codec_profile = session->video.key.codec_profile,
        .picture_format = session->video.key.picture_format,
        .reference_picture_format = session->video.key.reference_picture_format,
        .va_rt_format = surface->rt_format,
        .va_fourcc = surface->fourcc,
        .coded_extent = coded_extent,
        .usage = session->video.key.image_usage,
        .create_flags = session->video.key.image_create_flags,
        .tiling = session->video.key.image_tiling,
        .chroma_subsampling = session->video.key.chroma_subsampling,
        .luma_bit_depth = session->video.key.luma_bit_depth,
        .chroma_bit_depth = session->video.key.chroma_bit_depth,
    };
}

bool detached_memory_present(
        const vkvv::VulkanRuntime *runtime,
        VkDeviceMemory memory,
        uint64_t driver_instance_id,
        VASurfaceID surface_id) {
    for (const vkvv::ExportResource &resource : runtime->detached_exports) {
        if (resource.memory == memory &&
            resource.driver_instance_id == driver_instance_id &&
            resource.owner_surface_id == surface_id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(void) {
    char reason[512] = {};
    void *runtime = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }
    void *session = nullptr;

    VkvvSurface surface{};
    surface.id = 77;
    surface.driver_instance_id = 1;
    surface.rt_format = VA_RT_FORMAT_YUV420;
    surface.width = 64;
    surface.height = 64;
    surface.fourcc = VA_FOURCC_NV12;

    auto *typed_runtime = static_cast<vkvv::VulkanRuntime *>(runtime);
    std::printf("surface_export=%d\n", typed_runtime->surface_export);
    VAStatus status = vkvv_vulkan_prepare_surface_export(runtime, &surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        std::printf("%s\n", reason);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    VkvvSurface p010_surface{};
    p010_surface.id = 78;
    p010_surface.driver_instance_id = surface.driver_instance_id;
    p010_surface.rt_format = VA_RT_FORMAT_YUV420_10;
    p010_surface.width = 64;
    p010_surface.height = 64;
    p010_surface.fourcc = VA_FOURCC_P010;
    status = vkvv_vulkan_prepare_surface_export(runtime, &p010_surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "P010 export preparation should succeed once the format/export path is wired\n");
        vkvv_vulkan_surface_destroy(runtime, &p010_surface);
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    auto *p010_resource = static_cast<vkvv::SurfaceResource *>(p010_surface.vulkan);
    if (p010_resource == nullptr ||
        p010_resource->export_resource.image == VK_NULL_HANDLE ||
        p010_resource->export_resource.layout != VK_IMAGE_LAYOUT_GENERAL ||
        p010_resource->export_resource.content_generation != 0) {
        std::fprintf(stderr, "P010 export shadow should be initialized before first decode\n");
        vkvv_vulkan_surface_destroy(runtime, &p010_surface);
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    VADRMPRIMESurfaceDescriptor p010_descriptor{};
    p010_descriptor.objects[0].fd = -1;
    status = vkvv_vulkan_export_surface(
        runtime, &p010_surface, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &p010_descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS ||
        p010_descriptor.fourcc != VA_FOURCC_P010 ||
        p010_descriptor.num_objects != 1 ||
        p010_descriptor.num_layers != 2 ||
        p010_descriptor.layers[0].drm_format != DRM_FORMAT_R16 ||
        p010_descriptor.layers[1].drm_format != DRM_FORMAT_GR1616) {
        std::fprintf(stderr, "P010 descriptor builder returned an unexpected DRM PRIME shape\n");
        if (p010_descriptor.objects[0].fd >= 0) {
            close(p010_descriptor.objects[0].fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &p010_surface);
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (p010_descriptor.objects[0].fd >= 0) {
        close(p010_descriptor.objects[0].fd);
    }
    vkvv_vulkan_surface_destroy(runtime, &p010_surface);

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
        resource->image != VK_NULL_HANDLE ||
        resource->memory != VK_NULL_HANDLE ||
        resource->view != VK_NULL_HANDLE ||
        resource->coded_extent.width != 64 ||
        resource->coded_extent.height != 64 ||
        resource->visible_extent.width != 64 ||
        resource->visible_extent.height != 64 ||
        resource->va_fourcc != VA_FOURCC_NV12 ||
        resource->allocation_size != 0 ||
        resource->export_resource.allocation_size == 0 ||
        resource->export_resource.layout != VK_IMAGE_LAYOUT_GENERAL ||
        resource->export_resource.content_generation != 0 ||
        !resource->export_resource.exported) {
        std::fprintf(stderr, "export preparation should allocate only an export shadow before decode\n");
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    VkDeviceMemory first_export_memory = resource->export_resource.memory;
    const VkDeviceSize first_export_size = resource->export_resource.allocation_size;
    const int first_fd = descriptor.objects[0].fd;

    session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    status = vkvv_vulkan_ensure_h264_session(runtime, session, 64, 64, reason, sizeof(reason));
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

    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);
    const vkvv::DecodeImageKey decode_key = h264_decode_key(typed_session, &surface, {64, 64});
    if (!vkvv::ensure_surface_resource(typed_runtime, &surface, decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    resource = static_cast<vkvv::SurfaceResource *>(surface.vulkan);
    if (resource == nullptr ||
        resource->image == VK_NULL_HANDLE ||
        resource->allocation_size == 0 ||
        resource->export_resource.memory != first_export_memory ||
        resource->decode_key.codec_operation != VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR ||
        resource->decode_key.picture_format != typed_session->video.key.picture_format ||
        resource->decode_key.va_fourcc != VA_FOURCC_NV12 ||
        resource->decode_key.coded_extent.width != 64 ||
        resource->decode_key.coded_extent.height != 64) {
        std::fprintf(stderr, "decode image creation did not preserve the pre-exported shadow image\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    surface.decoded = true;
    resource->content_generation++;
    vkvv::DecodeImageKey larger_key = decode_key;
    larger_key.coded_extent = {128, 64};
    if (vkvv::ensure_surface_resource(typed_runtime, &surface, larger_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "decoded reference accepted a changed decode image key\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

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
        resource->export_resource.memory != first_export_memory ||
        resource->export_resource.allocation_size == 0 ||
        resource->export_resource.content_generation != resource->content_generation) {
        std::fprintf(stderr, "export refresh did not update the previously exported shadow image in place\n");
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
    if (typed_runtime->detached_exports.empty() ||
        typed_runtime->detached_export_memory_bytes == 0 ||
        typed_runtime->detached_exports.back().memory != first_export_memory) {
        std::fprintf(stderr, "destroying the VA surface should detach the exported shadow image into the runtime pool\n");
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

    const VkDeviceSize detached_bytes_before_reattach = typed_runtime->detached_export_memory_bytes;
    const size_t detached_count_before_reattach = typed_runtime->detached_exports.size();
    VkvvSurface replacement{};
    replacement.id = surface.id;
    replacement.driver_instance_id = surface.driver_instance_id;
    replacement.rt_format = surface.rt_format;
    replacement.width = surface.width;
    replacement.height = surface.height;
    replacement.fourcc = surface.fourcc;
    status = vkvv_vulkan_prepare_surface_export(runtime, &replacement, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *replacement_resource = static_cast<vkvv::SurfaceResource *>(replacement.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        replacement_resource == nullptr ||
        replacement_resource->export_resource.memory != first_export_memory ||
        replacement_resource->export_resource.content_generation != 0 ||
        replacement_resource->export_resource.layout != VK_IMAGE_LAYOUT_GENERAL ||
        first_export_size == 0 ||
        detached_bytes_before_reattach < first_export_size ||
        typed_runtime->detached_export_memory_bytes + first_export_size != detached_bytes_before_reattach ||
        detached_count_before_reattach == 0 ||
        typed_runtime->detached_exports.size() + 1 != detached_count_before_reattach) {
        std::fprintf(stderr, "replacement VA surface did not reattach its detached exported shadow image\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        if (refreshed_fd >= 0) {
            close(refreshed_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &replacement);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    vkvv_vulkan_surface_destroy(runtime, &replacement);

    VkvvSurface foreign{};
    foreign.id = surface.id;
    foreign.driver_instance_id = surface.driver_instance_id + 1;
    foreign.rt_format = surface.rt_format;
    foreign.width = surface.width;
    foreign.height = surface.height;
    foreign.fourcc = surface.fourcc;
    status = vkvv_vulkan_prepare_surface_export(runtime, &foreign, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *foreign_resource = static_cast<vkvv::SurfaceResource *>(foreign.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        foreign_resource == nullptr ||
        foreign_resource->export_resource.memory == VK_NULL_HANDLE ||
        foreign_resource->export_resource.memory == first_export_memory) {
        std::fprintf(stderr, "foreign driver namespace reused a detached export from another driver instance\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        if (refreshed_fd >= 0) {
            close(refreshed_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &foreign);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    vkvv_vulkan_surface_destroy(runtime, &foreign);

    VkvvSurface resized{};
    resized.id = surface.id;
    resized.driver_instance_id = surface.driver_instance_id;
    resized.rt_format = surface.rt_format;
    resized.width = 128;
    resized.height = 64;
    resized.fourcc = surface.fourcc;
    status = vkvv_vulkan_prepare_surface_export(runtime, &resized, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *resized_resource = static_cast<vkvv::SurfaceResource *>(resized.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        resized_resource == nullptr ||
        resized_resource->export_resource.memory == VK_NULL_HANDLE ||
        detached_memory_present(typed_runtime, first_export_memory, surface.driver_instance_id, surface.id)) {
        std::fprintf(stderr, "resized same-id surface reused or retained an incompatible detached export\n");
        if (first_fd >= 0) {
            close(first_fd);
        }
        if (refreshed_fd >= 0) {
            close(refreshed_fd);
        }
        vkvv_vulkan_surface_destroy(runtime, &resized);
        vkvv_vulkan_h264_session_destroy(runtime, session);
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    vkvv_vulkan_surface_destroy(runtime, &resized);
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
