#ifndef VKVV_VULKAN_H264_INTERNAL_H
#define VKVV_VULKAN_H264_INTERNAL_H

#include "vulkan/runtime_internal.h"
#include "codecs/h264/h264.h"

namespace vkvv {

    inline constexpr uint32_t max_va_h264_reference_frames = 16;
    inline constexpr uint32_t max_h264_dpb_slots           = 17;

    struct H264SurfaceDpbSlot {
        VASurfaceID surface_id = VA_INVALID_ID;
        int         slot       = -1;
    };

    struct H264VideoSession {
        VideoSession                    video;
        UploadBuffer                    upload;
        std::vector<H264SurfaceDpbSlot> surface_slots;
        VkDeviceSize                    bitstream_offset_alignment    = 1;
        VkDeviceSize                    bitstream_size_alignment      = 1;
        StdVideoH264LevelIdc            max_level                     = STD_VIDEO_H264_LEVEL_IDC_5_2;
        VkVideoDecodeCapabilityFlagsKHR decode_flags                  = 0;
        uint32_t                        next_dpb_slot                 = 0;
        uint32_t                        max_dpb_slots                 = 0;
        uint32_t                        max_active_reference_pictures = 0;
    };

    inline constexpr VideoProfileSpec h264_profile_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .std_profile = STD_VIDEO_H264_PROFILE_IDC_HIGH,
    };

    struct H264StdParameters {
        StdVideoH264SequenceParameterSet    sps{};
        StdVideoH264SequenceParameterSetVui vui{};
        StdVideoH264HrdParameters           vui_hrd{};
        StdVideoH264ScalingLists            sps_scaling{};
        StdVideoH264ScalingLists            pps_scaling{};
        StdVideoH264PictureParameterSet     pps{};
    };

    void              destroy_h264_video_session(VulkanRuntime* runtime, H264VideoSession* session);
    VkImageUsageFlags h264_surface_image_usage();
    bool              reset_h264_session(VulkanRuntime* runtime, H264VideoSession* session, VkVideoSessionParametersKHR parameters, char* reason, size_t reason_size);
    bool              h264_picture_is_invalid(const VAPictureH264& picture);
    int               h264_dpb_slot_for_surface(const H264VideoSession* session, VASurfaceID surface_id);
    void              h264_set_dpb_slot_for_surface(H264VideoSession* session, VASurfaceID surface_id, int slot);
    int               allocate_dpb_slot(H264VideoSession* session, const bool used_slots[max_h264_dpb_slots]);
    void              fill_reference_info(const VAPictureH264& picture, uint16_t frame_num, StdVideoDecodeH264ReferenceInfo* info);
    bool              bitstream_has_idr(const VkvvH264DecodeInput* input);
    void              build_h264_std_parameters(H264VideoSession* session, VAProfile profile, const VkvvH264DecodeInput* input, H264StdParameters* std_params);
    bool create_h264_session_parameters(VulkanRuntime* runtime, H264VideoSession* session, const H264StdParameters* std_params, VkVideoSessionParametersKHR* parameters,
                                        char* reason, size_t reason_size);
    bool create_empty_h264_session_parameters(VulkanRuntime* runtime, H264VideoSession* session, VkVideoSessionParametersKHR* parameters, char* reason, size_t reason_size);

} // namespace vkvv

#endif
