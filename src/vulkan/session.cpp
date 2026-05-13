#include "vulkan/runtime_internal.h"
#include "telemetry.h"

#include <cstdio>
#include <vector>

namespace vkvv {

    void destroy_video_session(VulkanRuntime* runtime, VideoSession* session) {
        if (session == nullptr) {
            return;
        }
        if (runtime != nullptr && session->session != VK_NULL_HANDLE && runtime->destroy_video_session != nullptr) {
            runtime->destroy_video_session(runtime->device, session->session, nullptr);
            session->session = VK_NULL_HANDLE;
        }
        if (runtime != nullptr) {
            for (VkDeviceMemory memory : session->memory) {
                vkFreeMemory(runtime->device, memory, nullptr);
            }
        }
        session->memory.clear();
        session->memory_bytes = 0;
        session->key          = {};
        session->initialized  = false;
    }

    bool bind_video_session_memory(VulkanRuntime* runtime, VideoSession* session, char* reason, size_t reason_size) {
        if (!ensure_runtime_usable(runtime, reason, reason_size, "video session memory bind")) {
            return false;
        }
        uint32_t count  = 0;
        VkResult result = runtime->get_video_session_memory_requirements(runtime->device, session->session, &count, nullptr);
        if (!record_vk_result(runtime, result, "vkGetVideoSessionMemoryRequirementsKHR", "video session memory bind", reason, reason_size)) {
            return false;
        }
        if (count == 0) {
            return true;
        }

        std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(count);
        for (VkVideoSessionMemoryRequirementsKHR& requirement : requirements) {
            requirement.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
        }
        result = runtime->get_video_session_memory_requirements(runtime->device, session->session, &count, requirements.data());
        if (!record_vk_result(runtime, result, "vkGetVideoSessionMemoryRequirementsKHR", "video session memory bind", reason, reason_size)) {
            return false;
        }
        requirements.resize(count);

        std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
        binds.reserve(requirements.size());
        session->memory.reserve(requirements.size());

        for (const VkVideoSessionMemoryRequirementsKHR& requirement : requirements) {
            uint32_t memory_type_index = 0;
            if (!find_memory_type(runtime->memory_properties, requirement.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
                !find_memory_type(runtime->memory_properties, requirement.memoryRequirements.memoryTypeBits, 0, &memory_type_index)) {
                std::snprintf(reason, reason_size, "no memory type for video session bind index %u", requirement.memoryBindIndex);
                return false;
            }

            VkMemoryAllocateInfo allocate_info{};
            allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate_info.allocationSize  = requirement.memoryRequirements.size;
            allocate_info.memoryTypeIndex = memory_type_index;

            VkDeviceMemory memory = VK_NULL_HANDLE;
            result                = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &memory);
            if (!record_vk_result(runtime, result, "vkAllocateMemory", "video session memory bind", reason, reason_size)) {
                return false;
            }
            session->memory.push_back(memory);

            VkBindVideoSessionMemoryInfoKHR bind{};
            bind.sType           = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
            bind.memoryBindIndex = requirement.memoryBindIndex;
            bind.memory          = memory;
            bind.memoryOffset    = 0;
            bind.memorySize      = requirement.memoryRequirements.size;
            binds.push_back(bind);
            session->memory_bytes += requirement.memoryRequirements.size;
        }

        result = runtime->bind_video_session_memory(runtime->device, session->session, static_cast<uint32_t>(binds.size()), binds.data());
        if (!record_vk_result(runtime, result, "vkBindVideoSessionMemoryKHR", "video session memory bind", reason, reason_size)) {
            return false;
        }
        VKVV_TRACE("video-session-memory", "bytes=%llu binds=%zu", static_cast<unsigned long long>(session->memory_bytes), binds.size());

        return true;
    }

} // namespace vkvv
