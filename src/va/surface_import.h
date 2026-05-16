#ifndef VKVV_SURFACE_IMPORT_H
#define VKVV_SURFACE_IMPORT_H

#include "va/driver.h"

#include <va/va_drmcommon.h>

uint32_t                  vkvv_surface_import_memory_type(const VASurfaceAttrib* attrib_list, unsigned int num_attribs);
bool                      vkvv_surface_import_has_external_descriptor(const VASurfaceAttrib* attrib_list, unsigned int num_attribs);
VkvvFdIdentity            vkvv_fd_identity_from_fd(int fd);
VkvvExternalSurfaceImport vkvv_surface_import_from_attribs(const VASurfaceAttrib* attrib_list, unsigned int num_attribs, unsigned int index);

#endif
