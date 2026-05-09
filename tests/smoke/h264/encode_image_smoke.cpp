#include "va/private.h"
#include "vulkan/runtime_internal.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

    bool check(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
        }
        return condition;
    }

    bool check_va(VAStatus status, VAStatus expected, const char* label) {
        if (status != expected) {
            std::fprintf(stderr, "%s: expected %s got %s\n", label, vaErrorStr(expected), vaErrorStr(status));
            return false;
        }
        return true;
    }

    VASurfaceID add_test_surface(VkvvDriver* drv, unsigned int width, unsigned int height) {
        auto* surface = new (std::nothrow) VkvvSurface();
        if (surface == nullptr) {
            return VA_INVALID_ID;
        }
        surface->driver_instance_id = 17;
        surface->rt_format          = VA_RT_FORMAT_YUV420;
        surface->width              = width;
        surface->height             = height;
        surface->fourcc             = VA_FOURCC_NV12;
        surface->work_state         = VKVV_SURFACE_WORK_READY;
        surface->sync_status        = VA_STATUS_SUCCESS;
        const VASurfaceID id        = vkvv_object_add(drv, VKVV_OBJECT_SURFACE, surface);
        if (id == VA_INVALID_ID) {
            delete surface;
            return VA_INVALID_ID;
        }
        surface->id = id;
        return id;
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
        std::printf("H.264 encode runtime unavailable, skipping encode image smoke\n");
        vkvv_vulkan_runtime_destroy(runtime_ptr);
        unsetenv("VKVV_ENABLE_ENCODE");
        return 0;
    }

    bool            ok = true;
    VkvvDriver      drv{};
    VADriverContext ctx{};
    drv.next_id            = 1;
    drv.driver_instance_id = 17;
    drv.vulkan             = runtime_ptr;
    ctx.pDriverData        = &drv;

    VAImageFormat format{};
    format.fourcc         = VA_FOURCC_NV12;
    format.byte_order     = VA_LSB_FIRST;
    format.bits_per_pixel = 12;

    VAImage image{};
    ok = check_va(vkvvCreateImage(&ctx, &format, 64, 64, &image), VA_STATUS_SUCCESS, "create NV12 image") && ok;
    ok = check(image.image_id != VA_INVALID_ID && image.buf != VA_INVALID_ID && image.num_planes == 2, "image metadata was not initialized") && ok;
    ok = check(image.pitches[0] == 64 && image.pitches[1] == 64 && image.offsets[1] == 4096 && image.data_size == 6144, "NV12 image layout was unexpected") && ok;

    void* mapped = nullptr;
    ok           = check_va(vkvvMapBuffer(&ctx, image.buf, &mapped), VA_STATUS_SUCCESS, "map image buffer") && ok;
    if (mapped != nullptr) {
        std::memset(mapped, 0x10, image.offsets[1]);
        std::memset(static_cast<unsigned char*>(mapped) + image.offsets[1], 0x80, image.data_size - image.offsets[1]);
    }
    ok = check_va(vkvvUnmapBuffer(&ctx, image.buf), VA_STATUS_SUCCESS, "unmap image buffer") && ok;

    const VASurfaceID surface_id = add_test_surface(&drv, 64, 64);
    ok                           = check(surface_id != VA_INVALID_ID, "failed to add test surface") && ok;
    ok                           = check_va(vkvvPutImage(&ctx, surface_id, image.image_id, 0, 0, 64, 64, 0, 0, 64, 64), VA_STATUS_SUCCESS, "put full NV12 image") && ok;

    auto* surface  = static_cast<VkvvSurface*>(vkvv_object_get(&drv, surface_id, VKVV_OBJECT_SURFACE));
    auto* resource = surface != nullptr ? static_cast<vkvv::SurfaceResource*>(surface->vulkan) : nullptr;
    ok             = check(resource != nullptr && resource->image != VK_NULL_HANDLE && resource->layout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
                           "put image did not upload into an encode source image") &&
        ok;
    ok = check(resource != nullptr && resource->content_generation == 1 && (surface->role_flags & VKVV_SURFACE_ROLE_ENCODE_INPUT) != 0,
               "put image did not mark encode input state") &&
        ok;

    ok = check_va(vkvvPutImage(&ctx, surface_id, image.image_id, 0, 0, 32, 64, 0, 0, 32, 64), VA_STATUS_ERROR_UNIMPLEMENTED, "cropped put image") && ok;

    ok = check_va(vkvvDestroyImage(&ctx, image.image_id), VA_STATUS_SUCCESS, "destroy image") && ok;
    ok = check(vkvv_object_get(&drv, image.image_id, VKVV_OBJECT_IMAGE) == nullptr && vkvv_object_get(&drv, image.buf, VKVV_OBJECT_BUFFER) == nullptr,
               "destroy image did not release image buffer") &&
        ok;

    if (surface != nullptr) {
        vkvv_vulkan_surface_destroy(runtime_ptr, surface);
    }
    vkvv_object_clear(&drv);
    vkvv_vulkan_runtime_destroy(runtime_ptr);
    unsetenv("VKVV_ENABLE_ENCODE");

    if (!ok) {
        return 1;
    }
    std::printf("H.264 encode image smoke passed\n");
    return 0;
}
