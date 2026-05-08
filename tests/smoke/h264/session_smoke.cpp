#include "vulkan/codecs/h264/api.h"
#include "vulkan/codecs/h264/internal.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

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
    if (!check(video.key.image_usage != 0,
               "H.264 session key did not record decode image usage")) {
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

bool ensure_upload(
        vkvv::VulkanRuntime *runtime,
        vkvv::H264VideoSession *session,
        const std::vector<uint8_t> &bytes) {
    char reason[512] = {};
    if (!check(vkvv::ensure_bitstream_upload_buffer(
                   runtime,
                   vkvv::h264_profile_spec,
                   bytes.data(),
                   bytes.size(),
                   session->bitstream_size_alignment,
                   VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
                   &session->upload,
                   "H.264 smoke bitstream",
                   reason,
                   sizeof(reason)),
               "ensure_bitstream_upload_buffer failed")) {
        std::fprintf(stderr, "%s\n", reason);
        return false;
    }
    if (!check(session->upload.buffer != VK_NULL_HANDLE &&
               session->upload.memory != VK_NULL_HANDLE &&
               session->upload.size >= bytes.size() &&
               session->upload.capacity >= session->upload.size,
               "upload buffer was not populated correctly")) {
        return false;
    }
    return true;
}

bool check_async_completion(vkvv::VulkanRuntime *runtime) {
    VkvvSurface surface{};
    surface.work_state = VKVV_SURFACE_WORK_RENDERING;
    surface.sync_status = VA_STATUS_ERROR_TIMEDOUT;

    char reason[512] = {};
    {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (!check(vkvv::ensure_command_resources(runtime, reason, sizeof(reason)),
                   "ensure_command_resources failed")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }

        VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
        if (!check(result == VK_SUCCESS, "vkResetFences failed for async smoke")) {
            return false;
        }
        result = vkResetCommandBuffer(runtime->command_buffer, 0);
        if (!check(result == VK_SUCCESS, "vkResetCommandBuffer failed for async smoke")) {
            return false;
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
        if (!check(result == VK_SUCCESS, "vkBeginCommandBuffer failed for async smoke")) {
            return false;
        }
        result = vkEndCommandBuffer(runtime->command_buffer);
        if (!check(result == VK_SUCCESS, "vkEndCommandBuffer failed for async smoke")) {
            return false;
        }
        if (!check(vkvv::submit_command_buffer(runtime, reason, sizeof(reason), "async smoke"),
                   "submit_command_buffer failed for async smoke")) {
            std::fprintf(stderr, "%s\n", reason);
            return false;
        }
        vkvv::track_pending_decode(runtime, &surface, VK_NULL_HANDLE, 0, "async smoke");
    }

    VAStatus status = vkvv_vulkan_complete_surface_work(
        runtime, &surface, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
    if (!check(status == VA_STATUS_SUCCESS, "async surface completion failed")) {
        std::fprintf(stderr, "%s\n", reason);
        return false;
    }
    return check(surface.work_state == VKVV_SURFACE_WORK_READY &&
                 surface.sync_status == VA_STATUS_SUCCESS &&
                 surface.decoded,
                 "async surface completion did not mark the surface ready");
}

bool check_device_lost_fast_fail(vkvv::VulkanRuntime *runtime) {
    VkvvSurface surface{};
    surface.work_state = VKVV_SURFACE_WORK_RENDERING;
    surface.sync_status = VA_STATUS_ERROR_TIMEDOUT;

    char reason[512] = {};
    {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        runtime->pending_surface = &surface;
        runtime->pending_parameters = VK_NULL_HANDLE;
        runtime->pending_upload_allocation_size = 0;
        std::snprintf(runtime->pending_operation, sizeof(runtime->pending_operation), "device-lost smoke");
    }

    runtime->device_lost = true;
    if (!check(!vkvv::ensure_command_resources(runtime, reason, sizeof(reason)),
               "device-lost runtime accepted new command resources")) {
        return false;
    }

    VAStatus status = vkvv_vulkan_complete_surface_work(
        runtime, &surface, VA_TIMEOUT_INFINITE, reason, sizeof(reason));
    if (!check(status == VA_STATUS_ERROR_OPERATION_FAILED,
               "device-lost pending work did not fail fast")) {
        std::fprintf(stderr, "%s\n", reason);
        return false;
    }
    return check(runtime->pending_surface == nullptr &&
                 surface.work_state == VKVV_SURFACE_WORK_READY &&
                 surface.sync_status == VA_STATUS_ERROR_OPERATION_FAILED,
                 "device-lost pending work was not cleared deterministically");
}

bool check_h264_dpb_slots() {
    vkvv::H264VideoSession session{};
    bool used_slots[vkvv::max_h264_dpb_slots] = {};

    const int first_slot = vkvv::allocate_dpb_slot(&session, used_slots);
    if (!check(first_slot == 0, "first H.264 DPB slot allocation did not start at zero")) {
        return false;
    }
    vkvv::h264_set_dpb_slot_for_surface(&session, 41, first_slot);
    if (!check(vkvv::h264_dpb_slot_for_surface(&session, 41) == first_slot,
               "H.264 DPB slot lookup did not return the stored surface slot")) {
        return false;
    }

    used_slots[first_slot] = true;
    const int second_slot = vkvv::allocate_dpb_slot(&session, used_slots);
    if (!check(second_slot == 1, "H.264 DPB slot allocation did not skip a used slot")) {
        return false;
    }
    vkvv::h264_set_dpb_slot_for_surface(&session, 41, second_slot);
    return check(vkvv::h264_dpb_slot_for_surface(&session, 41) == second_slot,
                 "H.264 DPB slot update did not replace the old surface slot");
}

} // namespace

int main(void) {
    bool ok = check_h264_dpb_slots();

    char reason[512] = {};
    void *runtime = vkvv_vulkan_runtime_create(reason, sizeof(reason));
    std::printf("%s\n", reason);
    if (runtime == nullptr) {
        return 1;
    }
    auto *typed_runtime = static_cast<vkvv::VulkanRuntime *>(runtime);
    ok = check(
        typed_runtime->decode_queue_family != vkvv::invalid_queue_family,
        "runtime did not select a decode queue family");
    ok = check(
        (typed_runtime->enabled_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0,
        "runtime did not enable H.264 through codec-driven selection") && ok;
    ok = check(
        (typed_runtime->probed_decode_operations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) != 0,
        "runtime did not record probed H.264 decode support") && ok;
    ok = check(
        typed_runtime->enabled_encode_operations == 0 &&
            typed_runtime->probed_encode_operations == 0,
        "runtime should keep encode operation sets empty before encode probing is wired") && ok;

    void *session = vkvv_vulkan_h264_session_create();
    if (session == nullptr) {
        vkvv_vulkan_runtime_destroy(runtime);
        return 1;
    }

    ok = ensure_session(runtime, session, 64, 64, 256, 256) && ok;
    auto *typed_session = static_cast<vkvv::H264VideoSession *>(session);
    std::vector<uint8_t> first_upload(256, 0x11);
    ok = ensure_upload(typed_runtime, typed_session, first_upload) && ok;
    const VkBuffer first_upload_buffer = typed_session->upload.buffer;
    const VkDeviceMemory first_upload_memory = typed_session->upload.memory;
    const VkDeviceSize first_upload_capacity = typed_session->upload.capacity;
    const VkDeviceSize first_upload_allocation = typed_session->upload.allocation_size;

    std::vector<uint8_t> smaller_upload(128, 0x22);
    ok = ensure_upload(typed_runtime, typed_session, smaller_upload) && ok;
    ok = check(typed_session->upload.buffer == first_upload_buffer &&
               typed_session->upload.memory == first_upload_memory &&
               typed_session->upload.capacity == first_upload_capacity &&
               typed_session->upload.allocation_size == first_upload_allocation,
               "smaller H.264 upload did not reuse the existing buffer") && ok;

    std::vector<uint8_t> larger_upload(static_cast<size_t>(first_upload_capacity + 1), 0x33);
    ok = ensure_upload(typed_runtime, typed_session, larger_upload) && ok;
    ok = check(typed_session->upload.capacity > first_upload_capacity,
               "larger H.264 upload did not grow the reusable buffer") && ok;
    ok = check_async_completion(typed_runtime) && ok;

    ok = ensure_session(runtime, session, 640, 360, 640, 368) && ok;
    const VkVideoSessionKHR grown_session = typed_session->video.session;
    ok = check(grown_session != VK_NULL_HANDLE, "grown H.264 session handle was not created") && ok;

    ok = ensure_session(runtime, session, 320, 180, 640, 368) && ok;
    ok = check(typed_session->video.session == grown_session, "H.264 session unexpectedly shrank or recreated") && ok;

    ok = check_device_lost_fast_fail(typed_runtime) && ok;

    vkvv_vulkan_h264_session_destroy(runtime, session);
    vkvv_vulkan_runtime_destroy(runtime);
    if (!ok) {
        return 1;
    }

    std::printf("H.264 session sizing smoke passed\n");
    return 0;
}
