#include "va_private.h"

#include <cstdlib>

namespace {

VkvvObject *find_object_locked(VkvvDriver *drv, unsigned int id, VkvvObjectType type) {
    for (VkvvObject *object = drv->objects; object != NULL; object = object->next) {
        if (object->id == id && object->type == type) {
            return object;
        }
    }
    return NULL;
}

void destroy_payload(VkvvObject *object) {
    if (object == NULL) {
        return;
    }
    switch (object->type) {
        case VKVV_OBJECT_CONTEXT: {
            auto *vctx = static_cast<VkvvContext *>(object->payload);
            pthread_mutex_lock(&vctx->mutex);
            pthread_mutex_unlock(&vctx->mutex);
            vkvv_context_destroy_lock(vctx);
            break;
        }
        case VKVV_OBJECT_SURFACE: {
            auto *surface = static_cast<VkvvSurface *>(object->payload);
            pthread_mutex_lock(&surface->mutex);
            pthread_mutex_unlock(&surface->mutex);
            vkvv_surface_destroy_lock(surface);
            break;
        }
        default:
            break;
    }
    std::free(object->payload);
    object->payload = NULL;
}

} // namespace

void vkvv_driver_init_lock(VkvvDriver *drv) {
    if (drv != NULL) {
        pthread_mutex_init(&drv->object_mutex, NULL);
        pthread_mutex_init(&drv->state_mutex, NULL);
    }
}

void vkvv_driver_destroy_lock(VkvvDriver *drv) {
    if (drv != NULL) {
        pthread_mutex_destroy(&drv->state_mutex);
        pthread_mutex_destroy(&drv->object_mutex);
    }
}

void vkvv_context_init_lock(VkvvContext *vctx) {
    if (vctx != NULL) {
        pthread_mutex_init(&vctx->mutex, NULL);
    }
}

void vkvv_context_destroy_lock(VkvvContext *vctx) {
    if (vctx != NULL) {
        pthread_mutex_destroy(&vctx->mutex);
    }
}

void vkvv_surface_init_lock(VkvvSurface *surface) {
    if (surface != NULL) {
        pthread_mutex_init(&surface->mutex, NULL);
    }
}

void vkvv_surface_destroy_lock(VkvvSurface *surface) {
    if (surface != NULL) {
        pthread_mutex_destroy(&surface->mutex);
    }
}

unsigned int vkvv_object_add(VkvvDriver *drv, VkvvObjectType type, void *payload) {
    auto *object = static_cast<VkvvObject *>(std::calloc(1, sizeof(VkvvObject)));
    if (object == NULL) {
        return VA_INVALID_ID;
    }

    VkvvLockGuard lock(&drv->object_mutex);
    object->id = drv->next_id++;
    if (object->id == VA_INVALID_ID) {
        object->id = drv->next_id++;
    }
    object->type = type;
    object->payload = payload;
    object->next = drv->objects;
    drv->objects = object;
    return object->id;
}

void *vkvv_object_get(VkvvDriver *drv, unsigned int id, VkvvObjectType type) {
    VkvvLockGuard lock(&drv->object_mutex);
    VkvvObject *object = find_object_locked(drv, id, type);
    return object != NULL ? object->payload : NULL;
}

bool vkvv_object_remove(VkvvDriver *drv, unsigned int id, VkvvObjectType type) {
    VkvvObject *removed = NULL;
    {
        VkvvLockGuard lock(&drv->object_mutex);
        VkvvObject **cursor = &drv->objects;
        while (*cursor != NULL) {
            VkvvObject *object = *cursor;
            if (object->id == id && object->type == type) {
                *cursor = object->next;
                removed = object;
                break;
            }
            cursor = &object->next;
        }
    }
    if (removed == NULL) {
        return false;
    }
    destroy_payload(removed);
    std::free(removed);
    return true;
}

void vkvv_object_clear(VkvvDriver *drv) {
    VkvvObject *object = NULL;
    {
        VkvvLockGuard lock(&drv->object_mutex);
        object = drv->objects;
        drv->objects = NULL;
    }
    while (object != NULL) {
        VkvvObject *next = object->next;
        destroy_payload(object);
        std::free(object);
        object = next;
    }
}

VkvvSurface *vkvv_surface_get_locked(VkvvDriver *drv, unsigned int id) {
    VkvvLockGuard object_lock(&drv->object_mutex);
    VkvvObject *object = find_object_locked(drv, id, VKVV_OBJECT_SURFACE);
    if (object == NULL) {
        return NULL;
    }
    auto *surface = static_cast<VkvvSurface *>(object->payload);
    pthread_mutex_lock(&surface->mutex);
    return surface;
}

void vkvv_surface_unlock(VkvvSurface *surface) {
    if (surface != NULL) {
        pthread_mutex_unlock(&surface->mutex);
    }
}
