#ifndef VKVV_VULKAN_RUNTIME_INTERNAL_H
#define VKVV_VULKAN_RUNTIME_INTERNAL_H

#include "vulkan_runtime.h"
#include "vulkan/video_profile.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkvv {

inline constexpr uint32_t invalid_queue_family = UINT32_MAX;

struct ExportResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned int va_fourcc = 0;
    VkDeviceSize allocation_size = 0;
    VkSubresourceLayout plane_layouts[2]{};
    uint32_t plane_count = 0;
    uint64_t drm_format_modifier = 0;
    bool has_drm_format_modifier = false;
    bool exported = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct DecodeImageKey {
    VkVideoCodecOperationFlagsKHR codec_operation = 0;
    uint32_t codec_profile = 0;
    VkFormat picture_format = VK_FORMAT_UNDEFINED;
    VkFormat reference_picture_format = VK_FORMAT_UNDEFINED;
    unsigned int va_rt_format = 0;
    unsigned int va_fourcc = 0;
    VkExtent2D coded_extent{};
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags create_flags = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkVideoChromaSubsamplingFlagsKHR chroma_subsampling = 0;
    VkVideoComponentBitDepthFlagsKHR luma_bit_depth = 0;
    VkVideoComponentBitDepthFlagsKHR chroma_bit_depth = 0;
};

struct SurfaceResource {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkExtent2D coded_extent{};
    VkExtent2D visible_extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned int va_rt_format = 0;
    unsigned int va_fourcc = 0;
    DecodeImageKey decode_key{};
    VkDeviceSize allocation_size = 0;
    VkSubresourceLayout plane_layouts[2]{};
    uint32_t plane_count = 0;
    uint64_t drm_format_modifier = 0;
    bool exportable = false;
    bool has_drm_format_modifier = false;
    ExportResource export_resource{};
    std::vector<ExportResource> retired_exports;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct UploadBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize capacity = 0;
    VkDeviceSize allocation_size = 0;
    bool coherent = true;
};

struct VideoSessionKey {
    VkVideoCodecOperationFlagsKHR codec_operation = 0;
    uint32_t codec_profile = 0;
    VkFormat picture_format = VK_FORMAT_UNDEFINED;
    VkFormat reference_picture_format = VK_FORMAT_UNDEFINED;
    VkExtent2D max_coded_extent{};
    VkImageUsageFlags image_usage = 0;
    VkImageCreateFlags image_create_flags = 0;
    VkImageTiling image_tiling = VK_IMAGE_TILING_OPTIMAL;
    VkVideoChromaSubsamplingFlagsKHR chroma_subsampling = 0;
    VkVideoComponentBitDepthFlagsKHR luma_bit_depth = 0;
    VkVideoComponentBitDepthFlagsKHR chroma_bit_depth = 0;
};

struct VideoSession {
    VkVideoSessionKHR session = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> memory;
    VkDeviceSize memory_bytes = 0;
    VideoSessionKey key{};
    bool initialized = false;
};

class VulkanRuntime {
  public:
    ~VulkanRuntime() {
        destroy_command_resources();
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue decode_queue = VK_NULL_HANDLE;
    uint32_t decode_queue_family = invalid_queue_family;
    VkVideoCodecOperationFlagsKHR probed_decode_operations = 0;
    VkVideoCodecOperationFlagsKHR enabled_decode_operations = 0;
    VkVideoCodecOperationFlagsKHR probed_encode_operations = 0;
    VkVideoCodecOperationFlagsKHR enabled_encode_operations = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};

    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_queue_family_properties2 = nullptr;
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_video_capabilities = nullptr;
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR get_video_format_properties = nullptr;
    PFN_vkCreateVideoSessionKHR create_video_session = nullptr;
    PFN_vkDestroyVideoSessionKHR destroy_video_session = nullptr;
    PFN_vkGetVideoSessionMemoryRequirementsKHR get_video_session_memory_requirements = nullptr;
    PFN_vkBindVideoSessionMemoryKHR bind_video_session_memory = nullptr;
    PFN_vkCreateVideoSessionParametersKHR create_video_session_parameters = nullptr;
    PFN_vkDestroyVideoSessionParametersKHR destroy_video_session_parameters = nullptr;
    PFN_vkCmdBeginVideoCodingKHR cmd_begin_video_coding = nullptr;
    PFN_vkCmdEndVideoCodingKHR cmd_end_video_coding = nullptr;
    PFN_vkCmdControlVideoCodingKHR cmd_control_video_coding = nullptr;
    PFN_vkCmdDecodeVideoKHR cmd_decode_video = nullptr;
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_drm_format_modifier_properties = nullptr;

    bool external_memory_fd = false;
    bool external_memory_dma_buf = false;
    bool image_drm_format_modifier = false;
    bool surface_export = false;

    bool video_maintenance2 = false;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkvvSurface *pending_surface = nullptr;
    VkVideoSessionParametersKHR pending_parameters = VK_NULL_HANDLE;
    VkDeviceSize pending_upload_allocation_size = 0;
    char pending_operation[64]{};
    std::mutex command_mutex;

    void destroy_command_resources() {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
        if (command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
            command_buffer = VK_NULL_HANDLE;
        }
    }
};

bool extension_present(const std::vector<VkExtensionProperties> &extensions, const char *name);
uint32_t round_up_16(uint32_t value);
VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment);
bool find_memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t *type_index);
bool enumerate_drm_format_modifiers(VulkanRuntime *runtime, VkFormat format, VkFormatFeatureFlags2 required, std::vector<uint64_t> *modifiers);

void destroy_video_session(VulkanRuntime *runtime, VideoSession *session);
bool bind_video_session_memory(VulkanRuntime *runtime, VideoSession *session, char *reason, size_t reason_size);
void destroy_export_resource(VulkanRuntime *runtime, ExportResource *resource);
VkDeviceSize export_memory_bytes(const SurfaceResource *resource);
void retire_export_resource(SurfaceResource *resource);
void destroy_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface);
bool ensure_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface, const DecodeImageKey &key, char *reason, size_t reason_size);
void destroy_upload_buffer(VulkanRuntime *runtime, UploadBuffer *upload);
bool ensure_bitstream_upload_buffer(
        VulkanRuntime *runtime,
        const VideoProfileSpec &profile_spec,
        const void *data,
        size_t data_size,
        VkDeviceSize size_alignment,
        VkBufferUsageFlags usage,
        UploadBuffer *upload,
        const char *label,
        char *reason,
        size_t reason_size);
bool ensure_command_resources(VulkanRuntime *runtime, char *reason, size_t reason_size);
bool submit_command_buffer(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation);
bool submit_command_buffer_and_wait(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation);
void track_pending_decode(VulkanRuntime *runtime, VkvvSurface *surface, VkVideoSessionParametersKHR parameters, VkDeviceSize upload_allocation_size, const char *operation);
void add_image_layout_barrier(std::vector<VkImageMemoryBarrier2> *barriers, SurfaceResource *resource, VkImageLayout new_layout, VkAccessFlags2 dst_access);
VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource *resource, VkExtent2D coded_extent);

} // namespace vkvv

#endif
