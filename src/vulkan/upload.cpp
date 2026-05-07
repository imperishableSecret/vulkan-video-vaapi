#include "../vulkan_runtime_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vkvv {

void destroy_upload_buffer(VulkanRuntime *runtime, UploadBuffer *upload) {
    if (upload == nullptr) {
        return;
    }
    if (runtime == nullptr) {
        *upload = {};
        return;
    }
    if (upload->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(runtime->device, upload->buffer, nullptr);
        upload->buffer = VK_NULL_HANDLE;
    }
    if (upload->memory != VK_NULL_HANDLE) {
        vkFreeMemory(runtime->device, upload->memory, nullptr);
        upload->memory = VK_NULL_HANDLE;
    }
    upload->size = 0;
    upload->capacity = 0;
    upload->allocation_size = 0;
    upload->coherent = true;
}

bool copy_upload_buffer(
        VulkanRuntime *runtime,
        UploadBuffer *upload,
        const VkvvH264DecodeInput *input,
        char *reason,
        size_t reason_size) {
    void *mapped = nullptr;
    VkResult result = vkMapMemory(runtime->device, upload->memory, 0, upload->size, 0, &mapped);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkMapMemory for H.264 bitstream failed: %d", result);
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(upload->size));
    std::memcpy(mapped, input->bitstream, input->bitstream_size);
    if (!upload->coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = upload->memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(runtime->device, 1, &range);
    }
    vkUnmapMemory(runtime->device, upload->memory);
    return true;
}

bool ensure_upload_buffer(
        VulkanRuntime *runtime,
        const H264VideoSession *session,
        const VkvvH264DecodeInput *input,
        UploadBuffer *upload,
        char *reason,
        size_t reason_size) {
    VideoProfileChain profile_chain;
    VkVideoProfileListInfoKHR profile_list{};
    profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    profile_list.profileCount = 1;
    profile_list.pProfiles = &profile_chain.profile;

    const VkDeviceSize requested_size = std::max<VkDeviceSize>(1, input->bitstream_size);
    const VkDeviceSize upload_size = align_up(requested_size, session->bitstream_size_alignment);
    upload->size = upload_size;
    if (upload->buffer != VK_NULL_HANDLE && upload->capacity >= upload_size) {
        return copy_upload_buffer(runtime, upload, input, reason, reason_size);
    }

    destroy_upload_buffer(runtime, upload);
    upload->size = upload_size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = &profile_list;
    buffer_info.size = upload->size;
    buffer_info.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &upload->buffer);
    if (result != VK_SUCCESS) {
        std::snprintf(reason, reason_size, "vkCreateBuffer for H.264 bitstream failed: %d", result);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(runtime->device, upload->buffer, &requirements);
    upload->capacity = upload_size;
    upload->allocation_size = requirements.size;

    uint32_t memory_type_index = 0;
    upload->coherent = true;
    if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &memory_type_index)) {
        upload->coherent = false;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index)) {
            destroy_upload_buffer(runtime, upload);
            std::snprintf(reason, reason_size, "no host-visible memory type for H.264 bitstream");
            return false;
        }
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type_index;
    result = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &upload->memory);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, upload);
        std::snprintf(reason, reason_size, "vkAllocateMemory for H.264 bitstream failed: %d", result);
        return false;
    }

    result = vkBindBufferMemory(runtime->device, upload->buffer, upload->memory, 0);
    if (result != VK_SUCCESS) {
        destroy_upload_buffer(runtime, upload);
        std::snprintf(reason, reason_size, "vkBindBufferMemory for H.264 bitstream failed: %d", result);
        return false;
    }

    if (!copy_upload_buffer(runtime, upload, input, reason, reason_size)) {
        destroy_upload_buffer(runtime, upload);
        return false;
    }
    return true;
}

} // namespace vkvv
