#include "va_private.h"

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

bool add_profile_capability(
        VkvvDriver *drv,
        VAProfile profile,
        VAEntrypoint entrypoint,
        unsigned int rt_format,
        VkvvVideoProfileLimits limits,
        bool exportable) {
    if (drv->profile_cap_count >= VKVV_MAX_PROFILES) {
        return false;
    }

    limits = normalized_limits(limits);
    VkvvProfileCapability *cap = &drv->profile_caps[drv->profile_cap_count++];
    *cap = {};
    cap->profile = profile;
    cap->entrypoint = entrypoint;
    cap->rt_format = rt_format;
    cap->fourcc = vkvv_surface_fourcc_for_format(rt_format);
    cap->bit_depth = vkvv_rt_format_bit_depth(rt_format);
    cap->min_width = limits.min_width;
    cap->min_height = limits.min_height;
    cap->max_width = limits.max_width;
    cap->max_height = limits.max_height;
    cap->exportable = exportable;
    return true;
}

void fill_image_format(VAImageFormat *format, unsigned int fourcc, unsigned int bit_depth) {
    *format = {};
    format->fourcc = fourcc;
    format->byte_order = VA_LSB_FIRST;
    format->bits_per_pixel = bit_depth == 10 ? 24 : 12;
}

bool image_format_seen(const VAImageFormat *formats, unsigned int count, unsigned int fourcc) {
    for (unsigned int i = 0; i < count; i++) {
        if (formats[i].fourcc == fourcc) {
            return true;
        }
    }
    return false;
}

} // namespace

void vkvv_init_profile_capabilities(VkvvDriver *drv) {
    if (drv == NULL) {
        return;
    }

    drv->profile_cap_count = 0;
    if (drv->caps.h264 && drv->caps.surface_export) {
        add_profile_capability(
            drv, VAProfileH264ConstrainedBaseline, VAEntrypointVLD,
            VA_RT_FORMAT_YUV420, drv->caps.h264_limits, true);
        add_profile_capability(
            drv, VAProfileH264Main, VAEntrypointVLD,
            VA_RT_FORMAT_YUV420, drv->caps.h264_limits, true);
        add_profile_capability(
            drv, VAProfileH264High, VAEntrypointVLD,
            VA_RT_FORMAT_YUV420, drv->caps.h264_limits, true);
    }
}

const VkvvProfileCapability *vkvv_profile_capability(const VkvvDriver *drv, VAProfile profile) {
    if (drv == NULL) {
        return NULL;
    }
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        if (drv->profile_caps[i].profile == profile) {
            return &drv->profile_caps[i];
        }
    }
    return NULL;
}

bool vkvv_profile_supported(const VkvvDriver *drv, VAProfile profile) {
    return vkvv_profile_capability(drv, profile) != NULL;
}

unsigned int vkvv_surface_fourcc_for_format(unsigned int rt_format) {
    if (rt_format & VA_RT_FORMAT_YUV420_10) {
        return VA_FOURCC_P010;
    }
    return VA_FOURCC_NV12;
}

unsigned int vkvv_rt_format_bit_depth(unsigned int rt_format) {
    if (rt_format & VA_RT_FORMAT_YUV420_10) {
        return 10;
    }
    return 8;
}

unsigned int vkvv_select_rt_format(const VkvvProfileCapability *cap, unsigned int requested) {
    if (cap == NULL) {
        return 0;
    }

    const unsigned int selected = cap->rt_format & requested;
    if (selected & VA_RT_FORMAT_YUV420) {
        return VA_RT_FORMAT_YUV420;
    }
    if (selected & VA_RT_FORMAT_YUV420_10) {
        return VA_RT_FORMAT_YUV420_10;
    }
    return 0;
}

unsigned int vkvv_select_driver_rt_format(const VkvvDriver *drv, unsigned int requested) {
    if (drv == NULL) {
        return 0;
    }

    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        const unsigned int selected = vkvv_select_rt_format(&drv->profile_caps[i], requested);
        if (selected != 0) {
            return selected;
        }
    }
    return 0;
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
            attrib->value = VA_DEC_SLICE_MODE_NORMAL;
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

unsigned int vkvv_query_image_formats(
        const VkvvDriver *drv,
        VAImageFormat *format_list,
        unsigned int max_formats) {
    if (drv == NULL || format_list == NULL || max_formats == 0) {
        return 0;
    }

    unsigned int count = 0;
    for (unsigned int i = 0; i < drv->profile_cap_count && count < max_formats; i++) {
        const VkvvProfileCapability *cap = &drv->profile_caps[i];
        if (!image_format_seen(format_list, count, cap->fourcc)) {
            fill_image_format(&format_list[count++], cap->fourcc, cap->bit_depth);
        }
    }
    return count;
}
