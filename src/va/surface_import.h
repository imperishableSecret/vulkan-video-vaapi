#ifndef VKVV_SURFACE_IMPORT_H
#define VKVV_SURFACE_IMPORT_H

#include "va/driver.h"

#include <va/va_drmcommon.h>

uint32_t                  vkvv_surface_import_memory_type(const VASurfaceAttrib* attrib_list, unsigned int num_attribs);
bool                      vkvv_surface_import_has_external_descriptor(const VASurfaceAttrib* attrib_list, unsigned int num_attribs);
VkvvFdIdentity            vkvv_fd_identity_from_fd(int fd);
bool                      vkvv_fd_identity_equal(VkvvFdIdentity lhs, VkvvFdIdentity rhs);
void                      vkvv_surface_import_close(VkvvExternalSurfaceImport* import);
VkvvExternalImageIdentity vkvv_external_image_identity_from_import(const VkvvExternalSurfaceImport& import);
VkvvExternalSurfaceImport vkvv_surface_import_from_attribs(const VASurfaceAttrib* attrib_list, unsigned int num_attribs, unsigned int index);

#endif
