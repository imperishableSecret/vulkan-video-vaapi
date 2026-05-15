#include "va/private.h"

#include <cstdint>
#include <cstdio>
#include <va/va.h>
#include <vulkan/vulkan.h>

namespace {

    constexpr uint64_t stream_id    = 121;
    constexpr uint32_t smoke_width  = 1024;
    constexpr uint32_t smoke_height = 576;

    void init_surface(VkvvSurface* surface, VASurfaceID id, uint64_t stream, unsigned int codec, uint32_t width, uint32_t height) {
        surface->id                 = id;
        surface->driver_instance_id = 1;
        surface->stream_id          = stream;
        surface->codec_operation    = codec;
        surface->rt_format          = VA_RT_FORMAT_YUV420;
        surface->width              = width;
        surface->height             = height;
        surface->fourcc             = VA_FOURCC_NV12;
    }

    bool check_context_only_domain_tags_matching_pool_surface() {
        VkvvDriver driver{};
        driver.driver_instance_id = 1;

        VkvvContext context{};
        context.mode            = VKVV_CONTEXT_MODE_DECODE;
        context.stream_id       = stream_id;
        context.codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        context.width           = smoke_width;
        context.height          = smoke_height;

        VkvvSurface pool_surface{};
        init_surface(&pool_surface, 301, 0, 0, smoke_width, smoke_height);

        vkvv_driver_note_decode_domain(&driver, &context, nullptr);
        if (!vkvv_driver_apply_active_decode_domain(&driver, &pool_surface) || pool_surface.stream_id != context.stream_id ||
            pool_surface.codec_operation != context.codec_operation) {
            std::fprintf(stderr, "context-only active decode domain did not tag matching pool surface\n");
            return false;
        }
        return true;
    }

    bool check_context_only_domain_rejects_mismatch_and_existing_domain() {
        VkvvDriver driver{};
        driver.driver_instance_id = 1;

        VkvvContext context{};
        context.mode            = VKVV_CONTEXT_MODE_DECODE;
        context.stream_id       = stream_id;
        context.codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        context.width           = smoke_width;
        context.height          = smoke_height;

        VkvvSurface mismatched_surface{};
        init_surface(&mismatched_surface, 302, 0, 0, smoke_width / 2, smoke_height);

        VkvvSurface tagged_surface{};
        init_surface(&tagged_surface, 303, stream_id + 1, VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR, smoke_width, smoke_height);

        vkvv_driver_note_decode_domain(&driver, &context, nullptr);
        if (vkvv_driver_apply_active_decode_domain(&driver, &mismatched_surface) || mismatched_surface.stream_id != 0 || mismatched_surface.codec_operation != 0) {
            std::fprintf(stderr, "context-only active decode domain tagged mismatched pool surface\n");
            return false;
        }
        if (vkvv_driver_apply_active_decode_domain(&driver, &tagged_surface) || tagged_surface.stream_id != stream_id + 1 ||
            tagged_surface.codec_operation != VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) {
            std::fprintf(stderr, "context-only active decode domain retagged an already-owned surface\n");
            return false;
        }
        return true;
    }

} // namespace

int main() {
    if (!check_context_only_domain_tags_matching_pool_surface()) {
        return 1;
    }
    if (!check_context_only_domain_rejects_mismatch_and_existing_domain()) {
        return 1;
    }
    std::printf("domain smoke passed\n");
    return 0;
}
