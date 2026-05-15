#include "va/private.h"
#include "va/surface_import.h"

#include <mutex>
#include <new>

namespace {

    VkvvObject* find_object_locked(VkvvDriver* drv, unsigned int id, VkvvObjectType type) {
        for (VkvvObject* object = drv->objects; object != NULL; object = object->next) {
            if (object->id == id && object->type == type) {
                return object;
            }
        }
        return NULL;
    }

    void destroy_payload(VkvvObject* object) {
        if (object == NULL) {
            return;
        }
        switch (object->type) {
            case VKVV_OBJECT_CONTEXT: {
                auto*                        vctx = static_cast<VkvvContext*>(object->payload);
                std::unique_lock<std::mutex> lock(vctx->mutex);
                lock.unlock();
                delete vctx;
                break;
            }
            case VKVV_OBJECT_SURFACE: {
                auto*                        surface = static_cast<VkvvSurface*>(object->payload);
                std::unique_lock<std::mutex> lock(surface->mutex);
                lock.unlock();
                vkvv_surface_import_close(&surface->import);
                delete surface;
                break;
            }
            case VKVV_OBJECT_CONFIG: delete static_cast<VkvvConfig*>(object->payload); break;
            case VKVV_OBJECT_BUFFER: delete static_cast<VkvvBuffer*>(object->payload); break;
            default: break;
        }
        object->payload = NULL;
    }

} // namespace

unsigned int vkvv_object_add(VkvvDriver* drv, VkvvObjectType type, void* payload) {
    auto* object = new (std::nothrow) VkvvObject();
    if (object == NULL) {
        return VA_INVALID_ID;
    }

    VkvvLockGuard lock(&drv->object_mutex);
    object->id = drv->next_id++;
    if (object->id == VA_INVALID_ID) {
        object->id = drv->next_id++;
    }
    object->type    = type;
    object->payload = payload;
    object->next    = drv->objects;
    drv->objects    = object;
    return object->id;
}

void* vkvv_object_get(VkvvDriver* drv, unsigned int id, VkvvObjectType type) {
    VkvvLockGuard lock(&drv->object_mutex);
    VkvvObject*   object = find_object_locked(drv, id, type);
    return object != NULL ? object->payload : NULL;
}

bool vkvv_object_remove(VkvvDriver* drv, unsigned int id, VkvvObjectType type) {
    VkvvObject* removed = NULL;
    {
        VkvvLockGuard lock(&drv->object_mutex);
        VkvvObject**  cursor = &drv->objects;
        while (*cursor != NULL) {
            VkvvObject* object = *cursor;
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
    delete removed;
    return true;
}

void vkvv_object_clear(VkvvDriver* drv) {
    VkvvObject* object = NULL;
    {
        VkvvLockGuard lock(&drv->object_mutex);
        object       = drv->objects;
        drv->objects = NULL;
    }
    while (object != NULL) {
        VkvvObject* next = object->next;
        destroy_payload(object);
        delete object;
        object = next;
    }
}

VkvvSurface* vkvv_surface_get_locked(VkvvDriver* drv, unsigned int id) {
    VkvvLockGuard object_lock(&drv->object_mutex);
    VkvvObject*   object = find_object_locked(drv, id, VKVV_OBJECT_SURFACE);
    if (object == NULL) {
        return NULL;
    }
    auto* surface = static_cast<VkvvSurface*>(object->payload);
    surface->mutex.lock();
    return surface;
}

void vkvv_surface_unlock(VkvvSurface* surface) {
    if (surface != NULL) {
        surface->mutex.unlock();
    }
}
