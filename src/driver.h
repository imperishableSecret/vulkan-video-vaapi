#ifndef VKVV_DRIVER_H
#define VKVV_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mutex>
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

typedef struct VkvvDriver VkvvDriver;
typedef struct VkvvContext VkvvContext;
typedef struct VkvvSurface VkvvSurface;
typedef struct VkvvBuffer VkvvBuffer;
typedef struct VkvvCodecOps VkvvCodecOps;

typedef struct {
    unsigned int min_width;
    unsigned int min_height;
    unsigned int max_width;
    unsigned int max_height;
    unsigned int max_dpb_slots;
    unsigned int max_active_references;
} VkvvVideoProfileLimits;

typedef struct {
    bool h264;
    bool h265;
    bool h265_10;
    bool vp9;
    bool av1;
    bool surface_export;
    VkvvVideoProfileLimits h264_limits;
    VkvvVideoProfileLimits h265_limits;
    VkvvVideoProfileLimits h265_10_limits;
    VkvvVideoProfileLimits vp9_limits;
    VkvvVideoProfileLimits av1_limits;
    char summary[512];
} VkvvVideoCaps;

typedef struct VkvvConfig {
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int rt_format;
    unsigned int fourcc;
    unsigned int bit_depth;
    unsigned int min_width;
    unsigned int min_height;
    unsigned int max_width;
    unsigned int max_height;
    bool exportable;
} VkvvConfig;

typedef struct {
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int rt_format;
    unsigned int fourcc;
    unsigned int bit_depth;
    unsigned int min_width;
    unsigned int min_height;
    unsigned int max_width;
    unsigned int max_height;
    bool exportable;
    bool hardware_supported;
    bool decode_wired;
    bool export_wired;
    bool advertise;
} VkvvProfileCapability;

typedef enum {
    VKVV_SURFACE_WORK_READY = 0,
    VKVV_SURFACE_WORK_RENDERING,
} VkvvSurfaceWorkState;

typedef struct VkvvSurface {
    std::mutex mutex;
    unsigned int rt_format;
    unsigned int width;
    unsigned int height;
    unsigned int fourcc;
    void *vulkan;
    int dpb_slot;
    VkvvSurfaceWorkState work_state;
    VAStatus sync_status;
    bool destroying;
    bool decoded;
} VkvvSurface;

typedef struct VkvvBuffer {
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    void *data;
    bool mapped;
} VkvvBuffer;

typedef struct VkvvCodecOps {
    const char *name;
    void *(*state_create)(void);
    void (*state_destroy)(void *state);
    void *(*session_create)(void);
    void (*session_destroy)(void *runtime, void *session);
    void (*begin_picture)(void *state);
    VAStatus (*render_buffer)(void *state, const VkvvBuffer *buffer);
    VAStatus (*prepare_decode)(void *state, unsigned int *width, unsigned int *height, char *reason, size_t reason_size);
    VAStatus (*ensure_session)(void *runtime, void *session, unsigned int width, unsigned int height, char *reason, size_t reason_size);
    VAStatus (*decode)(
        void *runtime,
        void *session,
        VkvvDriver *drv,
        VkvvContext *vctx,
        VkvvSurface *target,
        VAProfile profile,
        void *state,
        char *reason,
        size_t reason_size);
} VkvvCodecOps;

typedef struct VkvvContext {
    std::mutex mutex;
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int width;
    unsigned int height;
    VASurfaceID render_target;
    const VkvvCodecOps *codec_ops;
    void *codec_state;
    void *codec_session;
    unsigned int next_dpb_slot;
} VkvvContext;

typedef struct VkvvObject {
    unsigned int id;
    VkvvObjectType type;
    void *payload;
    struct VkvvObject *next;
} VkvvObject;

typedef struct VkvvDriver {
    std::mutex object_mutex;
    std::mutex state_mutex;
    VkvvVideoCaps caps;
    VkvvProfileCapability profile_caps[VKVV_MAX_PROFILES];
    unsigned int profile_cap_count;
    VkvvObject *objects;
    unsigned int next_id;
    void *vulkan;
} VkvvDriver;

bool vkvv_probe_vulkan_video(VkvvVideoCaps *caps);

unsigned int vkvv_object_add(VkvvDriver *drv, VkvvObjectType type, void *payload);
void *vkvv_object_get(VkvvDriver *drv, unsigned int id, VkvvObjectType type);
bool vkvv_object_remove(VkvvDriver *drv, unsigned int id, VkvvObjectType type);
void vkvv_object_clear(VkvvDriver *drv);
VkvvSurface *vkvv_surface_get_locked(VkvvDriver *drv, unsigned int id);
void vkvv_surface_unlock(VkvvSurface *surface);

#ifdef __cplusplus
}
#endif

#endif
