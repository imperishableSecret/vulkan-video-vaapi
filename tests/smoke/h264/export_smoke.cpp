#include "va_private.h"
#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/h264/internal.h"
#include "vulkan/codecs/vp9/api.h"
#include "vulkan/codecs/vp9/internal.h"

#include <cstdint>
#include <cstdio>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace {

constexpr uint64_t h264_stream_id = 101;
constexpr uint64_t vp9_stream_id = 102;

void init_nv12_surface(
        VkvvSurface *surface,
        VASurfaceID id,
        uint64_t stream_id,
        VkVideoCodecOperationFlagsKHR codec_operation) {
    surface->id = id;
    surface->driver_instance_id = 1;
    surface->stream_id = stream_id;
    surface->codec_operation = codec_operation;
    surface->rt_format = VA_RT_FORMAT_YUV420;
    surface->width = 64;
    surface->height = 64;
    surface->fourcc = VA_FOURCC_NV12;
}

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

bool check_export_preparation_drains_unrelated_pending_work(vkvv::VulkanRuntime *runtime) {
    VkvvSurface pending_surface{};
    pending_surface.id = 901;
    pending_surface.driver_instance_id = 1;
    pending_surface.rt_format = VA_RT_FORMAT_YUV420;
    pending_surface.width = 64;
    pending_surface.height = 64;
    pending_surface.fourcc = VA_FOURCC_NV12;
    pending_surface.work_state = VKVV_SURFACE_WORK_RENDERING;
    pending_surface.sync_status = VA_STATUS_ERROR_TIMEDOUT;

    char reason[512] = {};
    {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!vkvv::ensure_command_resources(runtime, reason, sizeof(reason))) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "vkResetFences failed for export pending-work smoke: %d\n", result);
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "vkResetCommandBuffer failed for export pending-work smoke: %d\n", result);
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "vkBeginCommandBuffer failed for export pending-work smoke: %d\n", result);
            return false;
        }
        result = vkEndCommandBuffer(runtime->command_buffer);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "vkEndCommandBuffer failed for export pending-work smoke: %d\n", result);
            return false;
        }
        if (!vkvv::submit_command_buffer(runtime, reason, sizeof(reason), "export pending-work smoke")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        vkvv::track_pending_decode(runtime, &pending_surface, VK_NULL_HANDLE, 0, "export pending-work smoke");
    }

    VkvvSurface export_surface{};
    export_surface.id = 902;
    export_surface.driver_instance_id = pending_surface.driver_instance_id;
    export_surface.rt_format = pending_surface.rt_format;
    export_surface.width = pending_surface.width;
    export_surface.height = pending_surface.height;
    export_surface.fourcc = pending_surface.fourcc;
    VAStatus status = vkvv_vulkan_prepare_surface_export(runtime, &export_surface, reason, sizeof(reason));
    std::printf("%s\n", reason);

    const bool drained_pending =
        runtime->pending_surface == nullptr &&
        pending_surface.work_state == VKVV_SURFACE_WORK_READY &&
        pending_surface.sync_status == VA_STATUS_SUCCESS &&
        pending_surface.decoded;
    if (pending_surface.work_state == VKVV_SURFACE_WORK_RENDERING) {
        char completion_reason[512] = {};
        (void) vkvv_vulkan_complete_surface_work(
            runtime, &pending_surface, VA_TIMEOUT_INFINITE,
            completion_reason, sizeof(completion_reason));
        if (completion_reason[0] != '\0') {
            std::printf("%s\n", completion_reason);
        }
    }
    vkvv_vulkan_surface_destroy(runtime, &export_surface);

    if (status != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "export preparation failed during pending-work smoke\n");
        return false;
    }
    if (!drained_pending) {
        std::fprintf(stderr,
                     "export preparation used command resources while unrelated decode work was still pending\n");
        return false;
    }
    return true;
}

bool check_predecode_backup_seeding(vkvv::VulkanRuntime *runtime) {
    char reason[512] = {};
    void *session = nullptr;
    VADRMPRIMESurfaceDescriptor backup_descriptor{};
    backup_descriptor.objects[0].fd = -1;

    VkvvSurface decoded{};
    init_nv12_surface(&decoded, 903, h264_stream_id, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);

    VkvvSurface backup{};
    init_nv12_surface(&backup, 904, h264_stream_id, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);

    auto cleanup = [&]() {
        if (backup_descriptor.objects[0].fd >= 0) {
            close(backup_descriptor.objects[0].fd);
            backup_descriptor.objects[0].fd = -1;
        }
        vkvv_vulkan_surface_destroy(runtime, &backup);
        vkvv_vulkan_surface_destroy(runtime, &decoded);
        vkvv_vulkan_h264_session_destroy(runtime, session);
    };

    VAStatus status = vkvv_vulkan_prepare_surface_export(runtime, &backup, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_export_surface(
        runtime, &backup, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &backup_descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *backup_resource = static_cast<vkvv::SurfaceResource *>(backup.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        backup_resource == nullptr ||
        !backup_resource->export_resource.predecode_exported ||
        backup_resource->export_resource.predecode_seeded ||
        backup_resource->export_resource.content_generation != 0) {
        std::fprintf(stderr, "predecode export should register an unseeded backup target\n");
        cleanup();
        return false;
    }

    status = vkvv_vulkan_prepare_surface_export(runtime, &decoded, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_ensure_h264_session(runtime, session, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);
    const vkvv::DecodeImageKey decode_key = h264_decode_key(typed_session, &decoded, {64, 64});
    if (!vkvv::ensure_surface_resource(runtime, &decoded, decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        cleanup();
        return false;
    }
    auto *decoded_resource = static_cast<vkvv::SurfaceResource *>(decoded.vulkan);
    decoded.decoded = true;
    decoded_resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &decoded, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    if (!backup_resource->export_resource.predecode_exported ||
        !backup_resource->export_resource.predecode_seeded ||
        backup_resource->export_resource.black_placeholder ||
        backup_resource->export_resource.seed_source_surface_id != decoded.id ||
        backup_resource->export_resource.seed_source_generation != decoded_resource->content_generation ||
        backup_resource->export_resource.content_generation != 0 ||
        backup_resource->export_resource.layout != VK_IMAGE_LAYOUT_GENERAL) {
        std::fprintf(stderr, "decoded refresh did not seed compatible predecode backup export\n");
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool check_export_time_last_good_seeding(vkvv::VulkanRuntime *runtime) {
    char reason[512] = {};
    void *session = nullptr;
    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.objects[0].fd = -1;

    VkvvSurface decoded{};
    init_nv12_surface(&decoded, 905, h264_stream_id + 1, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);

    VkvvSurface late_export{};
    init_nv12_surface(&late_export, 906, decoded.stream_id, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);

    auto cleanup = [&]() {
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
            descriptor.objects[0].fd = -1;
        }
        vkvv_vulkan_surface_destroy(runtime, &late_export);
        vkvv_vulkan_surface_destroy(runtime, &decoded);
        vkvv_vulkan_h264_session_destroy(runtime, session);
    };

    session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        cleanup();
        return false;
    }
    VAStatus status = vkvv_vulkan_ensure_h264_session(runtime, session, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);
    const vkvv::DecodeImageKey decode_key = h264_decode_key(typed_session, &decoded, {64, 64});
    if (!vkvv::ensure_surface_resource(runtime, &decoded, decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        cleanup();
        return false;
    }
    auto *decoded_resource = static_cast<vkvv::SurfaceResource *>(decoded.vulkan);
    decoded.decoded = true;
    decoded_resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &decoded, reason, sizeof(reason));
    if (reason[0] != '\0') {
        std::printf("%s\n", reason);
    }
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    status = vkvv_vulkan_prepare_surface_export(runtime, &late_export, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_export_surface(
        runtime, &late_export, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *late_resource = static_cast<vkvv::SurfaceResource *>(late_export.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        late_resource == nullptr ||
        !late_resource->export_resource.predecode_exported ||
        !late_resource->export_resource.predecode_seeded ||
        late_resource->export_resource.black_placeholder ||
        late_resource->export_resource.seed_source_surface_id != decoded.id ||
        late_resource->export_resource.seed_source_generation != decoded_resource->content_generation) {
        std::fprintf(stderr, "predecode export did not seed from the last same-stream decoded surface before fd return\n");
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool check_untagged_export_adopts_active_decode_domain(vkvv::VulkanRuntime *runtime) {
    char reason[512] = {};
    void *session = nullptr;
    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.objects[0].fd = -1;

    VkvvDriver driver{};
    driver.driver_instance_id = 1;

    VkvvContext context{};
    context.mode = VKVV_CONTEXT_MODE_DECODE;
    context.stream_id = h264_stream_id + 2;
    context.codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    context.width = 64;
    context.height = 64;

    VkvvSurface decoded{};
    init_nv12_surface(&decoded, 909, context.stream_id,
                      VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);

    VkvvSurface late_export{};
    init_nv12_surface(&late_export, 910, 0, 0);

    auto cleanup = [&]() {
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
            descriptor.objects[0].fd = -1;
        }
        vkvv_vulkan_surface_destroy(runtime, &late_export);
        vkvv_vulkan_surface_destroy(runtime, &decoded);
        vkvv_vulkan_h264_session_destroy(runtime, session);
    };

    vkvv_driver_note_decode_domain(&driver, &context, &decoded);

    session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        cleanup();
        return false;
    }
    VAStatus status = vkvv_vulkan_ensure_h264_session(runtime, session, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);
    const vkvv::DecodeImageKey decode_key = h264_decode_key(typed_session, &decoded, {64, 64});
    if (!vkvv::ensure_surface_resource(runtime, &decoded, decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        cleanup();
        return false;
    }
    auto *decoded_resource = static_cast<vkvv::SurfaceResource *>(decoded.vulkan);
    decoded.decoded = true;
    decoded_resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &decoded, reason, sizeof(reason));
    if (reason[0] != '\0') {
        std::printf("%s\n", reason);
    }
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    if (!vkvv_driver_apply_active_decode_domain(&driver, &late_export) ||
        late_export.stream_id != context.stream_id ||
        late_export.codec_operation != context.codec_operation) {
        std::fprintf(stderr, "active decode domain did not tag the unowned export surface\n");
        cleanup();
        return false;
    }

    status = vkvv_vulkan_prepare_surface_export(runtime, &late_export, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_export_surface(
        runtime, &late_export, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *late_resource = static_cast<vkvv::SurfaceResource *>(late_export.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        late_resource == nullptr ||
        late_resource->stream_id != context.stream_id ||
        late_resource->codec_operation != context.codec_operation ||
        !late_resource->export_resource.predecode_exported ||
        !late_resource->export_resource.predecode_seeded ||
        late_resource->export_resource.black_placeholder ||
        late_resource->export_resource.seed_source_surface_id != decoded.id ||
        late_resource->export_resource.seed_source_generation != decoded_resource->content_generation) {
        std::fprintf(stderr, "untagged Chrome-style export was not seeded after active-domain tagging\n");
        cleanup();
        return false;
    }
    if (!vkvv_vulkan_surface_has_predecode_export(&late_export)) {
        std::fprintf(stderr, "seeded predecode export was not marked for synchronous decode completion\n");
        cleanup();
        return false;
    }

    const vkvv::DecodeImageKey late_decode_key = h264_decode_key(typed_session, &late_export, {64, 64});
    if (!vkvv::ensure_surface_resource(runtime, &late_export, late_decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        cleanup();
        return false;
    }
    late_resource = static_cast<vkvv::SurfaceResource *>(late_export.vulkan);
    late_export.decoded = true;
    late_resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &late_export, reason, sizeof(reason));
    if (reason[0] != '\0') {
        std::printf("%s\n", reason);
    }
    if (status != VA_STATUS_SUCCESS ||
        vkvv_vulkan_surface_has_predecode_export(&late_export) ||
        late_resource->export_resource.content_generation != late_resource->content_generation) {
        std::fprintf(stderr, "decoded pre-exported surface did not refresh and clear predecode state\n");
        cleanup();
        return false;
    }

    const VkDeviceMemory first_decoded_export_memory = late_resource->export_resource.memory;
    late_resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &late_export, reason, sizeof(reason));
    if (reason[0] != '\0') {
        std::printf("%s\n", reason);
    }
    if (status != VA_STATUS_SUCCESS ||
        late_resource->export_resource.memory != first_decoded_export_memory ||
        late_resource->export_resource.content_generation != late_resource->content_generation ||
        detached_memory_present(runtime, first_decoded_export_memory,
                                late_export.driver_instance_id, late_export.id)) {
        std::fprintf(stderr, "surface reuse rotated an exported pool fd instead of updating it in place\n");
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool check_unknown_predecode_export_uses_first_decode_domain(vkvv::VulkanRuntime *runtime) {
    char reason[512] = {};
    void *session = nullptr;
    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.objects[0].fd = -1;

    VkvvSurface surface{};
    init_nv12_surface(&surface, 911, 0, 0);

    auto cleanup = [&]() {
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
            descriptor.objects[0].fd = -1;
        }
        vkvv_vulkan_surface_destroy(runtime, &surface);
        vkvv_vulkan_h264_session_destroy(runtime, session);
    };

    VAStatus status = vkvv_vulkan_prepare_surface_export(runtime, &surface, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_export_surface(
        runtime, &surface, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *resource = static_cast<vkvv::SurfaceResource *>(surface.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        resource == nullptr ||
        !resource->export_resource.predecode_exported ||
        resource->export_resource.stream_id != 0 ||
        resource->export_resource.codec_operation != 0) {
        std::fprintf(stderr, "unknown predecode export should remain untagged before first decode\n");
        cleanup();
        return false;
    }
    const VkDeviceMemory predecode_memory = resource->export_resource.memory;

    surface.stream_id = h264_stream_id + 3;
    surface.codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_ensure_h264_session(runtime, session, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);
    const vkvv::DecodeImageKey decode_key = h264_decode_key(typed_session, &surface, {64, 64});
    if (!vkvv::ensure_surface_resource(runtime, &surface, decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        cleanup();
        return false;
    }
    resource = static_cast<vkvv::SurfaceResource *>(surface.vulkan);
    surface.decoded = true;
    resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &surface, reason, sizeof(reason));
    if (reason[0] != '\0') {
        std::printf("%s\n", reason);
    }
    if (status != VA_STATUS_SUCCESS ||
        resource->export_resource.memory != predecode_memory ||
        resource->export_resource.predecode_exported ||
        resource->export_resource.stream_id != surface.stream_id ||
        resource->export_resource.codec_operation != surface.codec_operation ||
        resource->export_resource.content_generation != resource->content_generation) {
        std::fprintf(stderr, "first decode did not retag and fill the existing unknown predecode export\n");
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool check_predecode_seed_rejects_cross_codec(vkvv::VulkanRuntime *runtime) {
    char reason[512] = {};
    void *vp9_session = nullptr;
    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.objects[0].fd = -1;

    VkvvSurface vp9_decoded{};
    init_nv12_surface(&vp9_decoded, 907, vp9_stream_id, VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR);

    VkvvSurface av1_backup{};
    init_nv12_surface(&av1_backup, 908, vp9_stream_id, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

    auto cleanup = [&]() {
        if (descriptor.objects[0].fd >= 0) {
            close(descriptor.objects[0].fd);
            descriptor.objects[0].fd = -1;
        }
        vkvv_vulkan_surface_destroy(runtime, &av1_backup);
        vkvv_vulkan_surface_destroy(runtime, &vp9_decoded);
        vkvv_vulkan_vp9_session_destroy(runtime, vp9_session);
    };

    VAStatus status = vkvv_vulkan_prepare_surface_export(runtime, &av1_backup, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_export_surface(
        runtime, &av1_backup, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor, reason, sizeof(reason));
    std::printf("%s\n", reason);
    auto *backup_resource = static_cast<vkvv::SurfaceResource *>(av1_backup.vulkan);
    if (status != VA_STATUS_SUCCESS ||
        backup_resource == nullptr ||
        !backup_resource->export_resource.predecode_exported ||
        backup_resource->export_resource.predecode_seeded ||
        !backup_resource->export_resource.black_placeholder) {
        std::fprintf(stderr, "cross-codec target should start as an unseeded black predecode export\n");
        cleanup();
        return false;
    }

    vp9_session = vkvv_vulkan_vp9_session_create();
    if (vp9_session == nullptr) {
        cleanup();
        return false;
    }
    status = vkvv_vulkan_ensure_vp9_session(runtime, vp9_session, 64, 64, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    auto *typed_session = static_cast<vkvv::VP9VideoSession *>(vp9_session);
    const vkvv::DecodeImageKey decode_key{
        .codec_operation = typed_session->video.key.codec_operation,
        .codec_profile = typed_session->video.key.codec_profile,
        .picture_format = typed_session->video.key.picture_format,
        .reference_picture_format = typed_session->video.key.reference_picture_format,
        .va_rt_format = vp9_decoded.rt_format,
        .va_fourcc = vp9_decoded.fourcc,
        .coded_extent = {64, 64},
        .usage = typed_session->video.key.image_usage,
        .create_flags = typed_session->video.key.image_create_flags,
        .tiling = typed_session->video.key.image_tiling,
        .chroma_subsampling = typed_session->video.key.chroma_subsampling,
        .luma_bit_depth = typed_session->video.key.luma_bit_depth,
        .chroma_bit_depth = typed_session->video.key.chroma_bit_depth,
    };
    if (!vkvv::ensure_surface_resource(runtime, &vp9_decoded, decode_key, reason, sizeof(reason))) {
        std::fprintf(stderr, "%s\n", reason);
        cleanup();
        return false;
    }
    auto *decoded_resource = static_cast<vkvv::SurfaceResource *>(vp9_decoded.vulkan);
    vp9_decoded.decoded = true;
    decoded_resource->content_generation++;
    status = vkvv_vulkan_refresh_surface_export(runtime, &vp9_decoded, reason, sizeof(reason));
    if (reason[0] != '\0') {
        std::printf("%s\n", reason);
    }
    if (status != VA_STATUS_SUCCESS) {
        cleanup();
        return false;
    }

    if (backup_resource->export_resource.predecode_seeded ||
        !backup_resource->export_resource.black_placeholder ||
        backup_resource->export_resource.seed_source_surface_id != VA_INVALID_ID) {
        std::fprintf(stderr, "VP9 decoded refresh incorrectly seeded an AV1 predecode export\n");
        cleanup();
        return false;
    }

    cleanup();
    return true;
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
    surface.stream_id = h264_stream_id + 10;
    surface.codec_operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;

    auto *typed_runtime = static_cast<vkvv::VulkanRuntime *>(runtime);
    std::printf("surface_export=%d\n", typed_runtime->surface_export);
    if (!check_export_preparation_drains_unrelated_pending_work(typed_runtime)) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (!check_predecode_backup_seeding(typed_runtime)) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (!check_export_time_last_good_seeding(typed_runtime)) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (!check_untagged_export_adopts_active_decode_domain(typed_runtime)) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (!check_unknown_predecode_export_uses_first_decode_domain(typed_runtime)) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }
    if (!check_predecode_seed_rejects_cross_codec(typed_runtime)) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

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
    replacement.stream_id = surface.stream_id;
    replacement.codec_operation = surface.codec_operation;
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
    foreign.stream_id = surface.stream_id;
    foreign.codec_operation = surface.codec_operation;
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
    resized.stream_id = surface.stream_id;
    resized.codec_operation = surface.codec_operation;
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
