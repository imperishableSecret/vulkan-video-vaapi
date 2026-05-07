#include "driver.h"

#include <cstdlib>

unsigned int vkvv_object_add(VkvvDriver *drv, VkvvObjectType type, void *payload) {
    auto *object = static_cast<VkvvObject *>(std::calloc(1, sizeof(VkvvObject)));
    if (object == NULL) {
        return VA_INVALID_ID;
    }

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
    for (VkvvObject *object = drv->objects; object != NULL; object = object->next) {
        if (object->id == id && object->type == type) {
            return object->payload;
        }
    }
    return NULL;
}

bool vkvv_object_remove(VkvvDriver *drv, unsigned int id, VkvvObjectType type) {
    VkvvObject **cursor = &drv->objects;
    while (*cursor != NULL) {
        VkvvObject *object = *cursor;
        if (object->id == id && object->type == type) {
            *cursor = object->next;
            std::free(object->payload);
            std::free(object);
            return true;
        }
        cursor = &object->next;
    }
    return false;
}

void vkvv_object_clear(VkvvDriver *drv) {
    VkvvObject *object = drv->objects;
    while (object != NULL) {
        VkvvObject *next = object->next;
        std::free(object->payload);
        std::free(object);
        object = next;
    }
    drv->objects = NULL;
}
