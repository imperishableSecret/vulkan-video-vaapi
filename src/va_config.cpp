#include "va_private.h"

#include <new>

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        profile_list[i] = drv->profile_caps[i].profile;
    }

    *num_profiles = static_cast<int>(drv->profile_cap_count);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint *entrypoint_list,
        int *num_entrypoints) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    const VkvvProfileCapability *cap = vkvv_profile_capability(drv, profile);
    if (cap == NULL) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    entrypoint_list[0] = cap->entrypoint;
    *num_entrypoints = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvGetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    const VkvvProfileCapability *cap = vkvv_profile_capability(drv, profile);
    if (cap == NULL) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (entrypoint != cap->entrypoint) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    for (int i = 0; i < num_attribs; i++) {
        vkvv_fill_config_attribute(cap, &attrib_list[i]);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus vkvvCreateConfig(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    const VkvvProfileCapability *cap = vkvv_profile_capability(drv, profile);
    if (cap == NULL) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (entrypoint != cap->entrypoint) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    unsigned int rt_format = cap->rt_format;
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat &&
            attrib_list[i].value != VA_ATTRIB_NOT_SUPPORTED) {
            rt_format = vkvv_select_rt_format(cap, attrib_list[i].value);
            if (rt_format == 0) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
        }
    }

    auto *config = new (std::nothrow) VkvvConfig();
    if (config == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    config->profile = profile;
    config->entrypoint = entrypoint;
    config->rt_format = rt_format;
    config->fourcc = vkvv_surface_fourcc_for_format(rt_format);
    config->bit_depth = vkvv_rt_format_bit_depth(rt_format);
    config->min_width = cap->min_width;
    config->min_height = cap->min_height;
    config->max_width = cap->max_width;
    config->max_height = cap->max_height;
    config->exportable = cap->exportable;

    *config_id = vkvv_object_add(drv, VKVV_OBJECT_CONFIG, config);
    if (*config_id == VA_INVALID_ID) {
        delete config;
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvDestroyConfig(VADriverContextP ctx, VAConfigID config_id) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    return vkvv_object_remove(drv, config_id, VKVV_OBJECT_CONFIG) ?
           VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus vkvvQueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id,
        VAProfile *profile,
        VAEntrypoint *entrypoint,
        VAConfigAttrib *attrib_list,
        int *num_attribs) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    auto *config = static_cast<VkvvConfig *>(vkvv_object_get(drv, config_id, VKVV_OBJECT_CONFIG));
    if (config == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    *profile = config->profile;
    *entrypoint = config->entrypoint;
    const VkvvProfileCapability *cap = vkvv_profile_capability(drv, config->profile);
    if (attrib_list != NULL) {
        for (unsigned int i = 0; i < vkvv_config_attribute_count(); i++) {
            vkvv_fill_config_attribute_by_index(cap, i, &attrib_list[i]);
        }
    }
    *num_attribs = static_cast<int>(vkvv_config_attribute_count());
    return VA_STATUS_SUCCESS;
}
