#include "vulkan/runtime_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace vkvv {

    void destroy_upload_buffer(VulkanRuntime* runtime, UploadBuffer* upload) {
        if (upload == nullptr) {
            return;
        }
        if (runtime == nullptr) {
            *upload = {};
            return;
        }
        if (upload->mapped != nullptr) {
            vkUnmapMemory(runtime->device, upload->memory);
            upload->mapped = nullptr;
        }
        if (upload->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(runtime->device, upload->buffer, nullptr);
            upload->buffer = VK_NULL_HANDLE;
        }
        if (upload->memory != VK_NULL_HANDLE) {
            vkFreeMemory(runtime->device, upload->memory, nullptr);
            upload->memory = VK_NULL_HANDLE;
        }
        upload->size            = 0;
        upload->capacity        = 0;
        upload->allocation_size = 0;
        upload->coherent        = true;
    }

    bool copy_upload_buffer(VulkanRuntime* runtime, UploadBuffer* upload, const void* data, size_t data_size, const char* label, char* reason, size_t reason_size) {
        if (upload->mapped == nullptr) {
            VkResult result = vkMapMemory(runtime->device, upload->memory, 0, upload->allocation_size, 0, &upload->mapped);
            if (!record_vk_result(runtime, result, "vkMapMemory", label, reason, reason_size)) {
                return false;
            }
        }
        auto* mapped = static_cast<uint8_t*>(upload->mapped);
        std::memcpy(mapped, data, data_size);
        if (upload->size > data_size) {
            std::memset(mapped + data_size, 0, static_cast<size_t>(upload->size - data_size));
        }
        if (!upload->coherent) {
            const VkDeviceSize  atom_size  = std::max<VkDeviceSize>(1, runtime->device_properties.limits.nonCoherentAtomSize);
            VkDeviceSize        flush_size = std::min(upload->allocation_size, align_up(upload->size, atom_size));
            VkMappedMemoryRange range{};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = upload->memory;
            range.offset = 0;
            range.size   = flush_size;
            vkFlushMappedMemoryRanges(runtime->device, 1, &range);
        }
        return true;
    }

    bool ensure_bitstream_upload_buffer(VulkanRuntime* runtime, const VideoProfileSpec& profile_spec, const void* data, size_t data_size, VkDeviceSize size_alignment,
                                        VkBufferUsageFlags usage, UploadBuffer* upload, const char* label, char* reason, size_t reason_size) {
        if (runtime == nullptr || data == nullptr || data_size == 0 || upload == nullptr) {
            std::snprintf(reason, reason_size, "missing %s upload data", label);
            return false;
        }
        if (!ensure_runtime_usable(runtime, reason, reason_size, label)) {
            return false;
        }

        VideoProfileChain         profile_chain(profile_spec);
        VkVideoProfileListInfoKHR profile_list{};
        profile_list.sType        = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
        profile_list.profileCount = 1;
        profile_list.pProfiles    = &profile_chain.profile;

        const VkDeviceSize requested_size = std::max<VkDeviceSize>(1, data_size);
        const VkDeviceSize upload_size    = align_up(requested_size, std::max<VkDeviceSize>(1, size_alignment));
        upload->size                      = upload_size;
        if (upload->buffer != VK_NULL_HANDLE && upload->capacity >= upload_size) {
            return copy_upload_buffer(runtime, upload, data, data_size, label, reason, reason_size);
        }

        destroy_upload_buffer(runtime, upload);
        upload->size = upload_size;

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.pNext       = &profile_list;
        buffer_info.size        = upload->size;
        buffer_info.usage       = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(runtime->device, &buffer_info, nullptr, &upload->buffer);
        if (!record_vk_result(runtime, result, "vkCreateBuffer", label, reason, reason_size)) {
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(runtime->device, upload->buffer, &requirements);
        upload->capacity        = upload_size;
        upload->allocation_size = requirements.size;

        uint32_t memory_type_index = 0;
        upload->coherent           = true;
        if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &memory_type_index)) {
            upload->coherent = false;
            if (!find_memory_type(runtime->memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type_index)) {
                destroy_upload_buffer(runtime, upload);
                std::snprintf(reason, reason_size, "no host-visible memory type for %s", label);
                return false;
            }
        }

        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize  = requirements.size;
        allocate_info.memoryTypeIndex = memory_type_index;
        result                        = vkAllocateMemory(runtime->device, &allocate_info, nullptr, &upload->memory);
        if (!record_vk_result(runtime, result, "vkAllocateMemory", label, reason, reason_size)) {
            destroy_upload_buffer(runtime, upload);
            return false;
        }

        result = vkBindBufferMemory(runtime->device, upload->buffer, upload->memory, 0);
        if (!record_vk_result(runtime, result, "vkBindBufferMemory", label, reason, reason_size)) {
            destroy_upload_buffer(runtime, upload);
            return false;
        }

        if (!copy_upload_buffer(runtime, upload, data, data_size, label, reason, reason_size)) {
            destroy_upload_buffer(runtime, upload);
            return false;
        }
        return true;
    }

} // namespace vkvv
