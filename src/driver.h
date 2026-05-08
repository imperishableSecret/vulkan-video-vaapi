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

#define VKVV_MAX_PROFILES 24
#define VKVV_MAX_ENTRYPOINTS 4
#define VKVV_MAX_ATTRIBUTES 5
#define VKVV_MAX_IMAGE_FORMATS 3
#define VKVV_MAX_FORMAT_VARIANTS 3
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
typedef struct VkvvDecodeOps VkvvDecodeOps;
typedef struct VkvvEncodeOps VkvvEncodeOps;

typedef enum {
    VKVV_CODEC_DIRECTION_DECODE = 0,
    VKVV_CODEC_DIRECTION_ENCODE = 1,
} VkvvCodecDirection;

typedef enum {
    VKVV_CONTEXT_MODE_DECODE = 0,
    VKVV_CONTEXT_MODE_ENCODE = 1,
} VkvvContextMode;

typedef enum {
    VKVV_BUFFER_CLASS_PARAMETER = 0,
    VKVV_BUFFER_CLASS_ENCODE_CODED_OUTPUT = 1,
} VkvvBufferClass;

typedef enum {
    VKVV_SURFACE_ROLE_DECODE_OUTPUT = 1u << 0,
    VKVV_SURFACE_ROLE_EXPORTABLE = 1u << 1,
    VKVV_SURFACE_ROLE_ENCODE_INPUT = 1u << 2,
    VKVV_SURFACE_ROLE_ENCODE_RECONSTRUCTED = 1u << 3,
    VKVV_SURFACE_ROLE_ENCODE_REFERENCE = 1u << 4,
} VkvvSurfaceRoleFlags;

typedef struct VkvvCodedBufferPayload VkvvCodedBufferPayload;

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
    bool h265_12;
    bool vp9;
    bool vp9_10;
    bool vp9_12;
    bool av1;
    bool av1_10;
    bool av1_12;
    bool surface_export;
    bool surface_export_nv12;
    bool surface_export_p010;
    bool surface_export_p012;
    VkvvVideoProfileLimits h264_limits;
    VkvvVideoProfileLimits h265_limits;
    VkvvVideoProfileLimits h265_10_limits;
    VkvvVideoProfileLimits h265_12_limits;
    VkvvVideoProfileLimits vp9_limits;
    VkvvVideoProfileLimits vp9_10_limits;
    VkvvVideoProfileLimits vp9_12_limits;
    VkvvVideoProfileLimits av1_limits;
    VkvvVideoProfileLimits av1_10_limits;
    VkvvVideoProfileLimits av1_12_limits;
    char summary[512];
} VkvvVideoCaps;

typedef struct VkvvConfig {
    VAProfile profile;
    VAEntrypoint entrypoint;
    VkvvCodecDirection direction;
    VkvvContextMode mode;
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
    unsigned int rt_format;
    unsigned int fourcc;
    unsigned int bit_depth;
    unsigned int vulkan_format;
    bool hardware_supported;
    bool surface_wired;
    bool export_wired;
    bool advertise;
    bool exportable;
} VkvvFormatVariant;

typedef struct {
    VAProfile profile;
    VAEntrypoint entrypoint;
    VkvvCodecDirection direction;
    unsigned int vulkan_codec_operation;
    const char *required_extension;
    unsigned int rt_format;
    unsigned int fourcc;
    unsigned int bit_depth;
    unsigned int min_width;
    unsigned int min_height;
    unsigned int max_width;
    unsigned int max_height;
    bool exportable;
    bool hardware_supported;
    bool parser_wired;
    bool runtime_wired;
    bool surface_wired;
    bool decode_wired;
    bool export_wired;
    bool advertise;
    unsigned int format_count;
    VkvvFormatVariant formats[VKVV_MAX_FORMAT_VARIANTS];
} VkvvProfileCapability;

typedef enum {
    VKVV_SURFACE_WORK_READY = 0,
    VKVV_SURFACE_WORK_RENDERING,
} VkvvSurfaceWorkState;

typedef struct VkvvSurface {
    std::mutex mutex;
    VASurfaceID id = VA_INVALID_ID;
    uint64_t driver_instance_id = 0;
    uint64_t stream_id = 0;
    unsigned int codec_operation = 0;
    unsigned int rt_format;
    unsigned int width;
    unsigned int height;
    unsigned int fourcc;
    unsigned int role_flags;
    void *vulkan;
    VkvvSurfaceWorkState work_state;
    VAStatus sync_status;
    bool destroying;
    bool decoded;
} VkvvSurface;

typedef struct VkvvBuffer {
    VABufferType type;
    VkvvBufferClass buffer_class;
    unsigned int size;
    unsigned int num_elements;
    void *data;
    VkvvCodedBufferPayload *coded_payload;
    bool mapped;
} VkvvBuffer;

typedef struct VkvvDecodeOps {
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
} VkvvDecodeOps;

typedef struct VkvvEncodeOps {
    const char *name;
} VkvvEncodeOps;

typedef struct VkvvContext {
    std::mutex mutex;
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VkvvContextMode mode;
    uint64_t stream_id;
    unsigned int codec_operation;
    unsigned int width;
    unsigned int height;
    VASurfaceID render_target;
    const VkvvDecodeOps *decode_ops;
    void *decode_state;
    void *decode_session;
    const VkvvEncodeOps *encode_ops;
    void *encode_state;
    void *encode_session;
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
    uint64_t driver_instance_id = 0;
    uint64_t next_stream_id = 1;
    uint64_t active_decode_stream_id = 0;
    unsigned int active_decode_codec_operation = 0;
    unsigned int active_decode_width = 0;
    unsigned int active_decode_height = 0;
    unsigned int active_decode_rt_format = 0;
    unsigned int active_decode_fourcc = 0;
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
