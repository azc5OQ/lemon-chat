#ifndef LEMON_DESKTOP_SERVER_INFO_H
#define LEMON_DESKTOP_SERVER_INFO_H

#include "clib.h"

#define DESKTOP_SERVER_FEATURES 24

typedef struct desktop_server_info_t
{
    boole ready;
    uint64 revision;
    int64 uptime;
    int64 used_slots;
    int64 max_slots;
    uint64 features_known;
    uint64 features_enabled;
} desktop_server_info_t;

typedef struct desktop_server_feature_t
{
    const char* key;
    const char* label;
} desktop_server_feature_t;

const desktop_server_feature_t* desktop_server_info__feature(int64 index);

#endif
