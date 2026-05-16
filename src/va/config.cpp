#include "va/private.h"
#include "telemetry.h"

#include <new>

namespace {

    bool profile_capability_advertised(const VkvvProfileCapability* cap) {
        return vkvv_profile_capability_stage(cap) == VKVV_PROFILE_CAPABILITY_STAGE_ADVERTISED;
    }

    bool profile_seen(const VAProfile* profile_list, unsigned int count, VAProfile profile) {
        for (unsigned int i = 0; i < count; i++) {
            if (profile_list[i] == profile) {
                return true;
            }
        }
        return false;
    }

    void trace_config_attribs(uint64_t driver_instance_id, const char* event, VAProfile profile, VAEntrypoint entrypoint, const VAConfigAttrib* attrib_list, int num_attribs) {
        if (!vkvv_trace_deep_enabled() || attrib_list == NULL) {
            return;
        }
        for (int i = 0; i < num_attribs; i++) {
            VKVV_TRACE_DEEP(event, "driver=%llu profile=%d entrypoint=%d index=%d type=%u value=0x%x", static_cast<unsigned long long>(driver_instance_id), profile, entrypoint, i,
                            static_cast<unsigned int>(attrib_list[i].type), attrib_list[i].value);
        }
    }

} // namespace

VAStatus vkvvQueryConfigProfiles(VADriverContextP ctx, VAProfile* profile_list, int* num_profiles) {
    VkvvDriver*  drv   = vkvv_driver_from_ctx(ctx);
    unsigned int count = 0;
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        if (!profile_capability_advertised(&drv->profile_caps[i]) || profile_seen(profile_list, count, drv->profile_caps[i].profile)) {
            continue;
        }
        profile_list[count++] = drv->profile_caps[i].profile;
    }

    *num_profiles = static_cast<int>(count);
    VKVV_TRACE_DEEP("config-query-profiles", "driver=%llu count=%u", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), count);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoint_list, int* num_entrypoints) {
    VkvvDriver* drv = vkvv_driver_from_ctx(ctx);
    if (!vkvv_profile_supported(drv, profile)) {
        VKVV_TRACE_DEEP("config-query-entrypoints", "driver=%llu profile=%d status=%d count=0", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), profile,
                        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    unsigned int count = 0;
    for (unsigned int i = 0; i < drv->profile_cap_count; i++) {
        const VkvvProfileCapability* cap = &drv->profile_caps[i];
        if (!profile_capability_advertised(cap) || cap->profile != profile) {
            continue;
        }
        entrypoint_list[count++] = cap->entrypoint;
    }
    *num_entrypoints = static_cast<int>(count);
    VKVV_TRACE_DEEP("config-query-entrypoints", "driver=%llu profile=%d status=%d count=%u", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), profile,
                    VA_STATUS_SUCCESS, count);
    return VA_STATUS_SUCCESS;
}

VAStatus vkvvGetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs) {
    VkvvDriver*                  drv = vkvv_driver_from_ctx(ctx);
    VAStatus                     status;
    const VkvvProfileCapability* cap = vkvv_profile_capability_for_config(drv, profile, entrypoint, &status);
    if (cap == NULL) {
        VKVV_TRACE_DEEP("config-get-attributes", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d",
                        static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), profile, entrypoint, status, num_attribs);
        return status;
    }

    for (int i = 0; i < num_attribs; i++) {
        vkvv_fill_config_attribute(cap, &attrib_list[i]);
    }
    VKVV_TRACE_DEEP("config-get-attributes", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d stage=%s rt_mask=0x%x min=%ux%u max=%ux%u fourcc=0x%x exportable=%u",
                    static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), profile, entrypoint, VA_STATUS_SUCCESS, num_attribs,
                    vkvv_profile_capability_stage_name(vkvv_profile_capability_stage(cap)), vkvv_config_rt_format_mask(cap), cap->min_width, cap->min_height, cap->max_width,
                    cap->max_height, cap->fourcc, cap->export_wired ? 1U : 0U);
    trace_config_attribs(drv != NULL ? drv->driver_instance_id : 0, "config-get-attribute-value", profile, entrypoint, attrib_list, num_attribs);

    return VA_STATUS_SUCCESS;
}

VAStatus vkvvCreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attrib_list, int num_attribs, VAConfigID* config_id) {
    VkvvDriver*                  drv = vkvv_driver_from_ctx(ctx);
    VAStatus                     status;
    const VkvvProfileCapability* cap = vkvv_profile_capability_for_config(drv, profile, entrypoint, &status);
    if (cap == NULL) {
        VKVV_TRACE_DEEP("config-create", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0),
                        profile, entrypoint, status, num_attribs);
        return status;
    }

    unsigned int rt_format           = vkvv_select_rt_format(cap, cap->rt_format);
    unsigned int requested_rt_format = cap->rt_format;
    trace_config_attribs(drv != NULL ? drv->driver_instance_id : 0, "config-create-attribute-request", profile, entrypoint, attrib_list, num_attribs);
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat && attrib_list[i].value != VA_ATTRIB_NOT_SUPPORTED) {
            requested_rt_format = attrib_list[i].value;
            rt_format           = vkvv_select_rt_format(cap, attrib_list[i].value);
            if (rt_format == 0) {
                VKVV_TRACE_DEEP("config-create", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d requested_rt=0x%x supported_rt=0x%x",
                                static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), profile, entrypoint, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, num_attribs,
                                requested_rt_format, vkvv_config_rt_format_mask(cap));
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
        }
    }
    const VkvvFormatVariant* format = vkvv_profile_format_variant(cap, rt_format, true);
    if (format == NULL) {
        VKVV_TRACE_DEEP("config-create", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d requested_rt=0x%x selected_rt=0x%x supported_rt=0x%x",
                        static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), profile, entrypoint, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, num_attribs,
                        requested_rt_format, rt_format, vkvv_config_rt_format_mask(cap));
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    auto* config = new (std::nothrow) VkvvConfig();
    if (config == NULL) {
        VKVV_TRACE_DEEP("config-create", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0),
                        profile, entrypoint, VA_STATUS_ERROR_ALLOCATION_FAILED, num_attribs);
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
        VKVV_TRACE_DEEP("config-create", "driver=%llu profile=%d entrypoint=%d status=%d attribs=%d", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0),
                        profile, entrypoint, VA_STATUS_ERROR_ALLOCATION_FAILED, num_attribs);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    VKVV_TRACE_DEEP(
        "config-create",
        "driver=%llu config=%u profile=%d entrypoint=%d status=%d attribs=%d requested_rt=0x%x selected_rt=0x%x supported_rt=0x%x fourcc=0x%x bit_depth=%u min=%ux%u max=%ux%u "
        "exportable=%u",
        static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), *config_id, profile, entrypoint, VA_STATUS_SUCCESS, num_attribs, requested_rt_format, rt_format,
        vkvv_config_rt_format_mask(cap), config->fourcc, config->bit_depth, config->min_width, config->min_height, config->max_width, config->max_height,
        config->exportable ? 1U : 0U);
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
        VKVV_TRACE_DEEP("config-query-attributes", "driver=%llu config=%u status=%d", static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), config_id,
                        VA_STATUS_ERROR_INVALID_CONFIG);
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
    VKVV_TRACE_DEEP("config-query-attributes", "driver=%llu config=%u profile=%d entrypoint=%d status=%d attribs=%d rt_mask=0x%x fourcc=0x%x min=%ux%u max=%ux%u exportable=%u",
                    static_cast<unsigned long long>(drv != NULL ? drv->driver_instance_id : 0), config_id, config->profile, config->entrypoint, VA_STATUS_SUCCESS, *num_attribs,
                    cap != NULL ? vkvv_config_rt_format_mask(cap) : 0, config->fourcc, config->min_width, config->min_height, config->max_width, config->max_height,
                    config->exportable ? 1U : 0U);
    trace_config_attribs(drv != NULL ? drv->driver_instance_id : 0, "config-query-attribute-value", config->profile, config->entrypoint, attrib_list,
                         attrib_list != NULL ? *num_attribs : 0);
    return VA_STATUS_SUCCESS;
}
