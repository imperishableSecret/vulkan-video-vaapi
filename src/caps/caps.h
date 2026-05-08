#ifndef VKVV_CAPS_H
#define VKVV_CAPS_H

#include "../driver.h"

#ifdef __cplusplus
extern "C" {
#endif

void vkvv_init_profile_capabilities(VkvvDriver *drv);
const VkvvProfileCapability *vkvv_profile_capability(const VkvvDriver *drv, VAProfile profile);
const VkvvProfileCapability *vkvv_profile_capability_for_entrypoint(const VkvvDriver *drv, VAProfile profile, VAEntrypoint entrypoint);
const VkvvProfileCapability *vkvv_profile_capability_record(const VkvvDriver *drv, VAProfile profile, VAEntrypoint entrypoint, VkvvCodecDirection direction);
bool vkvv_profile_supported(const VkvvDriver *drv, VAProfile profile);
unsigned int vkvv_surface_fourcc_for_format(unsigned int rt_format);
unsigned int vkvv_rt_format_bit_depth(unsigned int rt_format);
unsigned int vkvv_select_rt_format(const VkvvProfileCapability *cap, unsigned int requested);
unsigned int vkvv_select_driver_rt_format(const VkvvDriver *drv, unsigned int requested);
unsigned int vkvv_config_attribute_count(void);
void vkvv_fill_config_attribute(const VkvvProfileCapability *cap, VAConfigAttrib *attrib);
bool vkvv_fill_config_attribute_by_index(const VkvvProfileCapability *cap, unsigned int index, VAConfigAttrib *attrib);
unsigned int vkvv_query_image_formats(const VkvvDriver *drv, VAImageFormat *format_list, unsigned int max_formats);

#ifdef __cplusplus
}
#endif

#endif
