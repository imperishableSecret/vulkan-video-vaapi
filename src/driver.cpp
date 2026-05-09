#include "va_private.h"
#include "vulkan_runtime.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <va/va_drmcommon.h>

#ifndef VKVV_VERSION
#define VKVV_VERSION "unknown"
#endif

static bool log_enabled(void) {
    const char *env = std::getenv("VKVV_LOG");
    return env != NULL && std::strcmp(env, "0") != 0;
}

namespace {

std::mutex global_runtime_mutex;
void *global_vulkan_runtime = NULL;
std::atomic<uint64_t> next_driver_instance_id{1};

} // namespace

void vkvv_log(const char *fmt, ...) {
    if (!log_enabled()) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "nvidia-vulkan-vaapi: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

void *vkvv_get_or_create_vulkan_runtime(char *reason, size_t reason_size) {
    std::lock_guard<std::mutex> lock(global_runtime_mutex);
    if (global_vulkan_runtime != NULL) {
        std::snprintf(reason, reason_size, "Vulkan Video runtime already initialized");
        return global_vulkan_runtime;
    }

    global_vulkan_runtime = vkvv_vulkan_runtime_create(reason, reason_size);
    return global_vulkan_runtime;
}

static void release_owned_payloads(VkvvDriver *drv) {
    for (VkvvObject *object = drv->objects; object != NULL; object = object->next) {
        switch (object->type) {
            case VKVV_OBJECT_CONTEXT:
                vkvv_release_context_payload(drv, static_cast<VkvvContext *>(object->payload));
                break;
            case VKVV_OBJECT_BUFFER: {
                auto *buffer = static_cast<VkvvBuffer *>(object->payload);
                if (buffer != NULL) {
                    std::free(buffer->data);
                    buffer->data = NULL;
                }
                break;
            }
            case VKVV_OBJECT_SURFACE:
                if (drv->vulkan != NULL) {
                    vkvv_vulkan_surface_destroy(drv->vulkan, static_cast<VkvvSurface *>(object->payload));
                }
                break;
            default:
                break;
        }
    }
}

static VAStatus vkvvTerminate(VADriverContextP ctx) {
    VkvvDriver *drv = vkvv_driver_from_ctx(ctx);
    if (drv != NULL) {
        release_owned_payloads(drv);
        vkvv_object_clear(drv);
        delete drv;
        ctx->pDriverData = NULL;
    }
    return VA_STATUS_SUCCESS;
}

template <typename Ret, typename... Args>
static Ret unsupported_callback_impl(Args...) {
    return static_cast<Ret>(VA_STATUS_ERROR_UNIMPLEMENTED);
}

template <typename Ret, typename... Args>
static Ret (*unsupported_callback(Ret (*)(Args...)))(Args...) {
    return unsupported_callback_impl<Ret, Args...>;
}

static struct VADriverVTable make_vtable(void) {
    struct VADriverVTable vt = {};
    vt.vaTerminate = vkvvTerminate;
    vt.vaQueryConfigProfiles = vkvvQueryConfigProfiles;
    vt.vaQueryConfigEntrypoints = vkvvQueryConfigEntrypoints;
    vt.vaGetConfigAttributes = vkvvGetConfigAttributes;
    vt.vaCreateConfig = vkvvCreateConfig;
    vt.vaDestroyConfig = vkvvDestroyConfig;
    vt.vaQueryConfigAttributes = vkvvQueryConfigAttributes;
    vt.vaCreateSurfaces = vkvvCreateSurfaces;
    vt.vaDestroySurfaces = vkvvDestroySurfaces;
    vt.vaCreateContext = vkvvCreateContext;
    vt.vaDestroyContext = vkvvDestroyContext;
    vt.vaCreateBuffer = vkvvCreateBuffer;
    vt.vaBufferSetNumElements = vkvvBufferSetNumElements;
    vt.vaMapBuffer = vkvvMapBuffer;
    vt.vaUnmapBuffer = vkvvUnmapBuffer;
    vt.vaDestroyBuffer = vkvvDestroyBuffer;
    vt.vaBeginPicture = vkvvBeginPicture;
    vt.vaRenderPicture = vkvvRenderPicture;
    vt.vaEndPicture = vkvvEndPicture;
    vt.vaSyncSurface = vkvvSyncSurface;
    vt.vaQuerySurfaceStatus = vkvvQuerySurfaceStatus;
    vt.vaQuerySurfaceError = vkvvQuerySurfaceError;
    vt.vaPutSurface = unsupported_callback(vt.vaPutSurface);
    vt.vaQueryImageFormats = vkvvQueryImageFormats;
    vt.vaCreateImage = unsupported_callback(vt.vaCreateImage);
    vt.vaDeriveImage = unsupported_callback(vt.vaDeriveImage);
    vt.vaDestroyImage = unsupported_callback(vt.vaDestroyImage);
    vt.vaSetImagePalette = unsupported_callback(vt.vaSetImagePalette);
    vt.vaGetImage = unsupported_callback(vt.vaGetImage);
    vt.vaPutImage = unsupported_callback(vt.vaPutImage);
    vt.vaQuerySubpictureFormats = unsupported_callback(vt.vaQuerySubpictureFormats);
    vt.vaCreateSubpicture = unsupported_callback(vt.vaCreateSubpicture);
    vt.vaDestroySubpicture = unsupported_callback(vt.vaDestroySubpicture);
    vt.vaSetSubpictureImage = unsupported_callback(vt.vaSetSubpictureImage);
    vt.vaSetSubpictureChromakey = unsupported_callback(vt.vaSetSubpictureChromakey);
    vt.vaSetSubpictureGlobalAlpha = unsupported_callback(vt.vaSetSubpictureGlobalAlpha);
    vt.vaAssociateSubpicture = unsupported_callback(vt.vaAssociateSubpicture);
    vt.vaDeassociateSubpicture = unsupported_callback(vt.vaDeassociateSubpicture);
    vt.vaQueryDisplayAttributes = unsupported_callback(vt.vaQueryDisplayAttributes);
    vt.vaGetDisplayAttributes = unsupported_callback(vt.vaGetDisplayAttributes);
    vt.vaSetDisplayAttributes = unsupported_callback(vt.vaSetDisplayAttributes);
    vt.vaBufferInfo = vkvvBufferInfo;
    vt.vaLockSurface = unsupported_callback(vt.vaLockSurface);
    vt.vaUnlockSurface = unsupported_callback(vt.vaUnlockSurface);
    vt.vaCreateSurfaces2 = vkvvCreateSurfaces2;
    vt.vaQuerySurfaceAttributes = vkvvQuerySurfaceAttributes;
    vt.vaAcquireBufferHandle = unsupported_callback(vt.vaAcquireBufferHandle);
    vt.vaReleaseBufferHandle = unsupported_callback(vt.vaReleaseBufferHandle);
    vt.vaCreateMFContext = unsupported_callback(vt.vaCreateMFContext);
    vt.vaMFAddContext = unsupported_callback(vt.vaMFAddContext);
    vt.vaMFReleaseContext = unsupported_callback(vt.vaMFReleaseContext);
    vt.vaMFSubmit = unsupported_callback(vt.vaMFSubmit);
    vt.vaCreateBuffer2 = unsupported_callback(vt.vaCreateBuffer2);
    vt.vaQueryProcessingRate = unsupported_callback(vt.vaQueryProcessingRate);
    vt.vaExportSurfaceHandle = vkvvExportSurfaceHandle;
    vt.vaSyncSurface2 = vkvvSyncSurface2;
    vt.vaSyncBuffer = unsupported_callback(vt.vaSyncBuffer);
    vt.vaCopy = unsupported_callback(vt.vaCopy);
    vt.vaMapBuffer2 = vkvvMapBuffer2;
    return vt;
}

static struct VADriverVTable vtable = make_vtable();

static VAStatus vkvvDriverInit(VADriverContextP ctx) {
    auto *drv = new (std::nothrow) VkvvDriver();
    if (drv == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    drv->next_id = 1;
    drv->driver_instance_id = next_driver_instance_id.fetch_add(1, std::memory_order_relaxed);
    if (drv->driver_instance_id == 0) {
        drv->driver_instance_id = next_driver_instance_id.fetch_add(1, std::memory_order_relaxed);
    }
    vkvv_probe_vulkan_video(&drv->caps);
    if (drv->caps.h264 || drv->caps.vp9 || drv->caps.vp9_10 || drv->caps.vp9_12 ||
        drv->caps.av1 || drv->caps.av1_10) {
        char reason[512] = {};
        drv->vulkan = vkvv_get_or_create_vulkan_runtime(reason, sizeof(reason));
        vkvv_log("%s", reason);
        if (drv->vulkan == NULL) {
            drv->caps.h264 = false;
            drv->caps.vp9 = false;
            drv->caps.vp9_10 = false;
            drv->caps.vp9_12 = false;
            drv->caps.av1 = false;
            drv->caps.av1_10 = false;
            drv->caps.av1_12 = false;
            drv->caps.surface_export = false;
            drv->caps.surface_export_nv12 = false;
            drv->caps.surface_export_p010 = false;
            drv->caps.surface_export_p012 = false;
        } else if (!vkvv_vulkan_supports_surface_export(drv->vulkan)) {
            drv->caps.surface_export = false;
            drv->caps.surface_export_nv12 = false;
            drv->caps.surface_export_p010 = false;
            drv->caps.surface_export_p012 = false;
        }
    }
    vkvv_init_profile_capabilities(drv);

    ctx->pDriverData = drv;
    *ctx->vtable = vtable;
    ctx->max_profiles = VKVV_MAX_PROFILES;
    ctx->max_entrypoints = VKVV_MAX_ENTRYPOINTS;
    ctx->max_attributes = VKVV_MAX_ATTRIBUTES;
    ctx->max_image_formats = VKVV_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats = VKVV_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes = VKVV_MAX_DISPLAY_ATTRIBUTES;
    ctx->str_vendor = "NVIDIA Vulkan Video VA-API prototype " VKVV_VERSION;

    vkvv_log("%s", drv->caps.summary);
    return VA_STATUS_SUCCESS;
}

extern "C" __attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    try {
        return vkvvDriverInit(ctx);
    } catch (...) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
}
