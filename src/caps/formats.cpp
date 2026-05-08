#include "caps/caps.h"

namespace {

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
        if (!cap->advertise) {
            continue;
        }
        if (!image_format_seen(format_list, count, cap->fourcc)) {
            fill_image_format(&format_list[count++], cap->fourcc, cap->bit_depth);
        }
    }
    return count;
}
