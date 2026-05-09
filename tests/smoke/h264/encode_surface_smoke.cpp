#include "vulkan/runtime_internal.h"

#include <cstdlib>
#include <cstdio>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    vkvv::EncodeImageKey h264_encode_key(unsigned int width, unsigned int height) {
        return {
            .codec_operation    = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
            .codec_profile      = STD_VIDEO_H264_PROFILE_IDC_HIGH,
            .picture_format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .va_rt_format       = VA_RT_FORMAT_YUV420,
            .va_fourcc          = VA_FOURCC_NV12,
            .coded_extent       = {width, height},
            .usage              = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .create_flags       = 0,
            .tiling             = VK_IMAGE_TILING_OPTIMAL,
            .chroma_subsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
            .luma_bit_depth     = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
            .chroma_bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        };
    }

    void init_surface(VkvvSurface* surface, unsigned int id, unsigned int width, unsigned int height) {
        surface->id                 = id;
        surface->driver_instance_id = 7;
        surface->stream_id          = 11;
        surface->codec_operation    = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        surface->rt_format          = VA_RT_FORMAT_YUV420;
        surface->width              = width;
        surface->height             = height;
        surface->fourcc             = VA_FOURCC_NV12;
        surface->work_state         = VKVV_SURFACE_WORK_READY;
        surface->sync_status        = VA_STATUS_SUCCESS;
    }

} // namespace

int main(void) {
    setenv("VKVV_ENABLE_ENCODE", "1", 1);

    char  reason[512]{};
    void* runtime_ptr = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime_ptr == nullptr) {
        unsetenv("VKVV_ENABLE_ENCODE");
        return 1;
    }

    auto* runtime = static_cast<vkvv::VulkanRuntime*>(runtime_ptr);
    if ((runtime->enabled_encode_operations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR) == 0) {
        std::printf("H.264 encode runtime unavailable, skipping encode surface smoke\n");
        vkvv_vulkan_runtime_destroy(runtime_ptr);
        unsetenv("VKVV_ENABLE_ENCODE");
        return 0;
    }

    bool        ok = true;
    VkvvSurface surface{};
    init_surface(&surface, 3, 64, 64);
    auto key = h264_encode_key(64, 64);
    ok       = check(vkvv::ensure_encode_input_resource(runtime, &surface, key, reason, sizeof(reason)), "ensure_encode_input_resource failed") && ok;
    if (!ok) {
        std::fprintf(stderr, "%s\n", reason);
    }

    auto* resource = static_cast<vkvv::SurfaceResource*>(surface.vulkan);
    ok             = check(resource != nullptr && resource->image != VK_NULL_HANDLE && resource->view != VK_NULL_HANDLE && resource->memory != VK_NULL_HANDLE,
                           "encode input resource did not allocate Vulkan handles") &&
        ok;
    ok = check(resource != nullptr && resource->encode_key.codec_operation == VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR &&
                   (resource->encode_key.usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) != 0 && !resource->exportable,
               "encode input resource recorded the wrong key") &&
        ok;
    ok = check(resource != nullptr && resource->allocation_size > 0 && (surface.role_flags & VKVV_SURFACE_ROLE_ENCODE_INPUT) != 0, "encode input resource accounting was empty") &&
        ok;

    const VkImage original_image = resource != nullptr ? resource->image : VK_NULL_HANDLE;
    ok       = check(vkvv::ensure_encode_input_resource(runtime, &surface, h264_encode_key(32, 32), reason, sizeof(reason)), "smaller encode input did not reuse resource") && ok;
    resource = static_cast<vkvv::SurfaceResource*>(surface.vulkan);
    ok       = check(resource != nullptr && resource->image == original_image, "smaller encode input recreated the image instead of reusing it") && ok;

    ok = check(vkvv::ensure_encode_input_resource(runtime, &surface, h264_encode_key(128, 64), reason, sizeof(reason)), "larger encode input did not recreate resource") && ok;
    resource = static_cast<vkvv::SurfaceResource*>(surface.vulkan);
    ok = check(resource != nullptr && resource->image != VK_NULL_HANDLE && resource->encode_key.coded_extent.width == 128, "larger encode input resource was not resized") && ok;

    vkvv_vulkan_surface_destroy(runtime_ptr, &surface);
    vkvv_vulkan_runtime_destroy(runtime_ptr);
    unsetenv("VKVV_ENABLE_ENCODE");

    if (!ok) {
        return 1;
    }
    std::printf("H.264 encode surface smoke passed\n");
    return 0;
}
