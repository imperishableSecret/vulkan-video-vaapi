#include "va_private.h"

#include <new>

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    int count = 0;

    if (drv->caps.h264) {
        profile_list[count++] = VAProfileH264ConstrainedBaseline;
        profile_list[count++] = VAProfileH264Main;
        profile_list[count++] = VAProfileH264High;
    }

    *num_profiles = count;
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint *entrypoint_list,
        int *num_entrypoints) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    if (!vkvv_profile_supported(drv, profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    entrypoint_list[0] = VAEntrypointVLD;
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
    if (!vkvv_profile_supported(drv, profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                attrib_list[i].value = vkvv_default_rt_format_for_profile(profile);
                break;
            case VAConfigAttribDecSliceMode:
                attrib_list[i].value = VA_DEC_SLICE_MODE_NORMAL;
                break;
            default:
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
        }
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
    if (!vkvv_profile_supported(drv, profile)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    unsigned int rt_format = vkvv_default_rt_format_for_profile(profile);
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat &&
            attrib_list[i].value != VA_ATTRIB_NOT_SUPPORTED) {
            if ((attrib_list[i].value & rt_format) == 0) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
            rt_format = attrib_list[i].value & rt_format;
        }
    }

    auto *config = new (std::nothrow) VkvvConfig();
    if (config == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    config->profile = profile;
    config->entrypoint = entrypoint;
    config->rt_format = rt_format;

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
    if (attrib_list != NULL) {
        attrib_list[0].type = VAConfigAttribRTFormat;
        attrib_list[0].value = config->rt_format;
    }
    *num_attribs = 1;
    return VA_STATUS_SUCCESS;
}
