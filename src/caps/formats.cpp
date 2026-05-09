#include "caps/caps.h"

namespace {

    void fill_image_format(VAImageFormat* format, unsigned int fourcc, unsigned int bit_depth) {
        *format                = {};
        format->fourcc         = fourcc;
        format->byte_order     = VA_LSB_FIRST;
        format->bits_per_pixel = bit_depth > 8 ? 24 : 12;
    }

    bool image_format_seen(const VAImageFormat* formats, unsigned int count, unsigned int fourcc) {
        for (unsigned int i = 0; i < count; i++) {
            if (formats[i].fourcc == fourcc) {
                return true;
            }
        }
        return false;
    }

    bool profile_accepts_yuv420_config_alias(const VkvvProfileCapability* cap) {
        // Config negotiation may use YUV420 as the baseline 4:2:0 bit; surface selection still resolves Main10 to P010.
        return cap != NULL && cap->profile == VAProfileHEVCMain10 && cap->entrypoint == VAEntrypointVLD && cap->direction == VKVV_CODEC_DIRECTION_DECODE &&
            vkvv_profile_format_variant(cap, VA_RT_FORMAT_YUV420_10, true) != NULL && vkvv_profile_format_variant(cap, VA_RT_FORMAT_YUV420, true) == NULL;
    }

} // namespace

bool vkvv_profile_supported(const VkvvDriver* drv, VAProfile profile) {
    return vkvv_profile_capability(drv, profile) != NULL;
}

const VkvvFormatVariant* vkvv_profile_format_variant(const VkvvProfileCapability* cap, unsigned int rt_format, bool require_advertised) {
    if (cap == NULL) {
        return NULL;
    }
    for (unsigned int i = 0; i < cap->format_count; i++) {
        const VkvvFormatVariant* format = &cap->formats[i];
        if ((format->rt_format & rt_format) == 0) {
            continue;
        }
        if (require_advertised && !format->advertise) {
            continue;
        }
        return format;
    }
    return NULL;
}

unsigned int vkvv_surface_fourcc_for_format(unsigned int rt_format) {
    if (rt_format & VA_RT_FORMAT_YUV420_12) {
        return VA_FOURCC_P012;
    }
    if (rt_format & VA_RT_FORMAT_YUV420_10) {
        return VA_FOURCC_P010;
    }
    return VA_FOURCC_NV12;
}

unsigned int vkvv_rt_format_bit_depth(unsigned int rt_format) {
    if (rt_format & VA_RT_FORMAT_YUV420_12) {
        return 12;
    }
    if (rt_format & VA_RT_FORMAT_YUV420_10) {
        return 10;
    }
    return 8;
}

unsigned int vkvv_select_rt_format(const VkvvProfileCapability* cap, unsigned int requested) {
    if (cap == NULL) {
        return 0;
    }

    if ((requested & VA_RT_FORMAT_YUV420) != 0 && profile_accepts_yuv420_config_alias(cap)) {
        return VA_RT_FORMAT_YUV420_10;
    }
    if ((requested & VA_RT_FORMAT_YUV420) != 0 && vkvv_profile_format_variant(cap, VA_RT_FORMAT_YUV420, true) != NULL) {
        return VA_RT_FORMAT_YUV420;
    }
    if ((requested & VA_RT_FORMAT_YUV420_10) != 0 && vkvv_profile_format_variant(cap, VA_RT_FORMAT_YUV420_10, true) != NULL) {
        return VA_RT_FORMAT_YUV420_10;
    }
    if ((requested & VA_RT_FORMAT_YUV420_12) != 0 && vkvv_profile_format_variant(cap, VA_RT_FORMAT_YUV420_12, true) != NULL) {
        return VA_RT_FORMAT_YUV420_12;
    }
    return 0;
}

unsigned int vkvv_config_rt_format_mask(const VkvvProfileCapability* cap) {
    if (cap == NULL) {
        return 0;
    }

    unsigned int rt_format = cap->rt_format;
    if (profile_accepts_yuv420_config_alias(cap)) {
        rt_format |= VA_RT_FORMAT_YUV420;
    }
    return rt_format;
}

unsigned int vkvv_select_driver_rt_format(const VkvvDriver* drv, unsigned int requested) {
    if (drv == NULL) {
        return 0;
    }

    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        if (!drv->profile_caps[i].advertise) {
            continue;
        }
        const unsigned int selected = vkvv_select_rt_format(&drv->profile_caps[i], requested);
        if (selected != 0) {
            return selected;
        }
    }
    return 0;
}

unsigned int vkvv_query_image_formats(const VkvvDriver* drv, VAImageFormat* format_list, unsigned int max_formats) {
    if (drv == NULL || format_list == NULL || max_formats == 0) {
        return 0;
    }

    unsigned int count = 0;
    for (unsigned int i = 0; i < drv->profile_cap_count && count < max_formats; i++) {
        const VkvvProfileCapability* cap = &drv->profile_caps[i];
        if (cap->direction != VKVV_CODEC_DIRECTION_DECODE) {
            continue;
        }
        for (unsigned int j = 0; j < cap->format_count && count < max_formats; j++) {
            const VkvvFormatVariant* format = &cap->formats[j];
            if (!format->hardware_supported || !format->export_wired) {
                continue;
            }
            if (!image_format_seen(format_list, count, format->fourcc)) {
                fill_image_format(&format_list[count++], format->fourcc, format->bit_depth);
            }
        }
    }
    return count;
}
