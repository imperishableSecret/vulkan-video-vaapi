#ifndef VKVV_CODECS_OPS_H
#define VKVV_CODECS_OPS_H

#include "../driver.h"

#ifdef __cplusplus
extern "C" {
#endif

const VkvvDecodeOps *vkvv_decode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint);
const VkvvEncodeOps *vkvv_encode_ops_for_profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint);

#ifdef __cplusplus
}
#endif

#endif
