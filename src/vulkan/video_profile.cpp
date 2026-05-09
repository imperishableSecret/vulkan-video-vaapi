#include "vulkan/video_profile.h"

namespace vkvv {

    VideoCapabilitiesChain::VideoCapabilitiesChain(const VideoProfileSpec& spec) {
        h264.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
        h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR;
        vp9.sType  = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR;
        av1.sType  = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR;

        void* codec_caps = nullptr;
        switch (spec.operation) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: codec_caps = &h264; break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: codec_caps = &h265; break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR: codec_caps = &vp9; break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: codec_caps = &av1; break;
            default: break;
        }

        decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
        decode.pNext = codec_caps;
        video.sType  = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
        video.pNext  = &decode;
    }

    VideoProfileChain::VideoProfileChain(const VideoProfileSpec& spec) {
        h264.sType         = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
        h264.stdProfileIdc = spec.std_profile != UINT32_MAX ? static_cast<StdVideoH264ProfileIdc>(spec.std_profile) : STD_VIDEO_H264_PROFILE_IDC_HIGH;
        h264.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

        h265.sType         = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
        h265.stdProfileIdc = spec.std_profile != UINT32_MAX           ? static_cast<StdVideoH265ProfileIdc>(spec.std_profile) :
            spec.bit_depth == VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR ? STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS :
            spec.bit_depth == VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR ? STD_VIDEO_H265_PROFILE_IDC_MAIN_10 :
                                                                        STD_VIDEO_H265_PROFILE_IDC_MAIN;

        vp9.sType      = VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR;
        vp9.stdProfile = spec.std_profile != UINT32_MAX              ? static_cast<StdVideoVP9Profile>(spec.std_profile) :
            spec.bit_depth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR ? STD_VIDEO_VP9_PROFILE_2 :
                                                                       STD_VIDEO_VP9_PROFILE_0;

        av1.sType            = VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR;
        av1.stdProfile       = spec.std_profile != UINT32_MAX ? static_cast<StdVideoAV1Profile>(spec.std_profile) : STD_VIDEO_AV1_PROFILE_MAIN;
        av1.filmGrainSupport = VK_FALSE;

        h264_encode.sType         = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
        h264_encode.stdProfileIdc = spec.std_profile != UINT32_MAX ? static_cast<StdVideoH264ProfileIdc>(spec.std_profile) : STD_VIDEO_H264_PROFILE_IDC_HIGH;

        void* codec_profile = nullptr;
        bool  encode        = false;
        switch (spec.operation) {
            case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: codec_profile = &h264; break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR: codec_profile = &h265; break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR: codec_profile = &vp9; break;
            case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR: codec_profile = &av1; break;
            case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
                codec_profile = &h264_encode;
                encode        = true;
                break;
            default: break;
        }

        decode_usage.sType           = VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR;
        decode_usage.pNext           = codec_profile;
        decode_usage.videoUsageHints = VK_VIDEO_DECODE_USAGE_STREAMING_BIT_KHR;

        encode_usage.sType             = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR;
        encode_usage.pNext             = codec_profile;
        encode_usage.videoUsageHints   = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR;
        encode_usage.videoContentHints = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR;
        encode_usage.tuningMode        = VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR;

        profile.sType               = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
        profile.pNext               = encode ? static_cast<void*>(&encode_usage) : static_cast<void*>(&decode_usage);
        profile.videoCodecOperation = spec.operation;
        profile.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
        profile.lumaBitDepth        = spec.bit_depth;
        profile.chromaBitDepth      = spec.bit_depth;
    }

} // namespace vkvv
