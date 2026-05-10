#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <cstdio>
#include <limits>
#include <mutex>

namespace vkvv {

    namespace {

        void mark_device_lost(VulkanRuntime* runtime) {
            if (runtime == nullptr || runtime->device_lost.exchange(true)) {
                return;
            }
            runtime->destroy_detached_export_resources();
        }

        CommandSlot& active_slot(VulkanRuntime* runtime) {
            return runtime->command_slots[runtime->active_command_slot];
        }

        void set_active_slot_locked(VulkanRuntime* runtime, size_t index) {
            runtime->active_command_slot = index;
            runtime->command_buffer      = runtime->command_slots[index].command_buffer;
            runtime->fence               = runtime->command_slots[index].fence;
        }

        void refresh_legacy_pending_snapshot_locked(VulkanRuntime* runtime) {
            runtime->pending_surface                = nullptr;
            runtime->pending_parameters             = VK_NULL_HANDLE;
            runtime->pending_upload_allocation_size = 0;
            runtime->pending_export_refresh         = true;
            runtime->pending_operation[0]           = '\0';
            for (const CommandSlot& slot : runtime->command_slots) {
                if (slot.pending_surface == nullptr) {
                    continue;
                }
                runtime->pending_surface                = slot.pending_surface;
                runtime->pending_parameters             = slot.pending_parameters;
                runtime->pending_upload_allocation_size = slot.pending_upload_allocation_size;
                runtime->pending_export_refresh         = slot.pending_export_refresh;
                std::snprintf(runtime->pending_operation, sizeof(runtime->pending_operation), "%s", slot.pending_operation);
                return;
            }
        }

        size_t pending_work_count_locked(const VulkanRuntime* runtime) {
            size_t count = 0;
            for (const CommandSlot& slot : runtime->command_slots) {
                if (slot.pending_surface != nullptr) {
                    count++;
                }
            }
            return count;
        }

        CommandSlot* find_pending_slot_locked(VulkanRuntime* runtime, const VkvvSurface* surface, size_t* index) {
            for (size_t i = 0; i < command_slot_count; i++) {
                CommandSlot& slot = runtime->command_slots[i];
                if (slot.pending_surface == nullptr) {
                    continue;
                }
                if (surface != nullptr && slot.pending_surface != surface) {
                    continue;
                }
                if (index != nullptr) {
                    *index = i;
                }
                return &slot;
            }
            return nullptr;
        }

        bool select_idle_command_slot_locked(VulkanRuntime* runtime, char* reason, size_t reason_size) {
            for (size_t attempt = 0; attempt < command_slot_count; attempt++) {
                const size_t index = (runtime->active_command_slot + attempt) % command_slot_count;
                CommandSlot& slot  = runtime->command_slots[index];
                if (slot.pending_surface != nullptr) {
                    continue;
                }
                if (slot.submitted) {
                    const VkResult status = vkGetFenceStatus(runtime->device, slot.fence);
                    if (status == VK_NOT_READY) {
                        continue;
                    }
                    if (!record_vk_result(runtime, status, "vkGetFenceStatus", "command slot selection", reason, reason_size)) {
                        return false;
                    }
                    slot.submitted = false;
                }
                set_active_slot_locked(runtime, index);
                return true;
            }
            std::snprintf(reason, reason_size, "no free Vulkan command slot");
            return false;
        }

        void clear_pending_work_locked(VulkanRuntime* runtime, CommandSlot* slot, VkvvSurface** surface, VkVideoSessionParametersKHR* parameters,
                                       VkDeviceSize* upload_allocation_size, bool* refresh_export, char* operation, size_t operation_size) {
            *surface                = slot->pending_surface;
            *parameters             = slot->pending_parameters;
            *upload_allocation_size = slot->pending_upload_allocation_size;
            *refresh_export         = slot->pending_export_refresh;
            std::snprintf(operation, operation_size, "%s", slot->pending_operation);
            slot->pending_surface                = nullptr;
            slot->pending_parameters             = VK_NULL_HANDLE;
            slot->pending_upload_allocation_size = 0;
            slot->pending_export_refresh         = true;
            slot->pending_operation[0]           = '\0';
            slot->submitted                      = false;
            refresh_legacy_pending_snapshot_locked(runtime);
        }

    } // namespace

    bool ensure_runtime_usable(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation) {
        if (runtime == nullptr) {
            std::snprintf(reason, reason_size, "missing Vulkan runtime for %s", operation);
            return false;
        }
        if (runtime->device_lost) {
            std::snprintf(reason, reason_size, "Vulkan device lost before %s", operation);
            return false;
        }
        return true;
    }

    bool record_vk_result(VulkanRuntime* runtime, VkResult result, const char* call, const char* operation, char* reason, size_t reason_size) {
        if (result == VK_SUCCESS) {
            return true;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            mark_device_lost(runtime);
        }
        std::snprintf(reason, reason_size, "%s for %s failed: %d", call, operation, result);
        return false;
    }

    bool ensure_command_resources(VulkanRuntime* runtime, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, "video command resources")) {
            return false;
        }
        if (runtime->command_pool != VK_NULL_HANDLE && runtime->command_slots[0].command_buffer != VK_NULL_HANDLE && runtime->command_slots[0].fence != VK_NULL_HANDLE) {
            return select_idle_command_slot_locked(runtime, reason, reason_size);
        }

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = runtime->decode_queue_family;

        VkResult result = vkCreateCommandPool(runtime->device, &pool_info, nullptr, &runtime->command_pool);
        if (!record_vk_result(runtime, result, "vkCreateCommandPool", "video command", reason, reason_size)) {
            return false;
        }

        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool        = runtime->command_pool;
        allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = command_slot_count;
        VkCommandBuffer command_buffers[command_slot_count]{};
        result = vkAllocateCommandBuffers(runtime->device, &allocate_info, command_buffers);
        if (!record_vk_result(runtime, result, "vkAllocateCommandBuffers", "video command", reason, reason_size)) {
            runtime->destroy_command_resources();
            return false;
        }
        for (size_t i = 0; i < command_slot_count; i++) {
            runtime->command_slots[i].command_buffer = command_buffers[i];
        }

        for (size_t i = 0; i < command_slot_count; i++) {
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            result           = vkCreateFence(runtime->device, &fence_info, nullptr, &runtime->command_slots[i].fence);
            if (!record_vk_result(runtime, result, "vkCreateFence", "video command", reason, reason_size)) {
                runtime->destroy_command_resources();
                return false;
            }
        }

        set_active_slot_locked(runtime, 0);
        return select_idle_command_slot_locked(runtime, reason, reason_size);
    }

    bool submit_command_buffer(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, operation)) {
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &runtime->command_buffer;

        VkResult result = vkQueueSubmit(runtime->decode_queue, 1, &submit, runtime->fence);
        if (!record_vk_result(runtime, result, "vkQueueSubmit", operation, reason, reason_size)) {
            return false;
        }
        active_slot(runtime).submitted = true;

        return true;
    }

    bool wait_for_command_fence(VulkanRuntime* runtime, uint64_t timeout_ns, char* reason, size_t reason_size, const char* operation) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, operation)) {
            return false;
        }
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
        if (!record_vk_result(runtime, result, "vkWaitForFences", operation, reason, reason_size)) {
            return false;
        }
        active_slot(runtime).submitted = false;

        return true;
    }

    void complete_surface_status(VkvvSurface* surface, VAStatus status) {
        if (surface == nullptr || surface->destroying) {
            return;
        }
        surface->work_state  = VKVV_SURFACE_WORK_READY;
        surface->sync_status = status;
    }

    bool submit_command_buffer_and_wait(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation) {
        if (!submit_command_buffer(runtime, reason, reason_size, operation)) {
            return false;
        }
        return wait_for_command_fence(runtime, std::numeric_limits<uint64_t>::max(), reason, reason_size, operation);
    }

    void track_pending_decode(VulkanRuntime* runtime, VkvvSurface* surface, VkVideoSessionParametersKHR parameters, VkDeviceSize upload_allocation_size, bool refresh_export,
                              const char* operation) {
        CommandSlot& slot                   = active_slot(runtime);
        slot.pending_surface                = surface;
        slot.pending_parameters             = parameters;
        slot.pending_upload_allocation_size = upload_allocation_size;
        slot.pending_export_refresh         = refresh_export;
        std::snprintf(slot.pending_operation, sizeof(slot.pending_operation), "%s", operation);
        refresh_legacy_pending_snapshot_locked(runtime);
        const auto* resource = surface != nullptr ? static_cast<const SurfaceResource*>(surface->vulkan) : nullptr;
        vkvv_trace(
            "pending-submit",
            "slot=%zu pending=%zu operation=%s surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u decoded=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu "
            "predecode=%u upload_mem=%llu",
            runtime->active_command_slot, pending_work_count_locked(runtime), operation != nullptr ? operation : "unknown", surface != nullptr ? surface->id : VA_INVALID_ID,
            surface != nullptr ? static_cast<unsigned long long>(surface->driver_instance_id) : 0ULL,
            surface != nullptr ? static_cast<unsigned long long>(surface->stream_id) : 0ULL, surface != nullptr ? surface->codec_operation : 0U, refresh_export ? 1U : 0U,
            surface != nullptr && surface->decoded ? 1U : 0U, resource != nullptr ? static_cast<unsigned long long>(resource->content_generation) : 0ULL,
            resource != nullptr ? vkvv_trace_handle(resource->export_resource.memory) : 0ULL,
            resource != nullptr ? static_cast<unsigned long long>(resource->export_resource.content_generation) : 0ULL,
            resource != nullptr && resource->export_resource.predecode_exported ? 1U : 0U, static_cast<unsigned long long>(upload_allocation_size));
    }

    VAStatus complete_pending_work(VulkanRuntime* runtime, VkvvSurface* surface, uint64_t timeout_ns, char* reason, size_t reason_size) {
        if (runtime == nullptr) {
            if (reason_size > 0) {
                reason[0] = '\0';
            }
            return VA_STATUS_SUCCESS;
        }

        VkvvSurface*                completed_surface      = nullptr;
        VkVideoSessionParametersKHR completed_parameters   = VK_NULL_HANDLE;
        VkDeviceSize                upload_allocation_size = 0;
        bool                        refresh_export         = true;
        char                        operation[64]          = {};
        if (runtime->device_lost) {
            {
                std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
                size_t                      slot_index = 0;
                CommandSlot*                slot       = find_pending_slot_locked(runtime, surface, &slot_index);
                if (slot != nullptr) {
                    set_active_slot_locked(runtime, slot_index);
                    clear_pending_work_locked(runtime, slot, &completed_surface, &completed_parameters, &upload_allocation_size, &refresh_export, operation, sizeof(operation));
                }
            }
            if (completed_parameters != VK_NULL_HANDLE && runtime->destroy_video_session_parameters != nullptr) {
                runtime->destroy_video_session_parameters(runtime->device, completed_parameters, nullptr);
            }
            complete_surface_status(completed_surface, VA_STATUS_ERROR_OPERATION_FAILED);
            std::snprintf(reason, reason_size, "Vulkan device lost before pending work completion");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            size_t                      slot_index = 0;
            CommandSlot*                slot       = find_pending_slot_locked(runtime, surface, &slot_index);
            if (slot == nullptr) {
                if (surface == nullptr && reason_size > 0) {
                    reason[0] = '\0';
                    return VA_STATUS_SUCCESS;
                }
                std::snprintf(reason, reason_size, "no matching pending Vulkan work");
                return VA_STATUS_ERROR_TIMEDOUT;
            }
            set_active_slot_locked(runtime, slot_index);

            if (!wait_for_command_fence(runtime, timeout_ns, reason, reason_size, slot->pending_operation[0] != '\0' ? slot->pending_operation : "Vulkan decode")) {
                if (runtime->device_lost) {
                    clear_pending_work_locked(runtime, slot, &completed_surface, &completed_parameters, &upload_allocation_size, &refresh_export, operation, sizeof(operation));
                } else {
                    return VA_STATUS_ERROR_TIMEDOUT;
                }
            } else {
                clear_pending_work_locked(runtime, slot, &completed_surface, &completed_parameters, &upload_allocation_size, &refresh_export, operation, sizeof(operation));
            }
        }

        const auto* completed_resource_before = completed_surface != nullptr ? static_cast<const SurfaceResource*>(completed_surface->vulkan) : nullptr;
        vkvv_trace("pending-complete-before",
                   "operation=%s surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u decoded=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u "
                   "exported=%u upload_mem=%llu",
                   operation[0] != '\0' ? operation : "Vulkan decode", completed_surface != nullptr ? completed_surface->id : VA_INVALID_ID,
                   completed_surface != nullptr ? static_cast<unsigned long long>(completed_surface->driver_instance_id) : 0ULL,
                   completed_surface != nullptr ? static_cast<unsigned long long>(completed_surface->stream_id) : 0ULL,
                   completed_surface != nullptr ? completed_surface->codec_operation : 0U, refresh_export ? 1U : 0U,
                   completed_surface != nullptr && completed_surface->decoded ? 1U : 0U,
                   completed_resource_before != nullptr ? static_cast<unsigned long long>(completed_resource_before->content_generation) : 0ULL,
                   completed_resource_before != nullptr ? vkvv_trace_handle(completed_resource_before->export_resource.memory) : 0ULL,
                   completed_resource_before != nullptr ? static_cast<unsigned long long>(completed_resource_before->export_resource.content_generation) : 0ULL,
                   completed_resource_before != nullptr && completed_resource_before->export_resource.predecode_exported ? 1U : 0U,
                   completed_resource_before != nullptr && completed_resource_before->export_resource.exported ? 1U : 0U, static_cast<unsigned long long>(upload_allocation_size));

        if (completed_parameters != VK_NULL_HANDLE && runtime->destroy_video_session_parameters != nullptr) {
            runtime->destroy_video_session_parameters(runtime->device, completed_parameters, nullptr);
        }
        if (runtime->device_lost) {
            complete_surface_status(completed_surface, VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (completed_surface == nullptr) {
            if (reason_size > 0) {
                reason[0] = '\0';
            }
            return VA_STATUS_SUCCESS;
        }

        completed_surface->decoded = true;
        if (completed_surface->vulkan != nullptr) {
            auto* resource = static_cast<SurfaceResource*>(completed_surface->vulkan);
            resource->content_generation++;
        }
        const auto* refresh_resource = completed_surface != nullptr ? static_cast<const SurfaceResource*>(completed_surface->vulkan) : nullptr;
        vkvv_trace("pending-refresh-decision",
                   "operation=%s surface=%u refresh_export=%u exported=%u predecode=%u shadow_mem=0x%llx content_gen=%llu shadow_gen=%llu action=refresh-current",
                   operation[0] != '\0' ? operation : "Vulkan decode", completed_surface != nullptr ? completed_surface->id : VA_INVALID_ID, refresh_export ? 1U : 0U,
                   refresh_resource != nullptr && refresh_resource->export_resource.exported ? 1U : 0U,
                   refresh_resource != nullptr && refresh_resource->export_resource.predecode_exported ? 1U : 0U,
                   refresh_resource != nullptr ? vkvv_trace_handle(refresh_resource->export_resource.memory) : 0ULL,
                   refresh_resource != nullptr ? static_cast<unsigned long long>(refresh_resource->content_generation) : 0ULL,
                   refresh_resource != nullptr ? static_cast<unsigned long long>(refresh_resource->export_resource.content_generation) : 0ULL);
        VAStatus status = ::vkvv_vulkan_refresh_surface_export(runtime, completed_surface, refresh_export, reason, reason_size);
        if (status != VA_STATUS_SUCCESS) {
            complete_surface_status(completed_surface, status);
            return status;
        }
        const auto* completed_resource_after = completed_surface != nullptr ? static_cast<const SurfaceResource*>(completed_surface->vulkan) : nullptr;
        vkvv_trace("pending-complete-after",
                   "operation=%s surface=%u status=%d refresh_export=%u decoded=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u exported=%u",
                   operation[0] != '\0' ? operation : "Vulkan decode", completed_surface != nullptr ? completed_surface->id : VA_INVALID_ID, status, refresh_export ? 1U : 0U,
                   completed_surface != nullptr && completed_surface->decoded ? 1U : 0U,
                   completed_resource_after != nullptr ? static_cast<unsigned long long>(completed_resource_after->content_generation) : 0ULL,
                   completed_resource_after != nullptr ? vkvv_trace_handle(completed_resource_after->export_resource.memory) : 0ULL,
                   completed_resource_after != nullptr ? static_cast<unsigned long long>(completed_resource_after->export_resource.content_generation) : 0ULL,
                   completed_resource_after != nullptr && completed_resource_after->export_resource.predecode_exported ? 1U : 0U,
                   completed_resource_after != nullptr && completed_resource_after->export_resource.exported ? 1U : 0U);
        complete_surface_status(completed_surface, VA_STATUS_SUCCESS);
        if (reason[0] == '\0') {
            std::snprintf(reason, reason_size, "%s completed asynchronously: surface=%u upload_mem=%llu", operation[0] != '\0' ? operation : "Vulkan decode",
                          completed_surface != nullptr ? completed_surface->id : VA_INVALID_ID, static_cast<unsigned long long>(upload_allocation_size));
        }
        return VA_STATUS_SUCCESS;
    }

    size_t runtime_pending_work_count(VulkanRuntime* runtime) {
        if (runtime == nullptr) {
            return 0;
        }
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        return pending_work_count_locked(runtime);
    }

    bool runtime_surface_has_pending_work(VulkanRuntime* runtime, const VkvvSurface* surface) {
        if (runtime == nullptr || surface == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        return find_pending_slot_locked(runtime, surface, nullptr) != nullptr;
    }

    bool runtime_surface_has_pending_export_refresh_work(VulkanRuntime* runtime, const VkvvSurface* surface) {
        if (runtime == nullptr || surface == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
        CommandSlot*                slot = find_pending_slot_locked(runtime, surface, nullptr);
        return slot != nullptr && slot->pending_export_refresh;
    }

    VAStatus ensure_command_slot_capacity(VulkanRuntime* runtime, const char* operation, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, operation != nullptr ? operation : "command slot capacity")) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        while (runtime_pending_work_count(runtime) >= command_slot_count) {
            const size_t pending_before = runtime_pending_work_count(runtime);
            vkvv_trace("pending-backpressure-drain", "operation=%s pending=%zu slots=%zu", operation != nullptr ? operation : "Vulkan decode", pending_before, command_slot_count);
            char     completion_reason[512] = {};
            VAStatus status                 = complete_pending_work(runtime, nullptr, std::numeric_limits<uint64_t>::max(), completion_reason, sizeof(completion_reason));
            vkvv_trace("pending-backpressure-drain-done", "operation=%s status=%d pending_before=%zu pending_after=%zu reason=\"%s\"",
                       operation != nullptr ? operation : "Vulkan decode", status, pending_before, runtime_pending_work_count(runtime), completion_reason);
            if (status != VA_STATUS_SUCCESS) {
                std::snprintf(reason, reason_size, "%s", completion_reason);
                return status;
            }
        }

        if (reason_size > 0) {
            reason[0] = '\0';
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus complete_pending_surface_work_if_needed(VulkanRuntime* runtime, VkvvSurface* surface, const char* operation, char* reason, size_t reason_size) {
        if (runtime == nullptr || surface == nullptr || surface->work_state != VKVV_SURFACE_WORK_RENDERING) {
            if (reason_size > 0) {
                reason[0] = '\0';
            }
            return VA_STATUS_SUCCESS;
        }
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            if (find_pending_slot_locked(runtime, surface, nullptr) == nullptr) {
                if (reason_size > 0) {
                    reason[0] = '\0';
                }
                return VA_STATUS_SUCCESS;
            }
        }

        vkvv_trace("pending-reference-drain", "operation=%s surface=%u driver=%llu stream=%llu codec=0x%x", operation != nullptr ? operation : "pending reference", surface->id,
                   static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), surface->codec_operation);
        VAStatus status = complete_pending_work(runtime, surface, std::numeric_limits<uint64_t>::max(), reason, reason_size);
        vkvv_trace("pending-reference-drain-done", "operation=%s surface=%u status=%d decoded=%u pending=%u", operation != nullptr ? operation : "pending reference", surface->id,
                   status, surface->decoded ? 1U : 0U, surface->work_state == VKVV_SURFACE_WORK_RENDERING ? 1U : 0U);
        return status;
    }

    VAStatus drain_pending_surface_work_before_sync_command(VulkanRuntime* runtime, VkvvSurface* surface, char* reason, size_t reason_size) {
        if (runtime == nullptr || surface == nullptr || surface->work_state != VKVV_SURFACE_WORK_RENDERING) {
            if (reason_size > 0) {
                reason[0] = '\0';
            }
            return VA_STATUS_SUCCESS;
        }
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            if (find_pending_slot_locked(runtime, surface, nullptr) == nullptr) {
                vkvv_trace("pending-drain-skip", "surface=%u driver=%llu stream=%llu codec=0x%x pending=%zu reason=no-slot", surface->id,
                           static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), surface->codec_operation,
                           pending_work_count_locked(runtime));
                if (reason_size > 0) {
                    reason[0] = '\0';
                }
                return VA_STATUS_SUCCESS;
            }
        }
        return complete_pending_work(runtime, surface, std::numeric_limits<uint64_t>::max(), reason, reason_size);
    }

    VAStatus drain_pending_work_before_sync_command(VulkanRuntime* runtime, char* reason, size_t reason_size) {
        VAStatus final_status = VA_STATUS_SUCCESS;
        if (reason_size > 0) {
            reason[0] = '\0';
        }
        while (runtime_pending_work_count(runtime) > 0) {
            char     completion_reason[512] = {};
            VAStatus status                 = complete_pending_work(runtime, nullptr, std::numeric_limits<uint64_t>::max(), completion_reason, sizeof(completion_reason));
            if (completion_reason[0] != '\0') {
                std::snprintf(reason, reason_size, "%s", completion_reason);
            }
            if (status != VA_STATUS_SUCCESS) {
                return status;
            }
            final_status = status;
        }
        return final_status;
    }

    void discard_pending_work_for_teardown(VulkanRuntime* runtime, VAStatus status, const char* reason) {
        if (runtime == nullptr) {
            return;
        }

        struct PendingTeardownRecord {
            VkvvSurface*                surface    = nullptr;
            VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
            char                        operation[64]{};
        };

        PendingTeardownRecord records[command_slot_count]{};
        size_t                record_count = 0;
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            for (CommandSlot& slot : runtime->command_slots) {
                if (slot.pending_surface == nullptr && slot.pending_parameters == VK_NULL_HANDLE) {
                    continue;
                }
                PendingTeardownRecord& record = records[record_count++];
                record.surface                = slot.pending_surface;
                record.parameters             = slot.pending_parameters;
                std::snprintf(record.operation, sizeof(record.operation), "%s", slot.pending_operation);
                slot.pending_surface                = nullptr;
                slot.pending_parameters             = VK_NULL_HANDLE;
                slot.pending_upload_allocation_size = 0;
                slot.pending_export_refresh         = true;
                slot.pending_operation[0]           = '\0';
                slot.submitted                      = false;
            }
            refresh_legacy_pending_snapshot_locked(runtime);
        }

        for (size_t i = 0; i < record_count; i++) {
            PendingTeardownRecord& record = records[i];
            if (record.parameters != VK_NULL_HANDLE && runtime->destroy_video_session_parameters != nullptr && runtime->device != VK_NULL_HANDLE) {
                runtime->destroy_video_session_parameters(runtime->device, record.parameters, nullptr);
            }
            complete_surface_status(record.surface, status);
            vkvv_trace("pending-teardown-discard", "operation=%s surface=%u status=%d reason=\"%s\"", record.operation[0] != '\0' ? record.operation : "Vulkan decode",
                       record.surface != nullptr ? record.surface->id : VA_INVALID_ID, status, reason != nullptr ? reason : "");
        }
    }

} // namespace vkvv

using namespace vkvv;

VAStatus vkvv_vulkan_complete_surface_work(void* runtime_ptr, VkvvSurface* surface, uint64_t timeout_ns, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    return complete_pending_work(runtime, surface, timeout_ns, reason, reason_size);
}

bool vkvv_vulkan_surface_has_pending_export_refresh_work(void* runtime_ptr, const VkvvSurface* surface) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    return runtime_surface_has_pending_export_refresh_work(runtime, surface);
}

VAStatus vkvv_vulkan_drain_pending_work(void* runtime_ptr, char* reason, size_t reason_size) {
    auto* runtime = static_cast<VulkanRuntime*>(runtime_ptr);
    return drain_pending_work_before_sync_command(runtime, reason, reason_size);
}
