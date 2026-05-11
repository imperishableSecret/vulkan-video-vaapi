#ifndef VKVV_VULKAN_RUNTIME_INTERNAL_H
#define VKVV_VULKAN_RUNTIME_INTERNAL_H

#include "vulkan/runtime.h"
#include "vulkan/video_profile.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

namespace vkvv {

    inline constexpr uint32_t invalid_queue_family = UINT32_MAX;
    inline constexpr size_t   command_slot_count   = 4;

    struct ExportResource {
        VkImage                       image              = VK_NULL_HANDLE;
        VkDeviceMemory                memory             = VK_NULL_HANDLE;
        uint64_t                      driver_instance_id = 0;
        uint64_t                      stream_id          = 0;
        VkVideoCodecOperationFlagsKHR codec_operation    = 0;
        VASurfaceID                   owner_surface_id   = VA_INVALID_ID;
        VkExtent2D                    extent{};
        VkFormat                      format          = VK_FORMAT_UNDEFINED;
        unsigned int                  va_fourcc       = 0;
        VkDeviceSize                  allocation_size = 0;
        VkSubresourceLayout           plane_layouts[2]{};
        uint32_t                      plane_count             = 0;
        uint64_t                      drm_format_modifier     = 0;
        bool                          has_drm_format_modifier = false;
        bool                          exported                = false;
        bool                          predecode_exported      = false;
        bool                          predecode_seeded        = false;
        bool                          black_placeholder       = false;
        VASurfaceID                   seed_source_surface_id  = VA_INVALID_ID;
        uint64_t                      seed_source_generation  = 0;
        uint64_t                      content_generation      = 0;
        bool                          fd_stat_valid           = false;
        uint64_t                      fd_dev                  = 0;
        uint64_t                      fd_ino                  = 0;
        VkImageLayout                 layout                  = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct PredecodeExportRecord {
        ExportResource*               resource           = nullptr;
        VkImage                       image              = VK_NULL_HANDLE;
        VkDeviceMemory                memory             = VK_NULL_HANDLE;
        uint64_t                      driver_instance_id = 0;
        uint64_t                      stream_id          = 0;
        VkVideoCodecOperationFlagsKHR codec_operation    = 0;
        VASurfaceID                   owner_surface_id   = VA_INVALID_ID;
        VkFormat                      format             = VK_FORMAT_UNDEFINED;
        unsigned int                  va_fourcc          = 0;
        VkExtent2D                    extent{};
        uint64_t                      content_generation = 0;
    };

    enum class RetainedExportState {
        Detached,
        Attached,
        Expired,
    };

    struct RetainedExportBacking {
        ExportResource      resource{};
        VkvvFdIdentity      fd{};
        RetainedExportState state             = RetainedExportState::Detached;
        uint64_t            retained_sequence = 0;

        RetainedExportBacking()                                        = default;
        RetainedExportBacking(const RetainedExportBacking&)            = delete;
        RetainedExportBacking& operator=(const RetainedExportBacking&) = delete;
        RetainedExportBacking(RetainedExportBacking&& other) noexcept {
            *this = std::move(other);
        }
        RetainedExportBacking& operator=(RetainedExportBacking&& other) noexcept {
            if (this != &other) {
                resource                = other.resource;
                fd                      = other.fd;
                state                   = other.state;
                retained_sequence       = other.retained_sequence;
                other.resource          = {};
                other.fd                = {};
                other.state             = RetainedExportState::Expired;
                other.retained_sequence = 0;
            }
            return *this;
        }
    };

    enum class RetainedExportMatch {
        Match,
        MissingImport,
        MissingFd,
        FdMismatch,
        FourccMismatch,
        ModifierMismatch,
        FormatMismatch,
        ExtentMismatch,
    };

    struct RetainedExportBudget {
        size_t       target_count     = 0;
        VkDeviceSize target_bytes     = 0;
        VkDeviceSize global_cap_bytes = 0;
        size_t       headroom_count   = 0;
        VkDeviceSize average_bytes    = 0;
    };

    struct RetainedExportStats {
        size_t       count                     = 0;
        VkDeviceSize bytes                     = 0;
        VkDeviceSize accounted_bytes           = 0;
        bool         accounting_valid          = true;
        size_t       count_limit               = 0;
        VkDeviceSize memory_budget             = 0;
        bool         transition_active         = false;
        size_t       transition_retained_count = 0;
        VkDeviceSize transition_retained_bytes = 0;
        size_t       transition_target_count   = 0;
        VkDeviceSize transition_target_bytes   = 0;
    };

    struct TransitionRetentionWindow {
        bool                          active             = false;
        uint64_t                      driver_instance_id = 0;
        uint64_t                      stream_id          = 0;
        VkVideoCodecOperationFlagsKHR codec_operation    = 0;
        VkFormat                      format             = VK_FORMAT_UNDEFINED;
        unsigned int                  va_fourcc          = 0;
        VkExtent2D                    coded_extent{};
        size_t                        retained_count = 0;
        VkDeviceSize                  retained_bytes = 0;
        size_t                        attached_count = 0;
        RetainedExportBudget          budget{};
    };

    struct DecodeImageKey {
        VkVideoCodecOperationFlagsKHR    codec_operation          = 0;
        uint32_t                         codec_profile            = 0;
        VkFormat                         picture_format           = VK_FORMAT_UNDEFINED;
        VkFormat                         reference_picture_format = VK_FORMAT_UNDEFINED;
        unsigned int                     va_rt_format             = 0;
        unsigned int                     va_fourcc                = 0;
        VkExtent2D                       coded_extent{};
        VkImageUsageFlags                usage              = 0;
        VkImageCreateFlags               create_flags       = 0;
        VkImageTiling                    tiling             = VK_IMAGE_TILING_OPTIMAL;
        VkVideoChromaSubsamplingFlagsKHR chroma_subsampling = 0;
        VkVideoComponentBitDepthFlagsKHR luma_bit_depth     = 0;
        VkVideoComponentBitDepthFlagsKHR chroma_bit_depth   = 0;
    };

    struct SurfaceResource {
        VkImage                       image              = VK_NULL_HANDLE;
        VkImageView                   view               = VK_NULL_HANDLE;
        VkDeviceMemory                memory             = VK_NULL_HANDLE;
        uint64_t                      driver_instance_id = 0;
        uint64_t                      stream_id          = 0;
        VkVideoCodecOperationFlagsKHR codec_operation    = 0;
        VASurfaceID                   surface_id         = VA_INVALID_ID;
        VkExtent2D                    extent{};
        VkExtent2D                    coded_extent{};
        VkExtent2D                    visible_extent{};
        VkFormat                      format       = VK_FORMAT_UNDEFINED;
        unsigned int                  va_rt_format = 0;
        unsigned int                  va_fourcc    = 0;
        DecodeImageKey                decode_key{};
        VkDeviceSize                  allocation_size = 0;
        VkSubresourceLayout           plane_layouts[2]{};
        uint32_t                      plane_count             = 0;
        uint64_t                      drm_format_modifier     = 0;
        bool                          exportable              = false;
        bool                          has_drm_format_modifier = false;
        bool                          exported                = false;
        VkvvExternalSurfaceImport     import;
        uint64_t                      last_nondisplay_skip_generation        = 0;
        uint64_t                      last_nondisplay_skip_shadow_generation = 0;
        VkDeviceMemory                last_nondisplay_skip_shadow_memory     = VK_NULL_HANDLE;
        uint64_t                      last_display_refresh_generation        = 0;
        ExportResource                export_resource{};
        uint64_t                      content_generation     = 0;
        uint64_t                      export_seed_generation = 0;
        VkImageLayout                 layout                 = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct UploadBuffer {
        VkBuffer       buffer           = VK_NULL_HANDLE;
        VkDeviceMemory memory           = VK_NULL_HANDLE;
        void*          mapped           = nullptr;
        VkDeviceSize   size             = 0;
        VkDeviceSize   capacity         = 0;
        VkDeviceSize   allocation_size  = 0;
        uint32_t       underused_frames = 0;
        bool           coherent         = true;
    };

    enum class CommandUse {
        Idle,
        Decode,
        Export,
        SessionReset,
        Encode,
    };

    inline const char* command_use_name(CommandUse use) {
        switch (use) {
            case CommandUse::Idle: return "idle";
            case CommandUse::Decode: return "decode";
            case CommandUse::Export: return "export";
            case CommandUse::SessionReset: return "session-reset";
            case CommandUse::Encode: return "encode";
            default: return "unknown";
        }
    }

    struct PendingWork {
        VkvvSurface*                surface                = nullptr;
        VkVideoSessionParametersKHR parameters             = VK_NULL_HANDLE;
        VkDeviceSize                upload_allocation_size = 0;
        bool                        refresh_export         = true;
        CommandUse                  use                    = CommandUse::Idle;
        char                        operation[64]{};
    };

    inline bool pending_work_has_payload(const PendingWork& work) {
        return work.surface != nullptr || work.parameters != VK_NULL_HANDLE;
    }

    inline bool pending_work_surface_is_destroying(const PendingWork& work) {
        return work.surface != nullptr && work.surface->destroying;
    }

    inline void reset_pending_work(PendingWork* work) {
        if (work != nullptr) {
            *work = {};
        }
    }

    struct CommandSlot {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkFence         fence          = VK_NULL_HANDLE;
        PendingWork     pending{};
        bool            submitted     = false;
        CommandUse      submitted_use = CommandUse::Idle;
    };

    struct VideoSessionKey {
        VkVideoCodecOperationFlagsKHR    codec_operation          = 0;
        uint32_t                         codec_profile            = 0;
        VkFormat                         picture_format           = VK_FORMAT_UNDEFINED;
        VkFormat                         reference_picture_format = VK_FORMAT_UNDEFINED;
        VkExtent2D                       max_coded_extent{};
        VkImageUsageFlags                image_usage        = 0;
        VkImageCreateFlags               image_create_flags = 0;
        VkImageTiling                    image_tiling       = VK_IMAGE_TILING_OPTIMAL;
        VkVideoChromaSubsamplingFlagsKHR chroma_subsampling = 0;
        VkVideoComponentBitDepthFlagsKHR luma_bit_depth     = 0;
        VkVideoComponentBitDepthFlagsKHR chroma_bit_depth   = 0;
    };

    struct VideoSession {
        VkVideoSessionKHR           session = VK_NULL_HANDLE;
        std::vector<VkDeviceMemory> memory;
        VkDeviceSize                memory_bytes = 0;
        VideoSessionKey             key{};
        bool                        initialized = false;
    };

    struct ExportSeedRecord {
        uint64_t                      driver_instance_id = 0;
        uint64_t                      stream_id          = 0;
        VkVideoCodecOperationFlagsKHR codec_operation    = 0;
        VkFormat                      format             = VK_FORMAT_UNDEFINED;
        unsigned int                  va_fourcc          = 0;
        VkExtent2D                    coded_extent{};
        SurfaceResource*              resource           = nullptr;
        VASurfaceID                   surface_id         = VA_INVALID_ID;
        uint64_t                      content_generation = 0;
    };

    struct VkvvCodecPerfCounters {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> completed{0};
    };

    struct VkvvPerfCounters {
        std::atomic<uint64_t> decode_submitted{0};
        std::atomic<uint64_t> decode_completed{0};
        std::atomic<uint64_t> command_fence_polls{0};
        std::atomic<uint64_t> command_fence_waits{0};
        std::atomic<uint64_t> command_fence_wait_ns{0};
        std::atomic<uint64_t> upload_bytes{0};
        std::atomic<uint64_t> upload_high_water{0};
        std::atomic<uint64_t> pending_high_water{0};
        std::atomic<uint64_t> decode_image_high_water{0};
        std::atomic<uint64_t> export_image_high_water{0};
        std::atomic<uint64_t> video_session_high_water{0};
        std::atomic<uint64_t> export_refresh_requested{0};
        std::atomic<uint64_t> export_refresh_skipped{0};
        std::atomic<uint64_t> export_refresh_no_backing{0};
        std::atomic<uint64_t> export_copy_count{0};
        std::atomic<uint64_t> export_copy_targets{0};
        std::atomic<uint64_t> export_copy_bytes{0};
        std::atomic<uint64_t> export_copy_wait_ns{0};
        std::atomic<uint64_t> retained_export_pruned{0};
        std::atomic<uint64_t> retained_export_removed{0};
        std::atomic<uint64_t> summary_fingerprint{0};
        VkvvCodecPerfCounters h264_decode;
        VkvvCodecPerfCounters h265_decode;
        VkvvCodecPerfCounters vp9_decode;
        VkvvCodecPerfCounters av1_decode;
    };

    inline void perf_update_high_water(std::atomic<uint64_t>& high_water, uint64_t value) {
        uint64_t current = high_water.load(std::memory_order_relaxed);
        while (current < value && !high_water.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    inline VkvvCodecPerfCounters* perf_decode_codec_counters(VkvvPerfCounters* counters, VkVideoCodecOperationFlagsKHR codec_operation) {
        if (counters == nullptr) {
            return nullptr;
        }
        switch (codec_operation) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: return &counters->h264_decode;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: return &counters->h265_decode;
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR: return &counters->vp9_decode;
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: return &counters->av1_decode;
            default: return nullptr;
        }
    }

    class VulkanRuntime {
      public:
        ~VulkanRuntime();

        VkInstance                                      instance                  = VK_NULL_HANDLE;
        VkPhysicalDevice                                physical_device           = VK_NULL_HANDLE;
        VkDevice                                        device                    = VK_NULL_HANDLE;
        VkQueue                                         decode_queue              = VK_NULL_HANDLE;
        uint32_t                                        decode_queue_family       = invalid_queue_family;
        VkVideoCodecOperationFlagsKHR                   probed_decode_operations  = 0;
        VkVideoCodecOperationFlagsKHR                   enabled_decode_operations = 0;
        VkVideoCodecOperationFlagsKHR                   probed_encode_operations  = 0;
        VkVideoCodecOperationFlagsKHR                   enabled_encode_operations = 0;
        VkPhysicalDeviceProperties                      device_properties{};
        VkPhysicalDeviceMemoryProperties                memory_properties{};

        PFN_vkGetPhysicalDeviceQueueFamilyProperties2   get_queue_family_properties2             = nullptr;
        PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR     get_video_capabilities                   = nullptr;
        PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR get_video_format_properties              = nullptr;
        PFN_vkCreateVideoSessionKHR                     create_video_session                     = nullptr;
        PFN_vkDestroyVideoSessionKHR                    destroy_video_session                    = nullptr;
        PFN_vkGetVideoSessionMemoryRequirementsKHR      get_video_session_memory_requirements    = nullptr;
        PFN_vkBindVideoSessionMemoryKHR                 bind_video_session_memory                = nullptr;
        PFN_vkCreateVideoSessionParametersKHR           create_video_session_parameters          = nullptr;
        PFN_vkDestroyVideoSessionParametersKHR          destroy_video_session_parameters         = nullptr;
        PFN_vkCmdBeginVideoCodingKHR                    cmd_begin_video_coding                   = nullptr;
        PFN_vkCmdEndVideoCodingKHR                      cmd_end_video_coding                     = nullptr;
        PFN_vkCmdControlVideoCodingKHR                  cmd_control_video_coding                 = nullptr;
        PFN_vkCmdDecodeVideoKHR                         cmd_decode_video                         = nullptr;
        PFN_vkGetMemoryFdKHR                            get_memory_fd                            = nullptr;
        PFN_vkGetImageDrmFormatModifierPropertiesEXT    get_image_drm_format_modifier_properties = nullptr;

        bool                                            external_memory_fd        = false;
        bool                                            external_memory_dma_buf   = false;
        bool                                            image_drm_format_modifier = false;
        bool                                            surface_export            = false;
        std::atomic_bool                                device_lost               = false;

        bool                                            video_decode_vp9   = false;
        bool                                            video_maintenance2 = false;

        VkCommandPool                                   command_pool   = VK_NULL_HANDLE;
        VkCommandBuffer                                 command_buffer = VK_NULL_HANDLE;
        VkFence                                         fence          = VK_NULL_HANDLE;
        CommandSlot                                     command_slots[command_slot_count];
        size_t                                          active_command_slot = 0;
        // Locking model:
        // - command_mutex protects command slots and pending-work state.
        // - export_mutex protects retained/predecode/seed export registries.
        // - do not hold command_mutex while publishing export state; export copy may
        //   briefly take command_mutex while export_mutex is held, then drops
        //   export_mutex before waiting on the GPU fence.
        std::mutex                         command_mutex;
        std::mutex                         export_mutex;
        std::vector<PredecodeExportRecord> predecode_exports;
        std::vector<RetainedExportBacking> retained_exports;
        std::vector<ExportSeedRecord>      export_seed_records;
        VkDeviceSize                       retained_export_memory_bytes  = 0;
        VkDeviceSize                       retained_export_memory_budget = 64ull * 1024ull * 1024ull;
        size_t                             retained_export_count_limit   = 4;
        uint64_t                           retained_export_sequence      = 0;
        TransitionRetentionWindow          transition_retention{};
        VkvvPerfCounters                   perf{};

        void                               destroy_command_resources() {
            for (CommandSlot& slot : command_slots) {
                if (slot.fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device, slot.fence, nullptr);
                }
                slot = {};
            }
            if (command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, command_pool, nullptr);
                command_pool   = VK_NULL_HANDLE;
                command_buffer = VK_NULL_HANDLE;
            }
            fence               = VK_NULL_HANDLE;
            active_command_slot = 0;
        }
        void destroy_detached_export_resources();
    };

    bool                      extension_present(const std::vector<VkExtensionProperties>& extensions, const char* name);
    uint32_t                  round_up_16(uint32_t value);
    VkDeviceSize              align_up(VkDeviceSize value, VkDeviceSize alignment);
    bool                      find_memory_type(const VkPhysicalDeviceMemoryProperties& properties, uint32_t type_bits, VkMemoryPropertyFlags required, uint32_t* type_index);
    bool                      enumerate_drm_format_modifiers(VulkanRuntime* runtime, VkFormat format, VkFormatFeatureFlags2 required, std::vector<uint64_t>* modifiers);

    void                      destroy_video_session(VulkanRuntime* runtime, VideoSession* session);
    bool                      bind_video_session_memory(VulkanRuntime* runtime, VideoSession* session, char* reason, size_t reason_size);
    bool                      ensure_runtime_usable(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation);
    bool                      record_vk_result(VulkanRuntime* runtime, VkResult result, const char* call, const char* operation, char* reason, size_t reason_size);
    void                      destroy_export_resource(VulkanRuntime* runtime, ExportResource* resource);
    VkDeviceSize              export_memory_bytes(const SurfaceResource* resource);
    VkvvFdIdentity            retained_export_fd_identity(const ExportResource& resource);
    VkvvExternalImageIdentity retained_export_image_identity(const ExportResource& resource);
    RetainedExportMatch       retained_export_match_import(const RetainedExportBacking& backing, const VkvvExternalSurfaceImport& import, unsigned int va_fourcc, VkFormat format,
                                                           VkExtent2D coded_extent);
    const char*               retained_export_match_reason(RetainedExportMatch match);
    bool                      retained_export_matches_window(const ExportResource& resource, const TransitionRetentionWindow& window);
    bool                      retained_export_seed_can_replace_window(const TransitionRetentionWindow& window, const ExportResource& seed);
    VkDeviceSize              retained_export_global_cap_bytes(const VkPhysicalDeviceMemoryProperties& properties);
    RetainedExportBudget      retained_export_budget_from_expected(size_t expected_count, VkDeviceSize expected_bytes, VkDeviceSize global_cap_bytes);
    size_t                    runtime_retained_export_count(VulkanRuntime* runtime);
    VkDeviceSize              runtime_retained_export_memory_bytes(VulkanRuntime* runtime);
    RetainedExportStats       runtime_retained_export_stats(VulkanRuntime* runtime);
    VkDeviceSize              runtime_retained_export_accounted_bytes(VulkanRuntime* runtime);
    bool                      runtime_retained_export_memory_accounting_valid(VulkanRuntime* runtime);
    size_t                    runtime_detached_export_count(VulkanRuntime* runtime);
    VkDeviceSize              runtime_detached_export_memory_bytes(VulkanRuntime* runtime);
    void                      prune_detached_exports_for_surface(VulkanRuntime* runtime, uint64_t driver_instance_id, VASurfaceID surface_id, uint64_t stream_id,
                                                                 VkVideoCodecOperationFlagsKHR codec_operation, unsigned int va_fourcc, VkFormat format, VkExtent2D coded_extent);
    void                      prune_detached_exports_for_driver(VulkanRuntime* runtime, uint64_t driver_instance_id);
    void                      detach_export_resource(VulkanRuntime* runtime, SurfaceResource* resource);
    void                      note_retained_export_attached_locked(VulkanRuntime* runtime);
    void                      register_predecode_export_resource(VulkanRuntime* runtime, ExportResource* resource);
    void                      unregister_predecode_export_resource(VulkanRuntime* runtime, ExportResource* resource);
    void                      unregister_predecode_export_resource_locked(VulkanRuntime* runtime, ExportResource* resource);
    PredecodeExportRecord     make_predecode_export_record(ExportResource* resource);
    bool                      predecode_export_record_matches_resource(const PredecodeExportRecord& record, const ExportResource* resource);
    bool                      predecode_export_record_still_valid(const PredecodeExportRecord& record);
    void                      remember_export_seed_resource(VulkanRuntime* runtime, SurfaceResource* resource);
    void                      unregister_export_seed_resource(VulkanRuntime* runtime, SurfaceResource* resource);
    void                      destroy_surface_resource(VulkanRuntime* runtime, VkvvSurface* surface);
    bool                      decode_image_key_matches(const DecodeImageKey& existing, const DecodeImageKey& requested);
    bool                      ensure_surface_resource(VulkanRuntime* runtime, VkvvSurface* surface, const DecodeImageKey& key, char* reason, size_t reason_size);
    void                      destroy_upload_buffer(VulkanRuntime* runtime, UploadBuffer* upload);
    bool     ensure_bitstream_upload_buffer(VulkanRuntime* runtime, const VideoProfileSpec& profile_spec, const void* data, size_t data_size, VkDeviceSize size_alignment,
                                            VkBufferUsageFlags usage, UploadBuffer* upload, const char* label, char* reason, size_t reason_size);
    bool     ensure_command_resources(VulkanRuntime* runtime, char* reason, size_t reason_size);
    bool     submit_command_buffer(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation, CommandUse use = CommandUse::Decode);
    bool     wait_for_command_fence(VulkanRuntime* runtime, uint64_t timeout_ns, char* reason, size_t reason_size, const char* operation);
    bool     submit_command_buffer_and_wait(VulkanRuntime* runtime, char* reason, size_t reason_size, const char* operation, CommandUse use = CommandUse::Decode);
    void     track_pending_decode(VulkanRuntime* runtime, VkvvSurface* surface, VkVideoSessionParametersKHR parameters, VkDeviceSize upload_allocation_size, bool refresh_export,
                                  const char* operation);
    size_t   runtime_pending_work_count(VulkanRuntime* runtime);
    bool     runtime_surface_has_pending_work(VulkanRuntime* runtime, const VkvvSurface* surface);
    bool     runtime_surface_has_pending_export_refresh_work(VulkanRuntime* runtime, const VkvvSurface* surface);
    VAStatus ensure_command_slot_capacity(VulkanRuntime* runtime, const char* operation, char* reason, size_t reason_size);
    VAStatus complete_pending_surface_work_if_needed(VulkanRuntime* runtime, VkvvSurface* surface, const char* operation, char* reason, size_t reason_size);
    VAStatus drain_pending_surface_work_before_sync_command(VulkanRuntime* runtime, VkvvSurface* surface, char* reason, size_t reason_size);
    VAStatus drain_pending_work_before_sync_command(VulkanRuntime* runtime, char* reason, size_t reason_size);
    void     discard_pending_work_for_teardown(VulkanRuntime* runtime, VAStatus status, const char* reason);
    void     add_image_layout_barrier(std::vector<VkImageMemoryBarrier2>* barriers, SurfaceResource* resource, VkImageLayout new_layout, VkAccessFlags2 dst_access);
    VkVideoPictureResourceInfoKHR make_picture_resource(SurfaceResource* resource, VkExtent2D coded_extent);

} // namespace vkvv

#endif
