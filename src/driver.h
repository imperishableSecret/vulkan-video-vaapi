#ifndef VKVV_DRIVER_H
#define VKVV_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKVV_MAX_PROFILES 8
#define VKVV_MAX_ENTRYPOINTS 1
#define VKVV_MAX_ATTRIBUTES 4
#define VKVV_MAX_IMAGE_FORMATS 2
#define VKVV_MAX_SUBPIC_FORMATS 1
#define VKVV_MAX_DISPLAY_ATTRIBUTES 1

typedef enum {
    VKVV_OBJECT_CONFIG = 1,
    VKVV_OBJECT_CONTEXT,
    VKVV_OBJECT_SURFACE,
    VKVV_OBJECT_BUFFER,
} VkvvObjectType;

typedef struct {
    bool h264;
    bool h265;
    bool h265_10;
    bool vp9;
    bool av1;
    bool surface_export;
    char summary[512];
} VkvvVideoCaps;

typedef struct VkvvConfig {
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int rt_format;
} VkvvConfig;

typedef struct VkvvSurface {
    unsigned int rt_format;
    unsigned int width;
    unsigned int height;
    unsigned int fourcc;
    void *vulkan;
    int dpb_slot;
    bool decoded;
} VkvvSurface;

typedef struct VkvvContext {
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int width;
    unsigned int height;
    VASurfaceID render_target;
    void *codec_state;
} VkvvContext;

typedef struct VkvvBuffer {
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    void *data;
    bool mapped;
} VkvvBuffer;

typedef struct VkvvObject {
    unsigned int id;
    VkvvObjectType type;
    void *payload;
    struct VkvvObject *next;
} VkvvObject;

typedef struct {
    VkvvVideoCaps caps;
    VkvvObject *objects;
    unsigned int next_id;
    unsigned int next_dpb_slot;
    void *vulkan;
} VkvvDriver;

bool vkvv_probe_vulkan_video(VkvvVideoCaps *caps);

unsigned int vkvv_object_add(VkvvDriver *drv, VkvvObjectType type, void *payload);
void *vkvv_object_get(VkvvDriver *drv, unsigned int id, VkvvObjectType type);
bool vkvv_object_remove(VkvvDriver *drv, unsigned int id, VkvvObjectType type);
void vkvv_object_clear(VkvvDriver *drv);

#ifdef __cplusplus
}
#endif

#endif
