#ifndef VKVV_VULKAN_RUNTIME_INTERNAL_H
#define VKVV_VULKAN_RUNTIME_INTERNAL_H

#include "vulkan_runtime.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkvv {

inline constexpr uint32_t invalid_queue_family = UINT32_MAX;
inline constexpr uint32_t max_va_h264_reference_frames = 16;
inline constexpr uint32_t max_h264_dpb_slots = 17;

struct VideoCapabilitiesChain {
    VkVideoDecodeH264CapabilitiesKHR h264{};
    VkVideoDecodeCapabilitiesKHR decode{};
    VkVideoCapabilitiesKHR video{};

    VideoCapabilitiesChain() {
        h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
        decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
        decode.pNext = &h264;
        video.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        video.pNext = &decode;
    }
};

struct VideoProfileChain {
    VkVideoDecodeH264ProfileInfoKHR h264{};
    VkVideoDecodeUsageInfoKHR usage{};
    VkVideoProfileInfoKHR profile{};

    VideoProfileChain() {
        h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

        usage.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR;
        usage.pNext = &h264;
        usage.videoUsageHints = VK_VIDEO_DECODE_USAGE_STREAMING_BIT_KHR;

        profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
        profile.pNext = &usage;
        profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
        profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    }
};

struct ExportResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkDeviceSize allocation_size = 0;
    VkSubresourceLayout plane_layouts[2]{};
    uint32_t plane_count = 0;
    uint64_t drm_format_modifier = 0;
    bool has_drm_format_modifier = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct SurfaceResource {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkDeviceSize allocation_size = 0;
    VkSubresourceLayout plane_layouts[2]{};
    uint32_t plane_count = 0;
    uint64_t drm_format_modifier = 0;
    bool exportable = false;
    bool has_drm_format_modifier = false;
    ExportResource export_resource{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct UploadBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

class VulkanRuntime {
  public:
    ~VulkanRuntime() {
        destroy_h264_session();
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

    VkVideoSessionKHR h264_session = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> h264_session_memory;
    VkFormat h264_picture_format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags h264_image_create_flags = 0;
    VkImageTiling h264_image_tiling = VK_IMAGE_TILING_OPTIMAL;
    VkExtent2D h264_extent{};
    VkDeviceSize h264_bitstream_offset_alignment = 1;
    VkDeviceSize h264_bitstream_size_alignment = 1;
    StdVideoH264LevelIdc h264_max_level = STD_VIDEO_H264_LEVEL_IDC_5_2;
    VkVideoDecodeCapabilityFlagsKHR h264_decode_flags = 0;
    bool h264_session_initialized = false;
    bool video_maintenance2 = false;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

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

    void destroy_h264_session() {
        if (h264_session != VK_NULL_HANDLE && destroy_video_session != nullptr) {
            destroy_video_session(device, h264_session, nullptr);
            h264_session = VK_NULL_HANDLE;
        }
        for (VkDeviceMemory memory : h264_session_memory) {
            vkFreeMemory(device, memory, nullptr);
        }
        h264_session_memory.clear();
        h264_picture_format = VK_FORMAT_UNDEFINED;
        h264_image_create_flags = 0;
        h264_image_tiling = VK_IMAGE_TILING_OPTIMAL;
        h264_extent = {};
        h264_session_initialized = false;
    }
};

bool extension_present(const std::vector<VkExtensionProperties> &extensions, const char *name);
uint32_t round_up_16(uint32_t value);
VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment);
bool find_memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t *type_index);

bool bind_h264_session_memory(VulkanRuntime *runtime, char *reason, size_t reason_size);
void destroy_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface);
bool ensure_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface, VkExtent2D extent, char *reason, size_t reason_size);
void destroy_upload_buffer(VulkanRuntime *runtime, UploadBuffer *upload);
bool create_upload_buffer(VulkanRuntime *runtime, const VkvvH264DecodeInput *input, UploadBuffer *upload, char *reason, size_t reason_size);
bool ensure_command_resources(VulkanRuntime *runtime, char *reason, size_t reason_size);
bool submit_command_buffer_and_wait(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation);
bool reset_h264_session(VulkanRuntime *runtime, VkVideoSessionParametersKHR parameters, char *reason, size_t reason_size);
void add_image_layout_barrier(std::vector<VkImageMemoryBarrier2> *barriers, SurfaceResource *resource, VkImageLayout new_layout, VkAccessFlags2 dst_access);
VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource *resource, VkExtent2D coded_extent);

} // namespace vkvv

#endif
