#ifndef VKVV_VULKAN_AV1_INTERNAL_H
#define VKVV_VULKAN_AV1_INTERNAL_H

#include "vulkan/runtime_internal.h"
#include "codecs/av1/av1.h"

#include <array>
#include <vector>

namespace vkvv {

    inline constexpr uint32_t         max_av1_reference_slots   = VKVV_AV1_REFERENCE_COUNT;
    inline constexpr uint32_t         max_av1_active_references = VKVV_AV1_ACTIVE_REFERENCE_COUNT;
    inline constexpr uint32_t         max_av1_dpb_slots         = 10;
    inline constexpr uint32_t         min_av1_dpb_slots         = VKVV_AV1_MIN_DPB_SLOTS;
    inline constexpr uint32_t         min_av1_active_references = VKVV_AV1_MIN_ACTIVE_REFERENCES;

    inline constexpr VideoProfileSpec av1_profile0_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .std_profile = STD_VIDEO_AV1_PROFILE_MAIN,
    };

    inline constexpr VideoProfileSpec av1_profile0_10bit_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
        .std_profile = STD_VIDEO_AV1_PROFILE_MAIN,
    };

    struct AV1ReferenceSlot {
        VASurfaceID                    surface_id = VA_INVALID_ID;
        int                            slot       = -1;
        StdVideoDecodeAV1ReferenceInfo info{};
        bool                           has_metadata = false;
        struct Metadata {
            uint64_t                      driver_instance_id = 0;
            uint64_t                      stream_id          = 0;
            VkVideoCodecOperationFlagsKHR codec_operation    = 0;
            VASurfaceID                   surface_id         = VA_INVALID_ID;
            uint64_t                      content_generation = 0;
            DecodeImageKey                decode_key{};
            VkExtent2D                    coded_extent{};
            unsigned int                  va_rt_format = 0;
            unsigned int                  va_fourcc    = 0;
            uint8_t                       bit_depth    = 0;
            uint32_t                      frame_id     = 0;
            bool                          showable     = false;
            bool                          displayed    = false;
        } metadata{};
    };

    using AV1ReferenceMetadata = AV1ReferenceSlot::Metadata;

    struct AV1VideoSession {
        VAProfile                                                 va_profile        = VAProfileAV1Profile0;
        unsigned int                                              va_rt_format      = VA_RT_FORMAT_YUV420;
        unsigned int                                              va_fourcc         = VA_FOURCC_NV12;
        uint8_t                                                   bitstream_profile = 0;
        uint8_t                                                   bit_depth         = 8;
        VideoProfileSpec                                          profile_spec      = av1_profile0_spec;
        VideoSession                                              video;
        std::array<UploadBuffer, command_slot_count>              uploads;
        AV1ReferenceSlot                                          reference_slots[max_av1_reference_slots]{};
        std::vector<AV1ReferenceSlot>                             surface_slots;
        std::array<SurfaceResource*, max_av1_dpb_slots>           retained_dpb_resources{};
        VkDeviceSize                                              bitstream_offset_alignment    = 1;
        VkDeviceSize                                              bitstream_size_alignment      = 1;
        StdVideoAV1Level                                          max_level                     = STD_VIDEO_AV1_LEVEL_6_2;
        VkVideoDecodeCapabilityFlagsKHR                           decode_flags                  = 0;
        uint32_t                                                  next_dpb_slot                 = 0;
        uint32_t                                                  max_dpb_slots                 = 0;
        uint32_t                                                  max_active_reference_pictures = 0;
        uint64_t                                                  frame_sequence                = 0;
        bool                                                      has_last_visible_order_hint   = false;
        uint32_t                                                  last_visible_order_hint       = 0;
        uint64_t                                                  last_visible_order_frame_seq  = 0;
        VASurfaceID                                               last_visible_order_surface    = VA_INVALID_ID;
        bool                                                      has_sequence_key              = false;
        VkvvAV1SequenceHeader                                     sequence_key{};
        std::array<int8_t, STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME>    loop_filter_ref_deltas{1, 0, 0, 0, -1, 0, -1, -1};
        std::array<int8_t, STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS> loop_filter_mode_deltas{0, 0};
    };

    struct AV1SessionStdParameters {
        StdVideoAV1ColorConfig    color{};
        StdVideoAV1TimingInfo     timing{};
        StdVideoAV1SequenceHeader sequence{};
    };

    struct AV1PictureStdData {
        std::array<uint16_t, STD_VIDEO_AV1_MAX_TILE_COLS + 1> mi_col_starts{};
        std::array<uint16_t, STD_VIDEO_AV1_MAX_TILE_ROWS + 1> mi_row_starts{};
        std::array<uint16_t, STD_VIDEO_AV1_MAX_TILE_COLS>     width_in_sbs_minus1{};
        std::array<uint16_t, STD_VIDEO_AV1_MAX_TILE_ROWS>     height_in_sbs_minus1{};
        StdVideoAV1TileInfo                                   tile_info{};
        StdVideoAV1Quantization                               quantization{};
        StdVideoAV1Segmentation                               segmentation{};
        StdVideoAV1LoopFilter                                 loop_filter{};
        StdVideoAV1CDEF                                       cdef{};
        StdVideoAV1LoopRestoration                            restoration{};
        StdVideoAV1GlobalMotion                               global_motion{};
        StdVideoDecodeAV1PictureInfo                          picture{};
    };

    void                    destroy_av1_video_session(VulkanRuntime* runtime, AV1VideoSession* session);
    VkImageUsageFlags       av1_surface_image_usage();
    bool                    reset_av1_session(VulkanRuntime* runtime, AV1VideoSession* session, VkVideoSessionParametersKHR parameters, char* reason, size_t reason_size);
    bool                    av1_target_surface_needs_detach(const AV1VideoSession* session, const VkvvAV1DecodeInput* input, VASurfaceID target_surface_id);
    SurfaceResource*        av1_retained_dpb_resource_for_slot(const AV1VideoSession* session, int slot);
    void                    av1_release_retained_dpb_resource(VulkanRuntime* runtime, AV1VideoSession* session, int slot);
    void                    av1_release_unreferenced_retained_dpb_resources(VulkanRuntime* runtime, AV1VideoSession* session);
    void                    av1_detach_target_dpb_resource(VulkanRuntime* runtime, AV1VideoSession* session, VkvvSurface* target, VASurfaceID target_surface_id);
    int                     av1_dpb_slot_for_surface(const AV1VideoSession* session, VASurfaceID surface_id);
    const AV1ReferenceSlot* av1_reference_slot_for_index(const AV1VideoSession* session, uint32_t reference_index);
    const AV1ReferenceSlot* av1_reference_slot_for_surface(const AV1VideoSession* session, VASurfaceID surface_id);
    const AV1ReferenceSlot* av1_surface_slot_for_surface(const AV1VideoSession* session, VASurfaceID surface_id);
    const AV1ReferenceSlot* av1_reconcile_reference_slot(AV1VideoSession* session, uint32_t reference_index, VASurfaceID surface_id);
    void                    av1_set_reference_slot(AV1VideoSession* session, uint32_t reference_index, VASurfaceID surface_id, int slot, const StdVideoDecodeAV1ReferenceInfo& info,
                                                   const AV1ReferenceMetadata* metadata = nullptr);
    void                    av1_set_surface_slot(AV1VideoSession* session, VASurfaceID surface_id, int slot, const StdVideoDecodeAV1ReferenceInfo& info,
                                                 const AV1ReferenceMetadata* metadata = nullptr);
    void                    av1_clear_reference_slot(AV1VideoSession* session, int slot);
    void                    av1_clear_surface_slot(AV1VideoSession* session, VASurfaceID surface_id);
    bool                    validate_av1_reference_slot(const AV1VideoSession* session, const AV1ReferenceSlot* slot, const VkvvSurface* surface, const SurfaceResource* resource,
                                                        const VkvvDriver* drv, const VkvvContext* vctx, const DecodeImageKey& current_decode_key, char* reason, size_t reason_size);
    void                    av1_mark_retained_reference_slots(const AV1VideoSession* session, const VkvvAV1DecodeInput* input, bool used_slots[max_av1_dpb_slots]);
    int                     av1_reserved_scratch_dpb_slot(const AV1VideoSession* session);
    int                     av1_select_target_dpb_slot(AV1VideoSession* session, VASurfaceID target_surface_id, const bool used_slots[max_av1_dpb_slots]);
    int            av1_select_current_setup_slot(AV1VideoSession* session, VASurfaceID target_surface_id, const bool used_slots[max_av1_dpb_slots], bool current_is_reference);
    VkImageLayout  av1_target_layout(bool has_setup_slot);
    VkAccessFlags2 av1_target_access(bool has_setup_slot);
    bool           av1_decode_needs_export_refresh(const VkvvAV1DecodeInput* input);
    void           av1_update_reference_slots_from_refresh(AV1VideoSession* session, const VkvvAV1DecodeInput* input, VASurfaceID target_surface_id, int target_slot,
                                                           const StdVideoDecodeAV1ReferenceInfo& info, const AV1ReferenceMetadata* metadata = nullptr);
    int            allocate_av1_dpb_slot(AV1VideoSession* session, const bool used_slots[max_av1_dpb_slots]);
    void           build_av1_session_parameters(const VkvvAV1DecodeInput* input, AV1SessionStdParameters* std_params);
    bool           validate_av1_switch_frame(const VkvvAV1DecodeInput* input, char* reason, size_t reason_size);
    bool           build_av1_picture_std_data(AV1VideoSession* session, const VkvvAV1DecodeInput* input, AV1PictureStdData* std_data, char* reason, size_t reason_size);
    StdVideoDecodeAV1ReferenceInfo build_av1_current_reference_info(const VkvvAV1DecodeInput* input);
    const AV1ReferenceSlot*        validate_av1_show_existing_reference(const AV1VideoSession* session, const VkvvAV1DecodeInput* input, const VkvvSurface* surface,
                                                                        const SurfaceResource* resource, const VkvvDriver* drv, const VkvvContext* vctx,
                                                                        const DecodeImageKey& current_decode_key, char* reason, size_t reason_size);
    bool create_av1_session_parameters(VulkanRuntime* runtime, AV1VideoSession* session, const AV1SessionStdParameters* std_params, VkVideoSessionParametersKHR* parameters,
                                       char* reason, size_t reason_size);

} // namespace vkvv

#endif
