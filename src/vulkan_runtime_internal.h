#ifndef VKVV_VULKAN_RUNTIME_INTERNAL_H
#define VKVV_VULKAN_RUNTIME_INTERNAL_H

#include "vulkan_runtime.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkvv {

inline constexpr uint32_t invalid_queue_family = UINT32_MAX;
inline constexpr uint32_t max_va_h264_reference_frames = 16;
inline constexpr uint32_t max_h264_dpb_slots = 17;

struct VideoCapabilitiesChain {
    VkVideoDecodeH264CapabilitiesKHR h264{};
    VkVideoDecodeH265CapabilitiesKHR h265{};
    VkVideoDecodeVP9CapabilitiesKHR vp9{};
    VkVideoDecodeAV1CapabilitiesKHR av1{};
    VkVideoDecodeCapabilitiesKHR decode{};
    VkVideoCapabilitiesKHR video{};

    explicit VideoCapabilitiesChain(
            VkVideoCodecOperationFlagBitsKHR operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
        h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
        h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
        vp9.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR;
        av1.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;

        void *codec_caps = nullptr;
        switch (operation) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
                codec_caps = &h264;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
                codec_caps = &h265;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
                codec_caps = &vp9;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
                codec_caps = &av1;
                break;
            default:
                break;
        }

        decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
        decode.pNext = codec_caps;
        video.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        video.pNext = &decode;
    }
};

struct VideoProfileChain {
    VkVideoDecodeH264ProfileInfoKHR h264{};
    VkVideoDecodeH265ProfileInfoKHR h265{};
    VkVideoDecodeVP9ProfileInfoKHR vp9{};
    VkVideoDecodeAV1ProfileInfoKHR av1{};
    VkVideoDecodeUsageInfoKHR usage{};
    VkVideoProfileInfoKHR profile{};

    explicit VideoProfileChain(
            VkVideoCodecOperationFlagBitsKHR operation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
            VkVideoComponentBitDepthFlagsKHR bit_depth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR) {
        h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        h264.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
        h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        h265.stdProfileIdc = bit_depth == VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR ?
                             STD_VIDEO_H265_PROFILE_IDC_MAIN_10 :
                             STD_VIDEO_H265_PROFILE_IDC_MAIN;
        vp9.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
        vp9.stdProfile = bit_depth == VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR ?
                         STD_VIDEO_VP9_PROFILE_2 :
                         STD_VIDEO_VP9_PROFILE_0;
        av1.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
        av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
        av1.filmGrainSupport = VK_FALSE;

        void *codec_profile = nullptr;
        switch (operation) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
                codec_profile = &h264;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
                codec_profile = &h265;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR:
                codec_profile = &vp9;
                break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR:
                codec_profile = &av1;
                break;
            default:
                break;
        }

        usage.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR;
        usage.pNext = codec_profile;
        usage.videoUsageHints = VK_VIDEO_DECODE_USAGE_STREAMING_BIT_KHR;

        profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
        profile.pNext = &usage;
        profile.videoCodecOperation = operation;
        profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        profile.lumaBitDepth = bit_depth;
        profile.chromaBitDepth = bit_depth;
    }
};

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
    VkDeviceSize allocation_size = 0;
};

struct VideoSessionKey {
    VkVideoCodecOperationFlagsKHR codec_operation = 0;
    uint32_t codec_profile = 0;
    VkFormat picture_format = VK_FORMAT_UNDEFINED;
    VkFormat reference_picture_format = VK_FORMAT_UNDEFINED;
    VkExtent2D max_coded_extent{};
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

struct H264VideoSession {
    VideoSession video;
    VkDeviceSize bitstream_offset_alignment = 1;
    VkDeviceSize bitstream_size_alignment = 1;
    StdVideoH264LevelIdc max_level = STD_VIDEO_H264_LEVEL_IDC_5_2;
    VkVideoDecodeCapabilityFlagsKHR decode_flags = 0;
    uint32_t max_dpb_slots = 0;
    uint32_t max_active_reference_pictures = 0;
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
    VkVideoCodecOperationFlagsKHR enabled_decode_operations = 0;
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

    VkFormat h264_picture_format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags h264_image_create_flags = 0;
    VkImageTiling h264_image_tiling = VK_IMAGE_TILING_OPTIMAL;
    bool video_maintenance2 = false;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
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
void destroy_h264_video_session(VulkanRuntime *runtime, H264VideoSession *session);
bool bind_video_session_memory(VulkanRuntime *runtime, VideoSession *session, char *reason, size_t reason_size);
void destroy_export_resource(VulkanRuntime *runtime, ExportResource *resource);
VkDeviceSize export_memory_bytes(const SurfaceResource *resource);
void retire_export_resource(SurfaceResource *resource);
void destroy_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface);
bool ensure_surface_resource(VulkanRuntime *runtime, VkvvSurface *surface, VkExtent2D extent, char *reason, size_t reason_size);
void destroy_upload_buffer(VulkanRuntime *runtime, UploadBuffer *upload);
bool create_upload_buffer(VulkanRuntime *runtime, const H264VideoSession *session, const VkvvH264DecodeInput *input, UploadBuffer *upload, char *reason, size_t reason_size);
bool ensure_command_resources(VulkanRuntime *runtime, char *reason, size_t reason_size);
bool submit_command_buffer_and_wait(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation);
bool reset_h264_session(VulkanRuntime *runtime, H264VideoSession *session, VkVideoSessionParametersKHR parameters, char *reason, size_t reason_size);
void add_image_layout_barrier(std::vector<VkImageMemoryBarrier2> *barriers, SurfaceResource *resource, VkImageLayout new_layout, VkAccessFlags2 dst_access);
VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource *resource, VkExtent2D coded_extent);

} // namespace vkvv

#endif
