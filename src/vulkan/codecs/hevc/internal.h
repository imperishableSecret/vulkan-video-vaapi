#ifndef VKVV_VULKAN_HEVC_INTERNAL_H
#define VKVV_VULKAN_HEVC_INTERNAL_H

#include "vulkan/runtime_internal.h"
#include "codecs/hevc/hevc.h"

#include <array>

namespace vkvv {

    inline constexpr uint32_t         max_va_hevc_reference_frames = 15;
    inline constexpr uint32_t         max_hevc_dpb_slots           = 16;

    inline constexpr VideoProfileSpec hevc_main_profile_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .std_profile = STD_VIDEO_H265_PROFILE_IDC_MAIN,
    };

    inline constexpr VideoProfileSpec hevc_main10_profile_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
        .std_profile = STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
    };

    struct HEVCSurfaceDpbSlot {
        VASurfaceID surface_id = VA_INVALID_ID;
        int         slot       = -1;
    };

    struct HEVCDpbReference {
        uint8_t  slot          = STD_VIDEO_H265_NO_REFERENCE_PICTURE;
        int32_t  pic_order_cnt = 0;
        uint32_t flags         = 0;
    };

    struct HEVCRpsCounts {
        uint32_t st_curr_before = 0;
        uint32_t st_curr_after  = 0;
        uint32_t lt_curr        = 0;
    };

    struct HEVCVideoSession {
        VAProfile                                    va_profile   = VAProfileHEVCMain;
        unsigned int                                 va_rt_format = VA_RT_FORMAT_YUV420;
        unsigned int                                 va_fourcc    = VA_FOURCC_NV12;
        uint8_t                                      bit_depth    = 8;
        VideoProfileSpec                             profile_spec = hevc_main_profile_spec;
        VideoSession                                 video;
        std::array<UploadBuffer, command_slot_count> uploads;
        std::vector<HEVCSurfaceDpbSlot>              surface_slots;
        VkDeviceSize                                 bitstream_offset_alignment    = 1;
        VkDeviceSize                                 bitstream_size_alignment      = 1;
        StdVideoH265LevelIdc                         max_level                     = STD_VIDEO_H265_LEVEL_IDC_6_2;
        VkVideoDecodeCapabilityFlagsKHR              decode_flags                  = 0;
        uint32_t                                     next_dpb_slot                 = 0;
        uint32_t                                     max_dpb_slots                 = 0;
        uint32_t                                     max_active_reference_pictures = 0;
    };

    struct HEVCStdParameters {
        StdVideoH265ProfileTierLevel        ptl{};
        StdVideoH265DecPicBufMgr            vps_dpb{};
        StdVideoH265VideoParameterSet       vps{};
        StdVideoH265ScalingLists            scaling{};
        StdVideoH265DecPicBufMgr            sps_dpb{};
        StdVideoH265SequenceParameterSetVui sps_vui{};
        StdVideoH265SequenceParameterSet    sps{};
        StdVideoH265PictureParameterSet     pps{};
    };

    void              destroy_hevc_video_session(VulkanRuntime* runtime, HEVCVideoSession* session);
    VkImageUsageFlags hevc_surface_image_usage();
    bool              reset_hevc_session(VulkanRuntime* runtime, HEVCVideoSession* session, VkVideoSessionParametersKHR parameters, char* reason, size_t reason_size);
    bool              hevc_picture_is_invalid(const VAPictureHEVC& picture);
    bool              hevc_picture_is_current_reference(const VAPictureHEVC& picture);
    int               hevc_dpb_slot_for_surface(const HEVCVideoSession* session, VASurfaceID surface_id);
    void              hevc_set_dpb_slot_for_surface(HEVCVideoSession* session, VASurfaceID surface_id, int slot);
    int               allocate_hevc_dpb_slot(HEVCVideoSession* session, const bool used_slots[max_hevc_dpb_slots]);
    void              fill_hevc_reference_info(const VAPictureHEVC& picture, StdVideoDecodeH265ReferenceInfo* info);
    HEVCRpsCounts     fill_hevc_picture_rps(const HEVCDpbReference* references, uint32_t reference_count, StdVideoDecodeH265PictureInfo* picture);
    void              build_hevc_std_parameters(HEVCVideoSession* session, VAProfile profile, const VkvvHEVCDecodeInput* input, HEVCStdParameters* std_params);
    bool create_hevc_session_parameters(VulkanRuntime* runtime, HEVCVideoSession* session, const HEVCStdParameters* std_params, VkVideoSessionParametersKHR* parameters,
                                        char* reason, size_t reason_size);
    bool create_empty_hevc_session_parameters(VulkanRuntime* runtime, HEVCVideoSession* session, VkVideoSessionParametersKHR* parameters, char* reason, size_t reason_size);

} // namespace vkvv

#endif
