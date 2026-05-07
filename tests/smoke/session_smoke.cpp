#include "vulkan_runtime_internal.h"

#include <cstdio>

namespace {

bool check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool ensure_session(
        void *runtime,
        void *session,
        unsigned int width,
        unsigned int height,
        unsigned int expected_width,
        unsigned int expected_height) {
    char reason[512] = {};
    VAStatus status = vkvv_vulkan_ensure_h264_session(
        runtime, session, width, height, reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (!check(status == VA_STATUS_SUCCESS, "vkvv_vulkan_ensure_h264_session failed")) {
        return false;
    }

    const auto *typed_session = static_cast<const vkvv::H264VideoSession *>(session);
    const vkvv::VideoSession &video = typed_session->video;
    if (!check(video.session != VK_NULL_HANDLE, "H.264 session handle was not created")) {
        return false;
    }
    if (!check(video.key.codec_operation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
               "H.264 session key did not record the codec operation")) {
        return false;
    }
    if (!check(video.key.picture_format != VK_FORMAT_UNDEFINED &&
               video.key.reference_picture_format == video.key.picture_format,
               "H.264 session key did not record picture/reference formats")) {
        return false;
    }
    if (!check(video.memory_bytes > 0, "H.264 session memory accounting stayed at zero")) {
        return false;
    }
    if (!check(video.key.max_coded_extent.width == expected_width &&
               video.key.max_coded_extent.height == expected_height,
               "H.264 session extent did not match the stream-sized policy")) {
        std::fprintf(stderr, "expected=%ux%u actual=%ux%u\n",
                     expected_width, expected_height,
                     video.key.max_coded_extent.width, video.key.max_coded_extent.height);
        return false;
    }
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

    void *session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    bool ok = ensure_session(runtime, session, 64, 64, 256, 256);
    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);

    ok = ensure_session(runtime, session, 640, 360, 640, 368) && ok;
    const VkVideoSessionKHR grown_session = typed_session->video.session;
    ok = check(grown_session != VK_NULL_HANDLE, "grown H.264 session handle was not created") && ok;

    ok = ensure_session(runtime, session, 320, 180, 640, 368) && ok;
    ok = check(typed_session->video.session == grown_session, "H.264 session unexpectedly shrank or recreated") && ok;

    vkvv_vulkan_h264_session_destroy(runtime, session);
    vkvv_vulkan_runtime_destroy(runtime);
    if (!ok) {
        return 1;
    }

    std::printf("H.264 session sizing smoke passed\n");
    return 0;
}
