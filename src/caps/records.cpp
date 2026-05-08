#include "va_private.h"

#include <vulkan/vulkan.h>

namespace {

constexpr unsigned int fallback_min_surface_dimension = 1;
constexpr unsigned int fallback_max_decode_surface_dimension = 4096;
constexpr unsigned int config_attribute_count = 4;

const VAConfigAttribType config_attribute_types[config_attribute_count] = {
    VAConfigAttribRTFormat,
    VAConfigAttribDecSliceMode,
    VAConfigAttribMaxPictureWidth,
    VAConfigAttribMaxPictureHeight,
};

VkvvVideoProfileLimits normalized_limits(VkvvVideoProfileLimits limits) {
    if (limits.min_width == 0) {
        limits.min_width = fallback_min_surface_dimension;
    }
    if (limits.min_height == 0) {
        limits.min_height = fallback_min_surface_dimension;
    }
    if (limits.max_width == 0) {
        limits.max_width = fallback_max_decode_surface_dimension;
    }
    if (limits.max_height == 0) {
        limits.max_height = fallback_max_decode_surface_dimension;
    }
    return limits;
}

bool add_profile_record(
        VkvvDriver *drv,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VkvvCodecDirection direction,
        unsigned int vulkan_codec_operation,
        const char *required_extension,
        unsigned int rt_format,
        VkvvVideoProfileLimits limits,
        bool hardware_supported,
        bool parser_wired,
        bool runtime_wired,
        bool surface_wired) {
    if (drv->profile_cap_count >= VKVV_MAX_PROFILES) {
        return false;
    }

    limits = normalized_limits(limits);
    VkvvProfileCapability *cap = &drv->profile_caps[drv->profile_cap_count++];
    *cap = {};
    cap->profile = profile;
    cap->entrypoint = entrypoint;
    cap->direction = direction;
    cap->vulkan_codec_operation = vulkan_codec_operation;
    cap->required_extension = required_extension;
    cap->rt_format = rt_format;
    cap->fourcc = vkvv_surface_fourcc_for_format(rt_format);
    cap->bit_depth = vkvv_rt_format_bit_depth(rt_format);
    cap->min_width = limits.min_width;
    cap->min_height = limits.min_height;
    cap->max_width = limits.max_width;
    cap->max_height = limits.max_height;
    cap->hardware_supported = hardware_supported;
    cap->parser_wired = parser_wired;
    cap->runtime_wired = runtime_wired;
    cap->surface_wired = surface_wired;
    cap->decode_wired = direction == VKVV_CODEC_DIRECTION_DECODE && parser_wired && runtime_wired;
    cap->export_wired = direction == VKVV_CODEC_DIRECTION_DECODE && surface_wired;
    cap->advertise = hardware_supported && parser_wired && runtime_wired && surface_wired;
    cap->exportable = cap->advertise && cap->export_wired;
    return true;
}

} // namespace

void vkvv_init_profile_capabilities(VkvvDriver *drv) {
    if (drv == NULL) {
        return;
    }

    drv->profile_cap_count = 0;
    add_profile_record(
        drv, VAProfileH264ConstrainedBaseline, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h264_limits,
        drv->caps.h264, true, true, drv->caps.surface_export);
    add_profile_record(
        drv, VAProfileH264Main, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h264_limits,
        drv->caps.h264, true, true, drv->caps.surface_export);
    add_profile_record(
        drv, VAProfileH264High, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h264_limits,
        drv->caps.h264, true, true, drv->caps.surface_export);
    add_profile_record(
        drv, VAProfileHEVCMain, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h265_limits,
        drv->caps.h265, false, false, drv->caps.surface_export);
    add_profile_record(
        drv, VAProfileHEVCMain10, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420_10, drv->caps.h265_10_limits,
        drv->caps.h265_10, false, false, false);
    add_profile_record(
        drv, VAProfileVP9Profile0, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR,
        VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.vp9_limits,
        drv->caps.vp9, false, false, drv->caps.surface_export);
    add_profile_record(
        drv, VAProfileAV1Profile0, VAEntrypointVLD,
        VKVV_CODEC_DIRECTION_DECODE,
        VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
        VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.av1_limits,
        drv->caps.av1, false, false, drv->caps.surface_export);
    add_profile_record(
        drv, VAProfileH264High, VAEntrypointEncSlice,
        VKVV_CODEC_DIRECTION_ENCODE,
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h264_limits,
        drv->caps.h264, false, false, false);
    add_profile_record(
        drv, VAProfileH264High, VAEntrypointEncSliceLP,
        VKVV_CODEC_DIRECTION_ENCODE,
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h264_limits,
        drv->caps.h264, false, false, false);
    add_profile_record(
        drv, VAProfileH264High, VAEntrypointEncPicture,
        VKVV_CODEC_DIRECTION_ENCODE,
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VA_RT_FORMAT_YUV420, drv->caps.h264_limits,
        drv->caps.h264, false, false, false);
}

const VkvvProfileCapability *vkvv_profile_capability(const VkvvDriver *drv, VAProfile profile) {
    if (drv == NULL) {
        return NULL;
    }
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        if (drv->profile_caps[i].advertise && drv->profile_caps[i].profile == profile) {
            return &drv->profile_caps[i];
        }
    }
    return NULL;
}

const VkvvProfileCapability *vkvv_profile_capability_for_entrypoint(
        const VkvvDriver *drv,
        VAProfile profile,
        VAEntrypoint entrypoint) {
    if (drv == NULL) {
        return NULL;
    }
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        const VkvvProfileCapability *cap = &drv->profile_caps[i];
        if (cap->advertise && cap->profile == profile && cap->entrypoint == entrypoint) {
            return cap;
        }
    }
    return NULL;
}

const VkvvProfileCapability *vkvv_profile_capability_record(
        const VkvvDriver *drv,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VkvvCodecDirection direction) {
    if (drv == NULL) {
        return NULL;
    }
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        const VkvvProfileCapability *cap = &drv->profile_caps[i];
        if (cap->profile == profile &&
            cap->entrypoint == entrypoint &&
            cap->direction == direction) {
            return cap;
        }
    }
    return NULL;
}

unsigned int vkvv_config_attribute_count(void) {
    return config_attribute_count;
}

void vkvv_fill_config_attribute(const VkvvProfileCapability *cap, VAConfigAttrib *attrib) {
    if (attrib == NULL) {
        return;
    }
    if (cap == NULL) {
        attrib->value = VA_ATTRIB_NOT_SUPPORTED;
        return;
    }

    switch (attrib->type) {
        case VAConfigAttribRTFormat:
            attrib->value = cap->rt_format;
            break;
        case VAConfigAttribDecSliceMode:
            attrib->value = cap->direction == VKVV_CODEC_DIRECTION_DECODE ?
                            VA_DEC_SLICE_MODE_NORMAL :
                            VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib->value = cap->max_width;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib->value = cap->max_height;
            break;
        default:
            attrib->value = VA_ATTRIB_NOT_SUPPORTED;
            break;
    }
}

bool vkvv_fill_config_attribute_by_index(
        const VkvvProfileCapability *cap,
        unsigned int index,
        VAConfigAttrib *attrib) {
    if (index >= config_attribute_count || attrib == NULL) {
        return false;
    }

    attrib->type = config_attribute_types[index];
    vkvv_fill_config_attribute(cap, attrib);
    return true;
}
