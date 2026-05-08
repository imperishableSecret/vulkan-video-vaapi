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
        std::snprintf(reason, reason_size, "vkCreateCommandPool for video command failed: %d", result);
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
        std::snprintf(reason, reason_size, "vkAllocateCommandBuffers for video command failed: %d", result);
        return false;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(runtime->device, &fence_info, nullptr, &runtime->fence);
    if (result != VK_SUCCESS) {
        runtime->destroy_command_resources();
        std::snprintf(reason, reason_size, "vkCreateFence for video command failed: %d", result);
        return false;
    }

    return true;
}

bool submit_command_buffer(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &runtime->command_buffer;

    VkResult result = vkQueueSubmit(runtime->decode_queue, 1, &submit, runtime->fence);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkQueueSubmit for %s failed: %d", operation, result);
        return false;
    }

    return true;
}

bool wait_for_command_fence(VulkanRuntime *runtime, uint64_t timeout_ns, char *reason, size_t reason_size, const char *operation) {
    VkResult result = VK_SUCCESS;
    if (timeout_ns == 0) {
        result = vkGetFenceStatus(runtime->device, runtime->fence);
        if (result == VK_NOT_READY) {
            std::snprintf(reason, reason_size, "%s is still pending", operation);
            return false;
        }
    } else {
        result = vkWaitForFences(runtime->device, 1, &runtime->fence, VK_TRUE, timeout_ns);
        if (result == VK_TIMEOUT) {
            std::snprintf(reason, reason_size, "%s timed out", operation);
            return false;
        }
    }
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkWaitForFences for %s failed: %d", operation, result);
        return false;
    }

    return true;
}

void complete_surface_status(VkvvSurface *surface, VAStatus status) {
    if (surface == nullptr || surface->destroying) {
        return;
    }
    surface->work_state = VKVV_SURFACE_WORK_READY;
    surface->sync_status = status;
}

bool submit_command_buffer_and_wait(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation) {
    if (!submit_command_buffer(runtime, reason, reason_size, operation)) {
        return false;
    }
    return wait_for_command_fence(runtime, std::numeric_limits<uint64_t>::max(), reason, reason_size, operation);
}

void track_pending_decode(
        VulkanRuntime *runtime,
        VkvvSurface *surface,
        VkVideoSessionParametersKHR parameters,
        VkDeviceSize upload_allocation_size,
        const char *operation) {
    runtime->pending_surface = surface;
    runtime->pending_parameters = parameters;
    runtime->pending_upload_allocation_size = upload_allocation_size;
    std::snprintf(runtime->pending_operation, sizeof(runtime->pending_operation), "%s", operation);
}

VAStatus complete_pending_work(
        VulkanRuntime *runtime,
        VkvvSurface *surface,
        uint64_t timeout_ns,
        char *reason,
        size_t reason_size) {
    if (runtime == nullptr) {
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    VkvvSurface *completed_surface = nullptr;
    VkVideoSessionParametersKHR completed_parameters = VK_NULL_HANDLE;
    VkDeviceSize upload_allocation_size = 0;
    char operation[64] = {};
    {
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        if (runtime->pending_surface == nullptr ||
            (surface != nullptr && runtime->pending_surface != surface)) {
            if (surface == nullptr && reason_size > 0) {
                reason[0] = '\0';
                return VA_STATUS_SUCCESS;
            }
            std::snprintf(reason, reason_size, "no matching pending Vulkan work");
            return VA_STATUS_ERROR_TIMEDOUT;
        }

        if (!wait_for_command_fence(
                runtime, timeout_ns, reason, reason_size,
                runtime->pending_operation[0] != '\0' ? runtime->pending_operation : "Vulkan decode")) {
            return VA_STATUS_ERROR_TIMEDOUT;
        }

        completed_surface = runtime->pending_surface;
        completed_parameters = runtime->pending_parameters;
        upload_allocation_size = runtime->pending_upload_allocation_size;
        std::snprintf(operation, sizeof(operation), "%s", runtime->pending_operation);
        runtime->pending_surface = nullptr;
        runtime->pending_parameters = VK_NULL_HANDLE;
        runtime->pending_upload_allocation_size = 0;
        runtime->pending_operation[0] = '\0';
    }

    if (completed_parameters != VK_NULL_HANDLE && runtime->destroy_video_session_parameters != nullptr) {
        runtime->destroy_video_session_parameters(runtime->device, completed_parameters, nullptr);
    }

    completed_surface->decoded = true;
    VAStatus status = ::vkvv_vulkan_refresh_surface_export(runtime, completed_surface, reason, reason_size);
    if (status != VA_STATUS_SUCCESS) {
        complete_surface_status(completed_surface, status);
        return status;
    }
    complete_surface_status(completed_surface, VA_STATUS_SUCCESS);
    if (reason[0] == '\0') {
        std::snprintf(reason, reason_size,
                      "%s completed asynchronously: upload_mem=%llu",
                      operation[0] != '\0' ? operation : "Vulkan decode",
                      static_cast<unsigned long long>(upload_allocation_size));
    }
    return VA_STATUS_SUCCESS;
}

} // namespace vkvv

using namespace vkvv;

VAStatus vkvv_vulkan_complete_surface_work(
        void *runtime_ptr,
        VkvvSurface *surface,
        uint64_t timeout_ns,
        char *reason,
        size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    return complete_pending_work(runtime, surface, timeout_ns, reason, reason_size);
}

VAStatus vkvv_vulkan_drain_pending_work(void *runtime_ptr, char *reason, size_t reason_size) {
    auto *runtime = static_cast<VulkanRuntime *>(runtime_ptr);
    return complete_pending_work(runtime, nullptr, std::numeric_limits<uint64_t>::max(), reason, reason_size);
}
