#ifndef VKVV_VULKAN_VP9_INTERNAL_H
#define VKVV_VULKAN_VP9_INTERNAL_H

#include "vulkan/runtime_internal.h"
#include "codecs/vp9/vp9.h"

#include <array>

namespace vkvv {

    inline constexpr uint32_t         max_vp9_reference_slots   = VKVV_VP9_REFERENCE_COUNT;
    inline constexpr uint32_t         max_vp9_active_references = VKVV_VP9_ACTIVE_REFERENCE_COUNT;
    inline constexpr uint32_t         max_vp9_dpb_slots         = VKVV_VP9_REFERENCE_COUNT;

    inline constexpr VideoProfileSpec vp9_profile0_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .std_profile = STD_VIDEO_VP9_PROFILE_0,
    };

    inline constexpr VideoProfileSpec vp9_profile2_10bit_spec{
        .operation   = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
        .bit_depth   = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
        .std_profile = STD_VIDEO_VP9_PROFILE_2,
    };

    struct VP9ReferenceSlot {
        VASurfaceID surface_id = VA_INVALID_ID;
        int         slot       = -1;
    };

    struct VP9VideoSession {
        VAProfile                                    va_profile        = VAProfileVP9Profile0;
        unsigned int                                 va_rt_format      = VA_RT_FORMAT_YUV420;
        unsigned int                                 va_fourcc         = VA_FOURCC_NV12;
        uint8_t                                      bitstream_profile = 0;
        uint8_t                                      bit_depth         = 8;
        VideoProfileSpec                             profile_spec      = vp9_profile0_spec;
        VideoSession                                 video;
        std::array<UploadBuffer, command_slot_count> uploads;
        VP9ReferenceSlot                             reference_slots[max_vp9_reference_slots]{};
        VkDeviceSize                                 bitstream_offset_alignment    = 1;
        VkDeviceSize                                 bitstream_size_alignment      = 1;
        StdVideoVP9Level                             max_level                     = STD_VIDEO_VP9_LEVEL_6_2;
        VkVideoDecodeCapabilityFlagsKHR              decode_flags                  = 0;
        uint32_t                                     next_dpb_slot                 = 0;
        uint32_t                                     max_dpb_slots                 = 0;
        uint32_t                                     max_active_reference_pictures = 0;
    };

    void              destroy_vp9_video_session(VulkanRuntime* runtime, VP9VideoSession* session);
    VkImageUsageFlags vp9_surface_image_usage();
    bool              reset_vp9_session(VulkanRuntime* runtime, VP9VideoSession* session, char* reason, size_t reason_size);
    int               vp9_dpb_slot_for_reference_index(const VP9VideoSession* session, uint32_t reference_index);
    int               vp9_dpb_slot_for_surface(const VP9VideoSession* session, VASurfaceID surface_id);
    void              vp9_set_reference_slot(VP9VideoSession* session, uint32_t reference_index, VASurfaceID surface_id, int slot);
    int               allocate_vp9_dpb_slot(VP9VideoSession* session, const bool used_slots[max_vp9_dpb_slots]);

} // namespace vkvv

#endif
