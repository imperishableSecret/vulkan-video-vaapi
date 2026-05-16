#include "va/private.h"

#include <new>

namespace {

    bool profile_seen(const VAProfile* profile_list, unsigned int count, VAProfile profile) {
        for (unsigned int i = 0; i < count; i++) {
            if (profile_list[i] == profile) {
                return true;
            }
        }
        return false;
    }

} // namespace

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile* profile_list, int* num_profiles) {
    VkvvDriver*  drv   = vkvv_driver_from_ctx(ctx);
    unsigned int count = 0;
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        if (!drv->profile_caps[i].advertise || profile_seen(profile_list, count, drv->profile_caps[i].profile)) {
            continue;
        }
        profile_list[count++] = drv->profile_caps[i].profile;
    }

    *num_profiles = static_cast<int>(count);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoint_list, int* num_entrypoints) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (!vkvv_profile_supported(drv, profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    unsigned int count = 0;
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        const VkvvProfileCapability* cap = &drv->profile_caps[i];
        if (!cap->advertise || cap->profile != profile) {
            continue;
        }
        entrypoint_list[count++] = cap->entrypoint;
    }
    *num_entrypoints = static_cast<int>(count);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvGetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (!vkvv_profile_supported(drv, profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    const VkvvProfileCapability* cap = vkvv_profile_capability_for_entrypoint(drv, profile, entrypoint);
    if (cap == NULL) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    for (int i = 0; i < num_attribs; i++) {
        vkvv_fill_config_attribute(cap, &attrib_list[i]);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus vkvvCreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs, VAConfigID* config_id) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (!vkvv_profile_supported(drv, profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    const VkvvProfileCapability* cap = vkvv_profile_capability_for_entrypoint(drv, profile, entrypoint);
    if (cap == NULL) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    unsigned int rt_format = vkvv_select_rt_format(cap, cap->rt_format);
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat && attrib_list[i].value != VA_ATTRIB_NOT_SUPPORTED) {
            rt_format = vkvv_select_rt_format(cap, attrib_list[i].value);
            if (rt_format == 0) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
        }
    }
    const VkvvFormatVariant* format = vkvv_profile_format_variant(cap, rt_format, true);
    if (format == NULL) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    auto* config = new (std::nothrow) VkvvConfig();
    if (config == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    config->profile    = profile;
    config->entrypoint = entrypoint;
    config->direction  = cap->direction;
    config->mode       = cap->direction == VKVV_CODEC_DIRECTION_ENCODE ? VKVV_CONTEXT_MODE_ENCODE : VKVV_CONTEXT_MODE_DECODE;
    config->rt_format  = rt_format;
    config->fourcc     = format->fourcc;
    config->bit_depth  = format->bit_depth;
    config->min_width  = cap->min_width;
    config->min_height = cap->min_height;
    config->max_width  = cap->max_width;
    config->max_height = cap->max_height;
    config->exportable = format->exportable;

    *config_id = vkvv_object_add(drv, VKVV_OBJECT_CONFIG, config);
    if (*config_id == VA_INVALID_ID) {
        delete config;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyConfig(VADriverContextP ctx, VAConfigID config_id) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    return vkvv_object_remove(drv, config_id, VKVV_OBJECT_CONFIG) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus vkvvQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id, VAProfile* profile, VAEntrypoint* entrypoint, VAConfigAttrib* attrib_list, int* num_attribs) {
    VkvvDriver* drv    = vkvv_driver_from_ctx(ctx);
    auto*       config = static_cast<VkvvConfig*>(vkvv_object_get(drv, config_id, VKVV_OBJECT_CONFIG));
    if (config == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    *profile                         = config->profile;
    *entrypoint                      = config->entrypoint;
    const VkvvProfileCapability* cap = vkvv_profile_capability_for_entrypoint(drv, config->profile, config->entrypoint);
    if (attrib_list != NULL) {
        for (unsigned int i = 0; i < vkvv_config_attribute_count(); i++) {
            vkvv_fill_config_attribute_by_index(cap, i, &attrib_list[i]);
        }
    }
    *num_attribs = static_cast<int>(vkvv_config_attribute_count());
    return VA_STATUS_SUCCESS;
}
