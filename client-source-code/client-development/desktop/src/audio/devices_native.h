#ifndef LEMON_DESKTOP_DEVICES_NATIVE_H
#define LEMON_DESKTOP_DEVICES_NATIVE_H

#include "audio/devices.h"
#include "miniaudio.h"

typedef struct desktop_devices_t desktop_devices_t;

typedef struct desktop_device_selection_t
{
    ma_backend backend;
    ma_device_id id;
} desktop_device_selection_t;

boole desktop_device_key__make(ma_backend backend, const ma_device_id* id, char* key, uint64 capacity);
desktop_devices_t* desktop_devices__create(boole native);
void desktop_devices__destroy(desktop_devices_t* devices);
boole desktop_devices__view(desktop_devices_t* devices, desktop_devices_view_t* view, boole refresh);
boole desktop_devices__resolve(desktop_devices_t* devices, const char* key, boole capture, desktop_device_selection_t* selection);

#endif
