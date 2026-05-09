#include "vulkan/runtime_internal.h"

#include <cstdio>
#include <vector>

namespace vkvv {

    uint32_t round_up_16(uint32_t value) {
        return (value + 15u) & ~15u;
    }

    VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
        if (alignment <= 1) {
            return value;
        }
        return ((value + alignment - 1) / alignment) * alignment;
    }

    bool find_memory_type(const VkPhysicalDeviceMemoryProperties& properties, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t* type_index) {
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
            if ((type_bits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & required) == required) {
                *type_index = i;
                return true;
            }
        }
        return false;
    }

} // namespace vkvv
