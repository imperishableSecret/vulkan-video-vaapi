#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <chrono>
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

        size_t pending_work_count_locked(const VulkanRuntime* runtime) {
            size_t count = 0;
            for (const CommandSlot& slot : runtime->command_slots) {
                if (pending_work_has_payload(slot.pending)) {
                    count++;
                }
            }
            return count;
        }

        CommandSlot* find_pending_slot_locked(VulkanRuntime* runtime, const VkvvSurface* surface, size_t* index) {
            for (size_t i = 0; i < command_slot_count; i++) {
                CommandSlot& slot = runtime->command_slots[i];
                if (!pending_work_has_payload(slot.pending)) {
                    continue;
                }
                if (surface != nullptr && slot.pending.surface != surface) {
                    continue;
                }
                if (index != nullptr) {
                    *index = i;
                }
                return &slot;
            }
            return nullptr;
        }

        void note_perf_decode_submitted(VulkanRuntime* runtime, const VkvvSurface* surface, VkDeviceSize upload_allocation_size, size_t pending_count) {
            if (runtime == nullptr || !vkvv_perf_enabled()) {
                return;
            }
            runtime->perf.decode_submitted.fetch_add(1, std::memory_order_relaxed);
            runtime->perf.upload_bytes.fetch_add(static_cast<uint64_t>(upload_allocation_size), std::memory_order_relaxed);
            perf_update_high_water(runtime->perf.upload_high_water, static_cast<uint64_t>(upload_allocation_size));
            perf_update_high_water(runtime->perf.pending_high_water, static_cast<uint64_t>(pending_count));

            VkvvCodecPerfCounters* codec_counters = perf_decode_codec_counters(&runtime->perf, surface != nullptr ? surface->codec_operation : 0);
            if (codec_counters != nullptr) {
                codec_counters->submitted.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void note_perf_decode_completed(VulkanRuntime* runtime, const VkvvSurface* surface) {
            if (runtime == nullptr || !vkvv_perf_enabled()) {
                return;
            }
            runtime->perf.decode_completed.fetch_add(1, std::memory_order_relaxed);
            VkvvCodecPerfCounters* codec_counters = perf_decode_codec_counters(&runtime->perf, surface != nullptr ? surface->codec_operation : 0);
            if (codec_counters != nullptr) {
                codec_counters->completed.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool select_idle_command_slot_locked(VulkanRuntime* runtime, char* reason, size_t reason_size) {
            for (size_t attempt = 0; attempt < command_slot_count; attempt++) {
                const size_t index = (runtime->active_command_slot + attempt) % command_slot_count;
                CommandSlot& slot  = runtime->command_slots[index];
                if (pending_work_has_payload(slot.pending)) {
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

        void clear_pending_work_locked(CommandSlot* slot, PendingWork* completed) {
            *completed          = slot->pending;
            slot->submitted     = false;
            slot->submitted_use = CommandUse::Idle;
            reset_pending_work(&slot->pending);
        }

        void destroy_pending_work_parameters(VulkanRuntime* runtime, PendingWork* work) {
            if (runtime == nullptr || work == nullptr || work->parameters == VK_NULL_HANDLE || runtime->destroy_video_session_parameters == nullptr) {
                return;
            }
            runtime->destroy_video_session_parameters(runtime->device, work->parameters, nullptr);
            work->parameters = VK_NULL_HANDLE;
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

    bool submit_command_buffer(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation, CommandUse use) {
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
        CommandSlot& slot  = active_slot(runtime);
        slot.submitted     = true;
        slot.submitted_use = use;
        VKVV_TRACE("command-submit", "slot=%zu use=%s operation=%s", runtime->active_command_slot, command_use_name(use), operation != nullptr ? operation : "unknown");

        return true;
    }

    bool wait_for_command_fence(VulkanRuntime* runtime, uint64_t timeout_ns, char* reason, size_t reason_size, const char* operation) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, operation)) {
            return false;
        }
        VkResult   result       = VK_SUCCESS;
        const bool perf_enabled = vkvv_perf_enabled();
        const auto wait_start   = perf_enabled && timeout_ns != 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (timeout_ns == 0) {
            if (perf_enabled) {
                runtime->perf.command_fence_polls.fetch_add(1, std::memory_order_relaxed);
            }
            result = vkGetFenceStatus(runtime->device, runtime->fence);
            if (result == VK_NOT_READY) {
                std::snprintf(reason, reason_size, "%s is still pending", operation);
                return false;
            }
        } else {
            if (perf_enabled) {
                runtime->perf.command_fence_waits.fetch_add(1, std::memory_order_relaxed);
            }
            result = vkWaitForFences(runtime->device, 1, &runtime->fence, VK_TRUE, timeout_ns);
            if (perf_enabled) {
                const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_start).count();
                runtime->perf.command_fence_wait_ns.fetch_add(static_cast<uint64_t>(wait_ns), std::memory_order_relaxed);
            }
            if (result == VK_TIMEOUT) {
                std::snprintf(reason, reason_size, "%s timed out", operation);
                return false;
            }
        }
        if (!record_vk_result(runtime, result, "vkWaitForFences", operation, reason, reason_size)) {
            return false;
        }
        CommandSlot& slot  = active_slot(runtime);
        slot.submitted     = false;
        slot.submitted_use = CommandUse::Idle;

        return true;
    }

    void complete_surface_status(VkvvSurface* surface, VAStatus status) {
        if (surface == nullptr || surface->destroying) {
            return;
        }
        surface->work_state  = VKVV_SURFACE_WORK_READY;
        surface->sync_status = status;
    }

    bool submit_command_buffer_and_wait(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation, CommandUse use) {
        if (!submit_command_buffer(runtime, reason, reason_size, operation, use)) {
            return false;
        }
        return wait_for_command_fence(runtime, std::numeric_limits<uint64_t>::max(), reason, reason_size, operation);
    }

    void track_pending_decode(VulkanRuntime* runtime, VkvvSurface* surface, VkVideoSessionParametersKHR parameters, VkDeviceSize upload_allocation_size, bool refresh_export,
                              const char* operation) {
        CommandSlot& slot                   = active_slot(runtime);
        slot.pending.surface                = surface;
        slot.pending.parameters             = parameters;
        slot.pending.upload_allocation_size = upload_allocation_size;
        slot.pending.refresh_export         = refresh_export;
        slot.pending.use                    = CommandUse::Decode;
        std::snprintf(slot.pending.operation, sizeof(slot.pending.operation), "%s", operation);
        const auto* resource      = surface != nullptr ? static_cast<const SurfaceResource*>(surface->vulkan) : nullptr;
        size_t      pending_count = 0;
        const bool  trace_enabled = vkvv_trace_enabled();
        const bool  perf_enabled  = vkvv_perf_enabled();
        if (trace_enabled || perf_enabled) {
            pending_count = pending_work_count_locked(runtime);
        }
        note_perf_decode_submitted(runtime, surface, upload_allocation_size, pending_count);
        if (trace_enabled) {
            vkvv_trace_emit("pending-submit",
                            "slot=%zu use=%s pending=%zu operation=%s surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u decoded=%u content_gen=%llu "
                            "shadow_mem=0x%llx shadow_gen=%llu "
                            "predecode=%u upload_mem=%llu",
                            runtime->active_command_slot, command_use_name(slot.pending.use), pending_count, operation != nullptr ? operation : "unknown",
                            surface != nullptr ? surface->id : VA_INVALID_ID, surface != nullptr ? static_cast<unsigned long long>(surface->driver_instance_id) : 0ULL,
                            surface != nullptr ? static_cast<unsigned long long>(surface->stream_id) : 0ULL, surface != nullptr ? surface->codec_operation : 0U,
                            refresh_export ? 1U : 0U, surface != nullptr && surface->decoded ? 1U : 0U,
                            resource != nullptr ? static_cast<unsigned long long>(resource->content_generation) : 0ULL,
                            resource != nullptr ? vkvv_trace_handle(resource->export_resource.memory) : 0ULL,
                            resource != nullptr ? static_cast<unsigned long long>(resource->export_resource.content_generation) : 0ULL,
                            resource != nullptr && resource->export_resource.predecode_exported ? 1U : 0U, static_cast<unsigned long long>(upload_allocation_size));
        }
    }

    VAStatus complete_pending_work(VulkanRuntime* runtime, VkvvSurface* surface, uint64_t timeout_ns, char* reason, size_t reason_size) {
        if (runtime == nullptr) {
            if (reason_size > 0) {
                reason[0] = '\0';
            }
            return VA_STATUS_SUCCESS;
        }

        PendingWork completed{};
        if (runtime->device_lost) {
            {
                std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
                size_t                      slot_index = 0;
                CommandSlot*                slot       = find_pending_slot_locked(runtime, surface, &slot_index);
                if (slot != nullptr) {
                    set_active_slot_locked(runtime, slot_index);
                    clear_pending_work_locked(slot, &completed);
                }
            }
            destroy_pending_work_parameters(runtime, &completed);
            complete_surface_status(completed.surface, VA_STATUS_ERROR_OPERATION_FAILED);
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

            if (!wait_for_command_fence(runtime, timeout_ns, reason, reason_size, slot->pending.operation[0] != '\0' ? slot->pending.operation : "Vulkan decode")) {
                if (runtime->device_lost) {
                    clear_pending_work_locked(slot, &completed);
                } else {
                    return VA_STATUS_ERROR_TIMEDOUT;
                }
            } else {
                clear_pending_work_locked(slot, &completed);
            }
        }

        const auto* completed_resource_before = completed.surface != nullptr ? static_cast<const SurfaceResource*>(completed.surface->vulkan) : nullptr;
        VKVV_TRACE("pending-complete-before",
                   "use=%s operation=%s surface=%u driver=%llu stream=%llu codec=0x%x refresh_export=%u decoded=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u "
                   "exported=%u upload_mem=%llu",
                   command_use_name(completed.use), completed.operation[0] != '\0' ? completed.operation : "Vulkan decode",
                   completed.surface != nullptr ? completed.surface->id : VA_INVALID_ID,
                   completed.surface != nullptr ? static_cast<unsigned long long>(completed.surface->driver_instance_id) : 0ULL,
                   completed.surface != nullptr ? static_cast<unsigned long long>(completed.surface->stream_id) : 0ULL,
                   completed.surface != nullptr ? completed.surface->codec_operation : 0U, completed.refresh_export ? 1U : 0U,
                   completed.surface != nullptr && completed.surface->decoded ? 1U : 0U,
                   completed_resource_before != nullptr ? static_cast<unsigned long long>(completed_resource_before->content_generation) : 0ULL,
                   completed_resource_before != nullptr ? vkvv_trace_handle(completed_resource_before->export_resource.memory) : 0ULL,
                   completed_resource_before != nullptr ? static_cast<unsigned long long>(completed_resource_before->export_resource.content_generation) : 0ULL,
                   completed_resource_before != nullptr && completed_resource_before->export_resource.predecode_exported ? 1U : 0U,
                   completed_resource_before != nullptr && completed_resource_before->export_resource.exported ? 1U : 0U,
                   static_cast<unsigned long long>(completed.upload_allocation_size));

        destroy_pending_work_parameters(runtime, &completed);
        if (runtime->device_lost) {
            complete_surface_status(completed.surface, VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (completed.surface == nullptr) {
            if (reason_size > 0) {
                reason[0] = '\0';
            }
            return VA_STATUS_SUCCESS;
        }

        completed.surface->decoded = true;
        note_perf_decode_completed(runtime, completed.surface);
        if (completed.surface->vulkan != nullptr) {
            auto* resource = static_cast<SurfaceResource*>(completed.surface->vulkan);
            resource->content_generation++;
        }
        const auto* refresh_resource = completed.surface != nullptr ? static_cast<const SurfaceResource*>(completed.surface->vulkan) : nullptr;
        VKVV_TRACE("pending-refresh-decision",
                   "use=%s operation=%s surface=%u refresh_export=%u exported=%u predecode=%u shadow_mem=0x%llx content_gen=%llu shadow_gen=%llu action=refresh-current",
                   command_use_name(completed.use), completed.operation[0] != '\0' ? completed.operation : "Vulkan decode",
                   completed.surface != nullptr ? completed.surface->id : VA_INVALID_ID, completed.refresh_export ? 1U : 0U,
                   refresh_resource != nullptr && refresh_resource->export_resource.exported ? 1U : 0U,
                   refresh_resource != nullptr && refresh_resource->export_resource.predecode_exported ? 1U : 0U,
                   refresh_resource != nullptr ? vkvv_trace_handle(refresh_resource->export_resource.memory) : 0ULL,
                   refresh_resource != nullptr ? static_cast<unsigned long long>(refresh_resource->content_generation) : 0ULL,
                   refresh_resource != nullptr ? static_cast<unsigned long long>(refresh_resource->export_resource.content_generation) : 0ULL);
        VAStatus status = ::vkvv_vulkan_refresh_surface_export(runtime, completed.surface, completed.refresh_export, reason, reason_size);
        if (status != VA_STATUS_SUCCESS) {
            complete_surface_status(completed.surface, status);
            return status;
        }
        const auto* completed_resource_after = completed.surface != nullptr ? static_cast<const SurfaceResource*>(completed.surface->vulkan) : nullptr;
        VKVV_TRACE("pending-complete-after",
                   "use=%s operation=%s surface=%u status=%d refresh_export=%u decoded=%u content_gen=%llu shadow_mem=0x%llx shadow_gen=%llu predecode=%u exported=%u",
                   command_use_name(completed.use), completed.operation[0] != '\0' ? completed.operation : "Vulkan decode",
                   completed.surface != nullptr ? completed.surface->id : VA_INVALID_ID, status, completed.refresh_export ? 1U : 0U,
                   completed.surface != nullptr && completed.surface->decoded ? 1U : 0U,
                   completed_resource_after != nullptr ? static_cast<unsigned long long>(completed_resource_after->content_generation) : 0ULL,
                   completed_resource_after != nullptr ? vkvv_trace_handle(completed_resource_after->export_resource.memory) : 0ULL,
                   completed_resource_after != nullptr ? static_cast<unsigned long long>(completed_resource_after->export_resource.content_generation) : 0ULL,
                   completed_resource_after != nullptr && completed_resource_after->export_resource.predecode_exported ? 1U : 0U,
                   completed_resource_after != nullptr && completed_resource_after->export_resource.exported ? 1U : 0U);
        complete_surface_status(completed.surface, VA_STATUS_SUCCESS);
        if (reason[0] == '\0') {
            VKVV_SUCCESS_REASON(reason, reason_size, "%s completed asynchronously: surface=%u upload_mem=%llu",
                                completed.operation[0] != '\0' ? completed.operation : "Vulkan decode", completed.surface != nullptr ? completed.surface->id : VA_INVALID_ID,
                                static_cast<unsigned long long>(completed.upload_allocation_size));
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
        return slot != nullptr && slot->pending.refresh_export;
    }

    VAStatus ensure_command_slot_capacity(VulkanRuntime* runtime, const char* operation, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, operation != nullptr ? operation : "command slot capacity")) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        while (runtime_pending_work_count(runtime) >= command_slot_count) {
            const size_t pending_before = runtime_pending_work_count(runtime);
            VKVV_TRACE("pending-backpressure-drain", "operation=%s pending=%zu slots=%zu", operation != nullptr ? operation : "Vulkan decode", pending_before, command_slot_count);
            char     completion_reason[512] = {};
            VAStatus status                 = complete_pending_work(runtime, nullptr, std::numeric_limits<uint64_t>::max(), completion_reason, sizeof(completion_reason));
            VKVV_TRACE("pending-backpressure-drain-done", "operation=%s status=%d pending_before=%zu pending_after=%zu reason=\"%s\"",
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

        VKVV_TRACE("pending-reference-drain", "operation=%s surface=%u driver=%llu stream=%llu codec=0x%x", operation != nullptr ? operation : "pending reference", surface->id,
                   static_cast<unsigned long long>(surface->driver_instance_id), static_cast<unsigned long long>(surface->stream_id), surface->codec_operation);
        VAStatus status = complete_pending_work(runtime, surface, std::numeric_limits<uint64_t>::max(), reason, reason_size);
        VKVV_TRACE("pending-reference-drain-done", "operation=%s surface=%u status=%d decoded=%u pending=%u", operation != nullptr ? operation : "pending reference", surface->id,
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
                VKVV_TRACE("pending-drain-skip", "surface=%u driver=%llu stream=%llu codec=0x%x pending=%zu reason=no-slot", surface->id,
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

        PendingWork records[command_slot_count]{};
        size_t      record_count = 0;
        {
            std::lock_guard<std::mutex> command_lock(runtime->command_mutex);
            for (CommandSlot& slot : runtime->command_slots) {
                if (!pending_work_has_payload(slot.pending)) {
                    continue;
                }
                records[record_count++] = slot.pending;
                slot.submitted          = false;
                slot.submitted_use      = CommandUse::Idle;
                reset_pending_work(&slot.pending);
            }
        }

        for (size_t i = 0; i < record_count; i++) {
            PendingWork& record = records[i];
            if (runtime->device != VK_NULL_HANDLE) {
                destroy_pending_work_parameters(runtime, &record);
            }
            complete_surface_status(record.surface, status);
            VKVV_TRACE("pending-teardown-discard", "use=%s operation=%s surface=%u status=%d reason=\"%s\"", command_use_name(record.use),
                       record.operation[0] != '\0' ? record.operation : "Vulkan decode", record.surface != nullptr ? record.surface->id : VA_INVALID_ID, status,
                       reason != nullptr ? reason : "");
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
