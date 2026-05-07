#include "../vulkan_runtime_internal.h"

#include <cstdio>
#include <limits>
#include <mutex>

namespace vkvv {

bool ensure_command_resources(VulkanRuntime *runtime, char *reason, size_t reason_size) {
    if (runtime->command_buffer != VK_NULL_HANDLE && runtime->fence != VK_NULL_HANDLE) {
        return true;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = runtime->decode_queue_family;

    VkResult result = vkCreateCommandPool(runtime->device, &pool_info, nullptr, &runtime->command_pool);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateCommandPool for H.264 decode failed: %d", result);
        return false;
    }

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = runtime->command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(runtime->device, &allocate_info, &runtime->command_buffer);
    if (result != VK_SUCCESS) {
        runtime->destroy_command_resources();
        std::snprintf(reason, reason_size, "vkAllocateCommandBuffers for H.264 decode failed: %d", result);
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(runtime->device, &fence_info, nullptr, &runtime->fence);
    if (result != VK_SUCCESS) {
        runtime->destroy_command_resources();
        std::snprintf(reason, reason_size, "vkCreateFence for H.264 decode failed: %d", result);
        return false;
    }

    return true;
}

bool submit_command_buffer_and_wait(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &runtime->command_buffer;

    VkResult result = vkQueueSubmit(runtime->decode_queue, 1, &submit, runtime->fence);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkQueueSubmit for %s failed: %d", operation, result);
        return false;
    }

    result = vkWaitForFences(runtime->device, 1, &runtime->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkWaitForFences for %s failed: %d", operation, result);
        return false;
    }

    return true;
}

bool reset_h264_session(
        VulkanRuntime *runtime,
        H264VideoSession *session,
        VkVideoSessionParametersKHR parameters,
        char *reason,
        size_t reason_size) {
    std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
    if (!ensure_command_resources(runtime, reason, reason_size)) {
        return false;
    }

    VkResult result = vkResetFences(runtime->device, 1, &runtime->fence);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkResetFences for H.264 session reset failed: %d", result);
        return false;
    }
    result = vkResetCommandBuffer(runtime->command_buffer, 0);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkResetCommandBuffer for H.264 session reset failed: %d", result);
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(runtime->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkBeginCommandBuffer for H.264 session reset failed: %d", result);
        return false;
    }

    VkVideoBeginCodingInfoKHR video_begin{};
    video_begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
    video_begin.videoSession = session->video.session;
    video_begin.videoSessionParameters = parameters;
    runtime->cmd_begin_video_coding(runtime->command_buffer, &video_begin);

    VkVideoCodingControlInfoKHR control{};
    control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    runtime->cmd_control_video_coding(runtime->command_buffer, &control);

    VkVideoEndCodingInfoKHR video_end{};
    video_end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
    runtime->cmd_end_video_coding(runtime->command_buffer, &video_end);

    result = vkEndCommandBuffer(runtime->command_buffer);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkEndCommandBuffer for H.264 session reset failed: %d", result);
        return false;
    }

    if (!submit_command_buffer_and_wait(runtime, reason, reason_size, "H.264 session reset")) {
        return false;
    }

    session->video.initialized = true;
    return true;
}

} // namespace vkvv
