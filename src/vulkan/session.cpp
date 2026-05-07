#include "../vulkan_runtime_internal.h"

#include <cstdio>
#include <vector>

namespace vkvv {

void destroy_video_session(VulkanRuntime *runtime, VideoSession *session) {
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
    session->key = {};
    session->initialized = false;
}

void destroy_h264_video_session(VulkanRuntime *runtime, H264VideoSession *session) {
    if (session == nullptr) {
        return;
    }
    destroy_upload_buffer(runtime, &session->upload);
    destroy_video_session(runtime, &session->video);
    session->bitstream_offset_alignment = 1;
    session->bitstream_size_alignment = 1;
    session->max_level = STD_VIDEO_H264_LEVEL_IDC_5_2;
    session->decode_flags = 0;
    session->max_dpb_slots = 0;
    session->max_active_reference_pictures = 0;
}

bool bind_video_session_memory(VulkanRuntime *runtime, VideoSession *session, char *reason, size_t reason_size) {
    uint32_t count = 0;
    VkResult result = runtime->get_video_session_memory_requirements(runtime->device, session->session, &count, nullptr);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetVideoSessionMemoryRequirementsKHR failed: %d", result);
        return false;
    }
    if (count == 0) {
        return true;
    }

    std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(count);
    for (VkVideoSessionMemoryRequirementsKHR &requirement : requirements) {
        requirement.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    result = runtime->get_video_session_memory_requirements(runtime->device, session->session, &count, requirements.data());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkGetVideoSessionMemoryRequirementsKHR failed: %d", result);
        return false;
    }
    requirements.resize(count);

    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    binds.reserve(requirements.size());
    session->memory.reserve(requirements.size());

    for (const VkVideoSessionMemoryRequirementsKHR &requirement : requirements) {
        uint32_t memory_type_index = 0;
        if (!find_memory_type(runtime->memory_properties, requirement.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type_index) &&
            !find_memory_type(runtime->memory_properties, requirement.memoryRequirements.memoryTypeBits, 0, &memory_type_index)) {
            std::snprintf(reason, reason_size, "no memory type for H.264 session bind index %u", requirement.memoryBindIndex);
            return false;
        }

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirement.memoryRequirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;

        VkDeviceMemory memory = VK_NULL_HANDLE;
        result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &memory);
        if (result != VK_SUCCESS) {
            std::snprintf(reason, reason_size, "vkAllocateMemory for H.264 session failed: %d", result);
            return false;
        }
        session->memory.push_back(memory);

        VkBindVideoSessionMemoryInfoKHR bind{};
        bind.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        bind.memoryBindIndex = requirement.memoryBindIndex;
        bind.memory = memory;
        bind.memoryOffset = 0;
        bind.memorySize = requirement.memoryRequirements.size;
        binds.push_back(bind);
        session->memory_bytes += requirement.memoryRequirements.size;
    }

    result = runtime->bind_video_session_memory(runtime->device, session->session,
                                               static_cast<uint32_t>(binds.size()), binds.data());
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkBindVideoSessionMemoryKHR failed: %d", result);
        return false;
    }

    return true;
}

} // namespace vkvv
