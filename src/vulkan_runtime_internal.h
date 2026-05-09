#ifndef VKVV_VULKAN_RUNTIME_INTERNAL_H
#define VKVV_VULKAN_RUNTIME_INTERNAL_H

#include "vulkan_runtime.h"
#include "vulkan/video_profile.h"

#include <atomic>
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
    uint64_t driver_instance_id = 0;
    uint64_t stream_id = 0;
    VkVideoCodecOperationFlagsKHR codec_operation = 0;
    VASurfaceID owner_surface_id = VA_INVALID_ID;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned int va_fourcc = 0;
    VkDeviceSize allocation_size = 0;
    VkSubresourceLayout plane_layouts[2]{};
    uint32_t plane_count = 0;
    uint64_t drm_format_modifier = 0;
    bool has_drm_format_modifier = false;
    bool exported = false;
    bool predecode_exported = false;
    bool predecode_seeded = false;
    bool black_placeholder = false;
    VASurfaceID seed_source_surface_id = VA_INVALID_ID;
    uint64_t seed_source_generation = 0;
    uint64_t content_generation = 0;
    bool fd_stat_valid = false;
    uint64_t fd_dev = 0;
    uint64_t fd_ino = 0;
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
    uint64_t driver_instance_id = 0;
    uint64_t stream_id = 0;
    VkVideoCodecOperationFlagsKHR codec_operation = 0;
    VASurfaceID surface_id = VA_INVALID_ID;
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
    bool imported_external = false;
    uint32_t import_memory_type = 0;
    bool import_fd_stat_valid = false;
    uint64_t import_fd_dev = 0;
    uint64_t import_fd_ino = 0;
    ExportResource export_resource{};
    uint64_t content_generation = 0;
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

struct ExportSeedRecord {
    uint64_t driver_instance_id = 0;
    uint64_t stream_id = 0;
    VkVideoCodecOperationFlagsKHR codec_operation = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned int va_fourcc = 0;
    VkExtent2D coded_extent{};
    SurfaceResource *resource = nullptr;
    VASurfaceID surface_id = VA_INVALID_ID;
    uint64_t content_generation = 0;
};

class VulkanRuntime {
  public:
    ~VulkanRuntime();

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
    std::atomic_bool device_lost = false;

    bool video_decode_vp9 = false;
    bool video_maintenance2 = false;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkvvSurface *pending_surface = nullptr;
    VkVideoSessionParametersKHR pending_parameters = VK_NULL_HANDLE;
    VkDeviceSize pending_upload_allocation_size = 0;
    bool pending_displayable = true;
    char pending_operation[64]{};
    std::mutex command_mutex;
    std::mutex export_mutex;
    std::vector<ExportResource *> predecode_exports;
    std::vector<ExportResource> detached_exports;
    std::vector<ExportSeedRecord> export_seed_records;
    VkDeviceSize detached_export_memory_bytes = 0;
    VkDeviceSize detached_export_memory_budget = 384ull * 1024ull * 1024ull;
    size_t detached_export_count_limit = 48;
    size_t transition_export_cursor = 0;

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
    void destroy_detached_export_resources();
};

bool extension_present(const std::vector<VkExtensionProperties> &extensions, const char *name);
uint32_t round_up_16(uint32_t value);
VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment);
bool find_memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t *type_index);
bool enumerate_drm_format_modifiers(VulkanRuntime *runtime, VkFormat format, VkFormatFeatureFlags2 required, std::vector<uint64_t> *modifiers);

void destroy_video_session(VulkanRuntime *runtime, VideoSession *session);
bool bind_video_session_memory(VulkanRuntime *runtime, VideoSession *session, char *reason, size_t reason_size);
bool ensure_runtime_usable(VulkanRuntime *runtime, char *reason, size_t reason_size, const char *operation);
bool record_vk_result(
        VulkanRuntime *runtime,
        VkResult result,
        const char *call,
        const char *operation,
        char *reason,
        size_t reason_size);
void destroy_export_resource(VulkanRuntime *runtime, ExportResource *resource);
VkDeviceSize export_memory_bytes(const SurfaceResource *resource);
size_t runtime_detached_export_count(VulkanRuntime *runtime);
VkDeviceSize runtime_detached_export_memory_bytes(VulkanRuntime *runtime);
void prune_detached_exports_for_surface(
        VulkanRuntime *runtime,
        uint64_t driver_instance_id,
        VASurfaceID surface_id,
        uint64_t stream_id,
        VkVideoCodecOperationFlagsKHR codec_operation,
        unsigned int va_fourcc,
        VkFormat format,
        VkExtent2D coded_extent);
void prune_detached_exports_for_driver(VulkanRuntime *runtime, uint64_t driver_instance_id);
void detach_export_resource(VulkanRuntime *runtime, SurfaceResource *resource);
void register_predecode_export_resource(VulkanRuntime *runtime, ExportResource *resource);
void unregister_predecode_export_resource(VulkanRuntime *runtime, ExportResource *resource);
void unregister_predecode_export_resource_locked(VulkanRuntime *runtime, ExportResource *resource);
void remember_export_seed_resource(VulkanRuntime *runtime, SurfaceResource *resource);
void unregister_export_seed_resource(VulkanRuntime *runtime, SurfaceResource *resource);
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
void track_pending_decode(VulkanRuntime *runtime, VkvvSurface *surface, VkVideoSessionParametersKHR parameters, VkDeviceSize upload_allocation_size, bool displayable, const char *operation);
VAStatus drain_pending_work_before_sync_command(VulkanRuntime *runtime, char *reason, size_t reason_size);
void add_image_layout_barrier(std::vector<VkImageMemoryBarrier2> *barriers, SurfaceResource *resource, VkImageLayout new_layout, VkAccessFlags2 dst_access);
VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource *resource, VkExtent2D coded_extent);

} // namespace vkvv

#endif
