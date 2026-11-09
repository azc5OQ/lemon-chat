#ifndef LEMON_DESKTOP_DEVICES_H
#define LEMON_DESKTOP_DEVICES_H

#include "clib.h"

#define DESKTOP_DEVICE_KEY 544
#define DESKTOP_DEVICE_LIMIT 64

typedef struct desktop_device_t
{
    char key[DESKTOP_DEVICE_KEY];
    char name[256];
    boole capture;
    boole default_device;
} desktop_device_t;

typedef struct desktop_devices_view_t
{
    uint64 revision;
    boole scanning;
    boole ready;
    boole truncated;
    int64 count;
    char detail[192];
    desktop_device_t items[DESKTOP_DEVICE_LIMIT];
} desktop_devices_view_t;

#endif
