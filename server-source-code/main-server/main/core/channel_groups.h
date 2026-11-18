#pragma once

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

enum
{
    CHANNEL_GROUP_MEMBER,
    CHANNEL_GROUP_ADMIN,
    CHANNEL_GROUP_LIMIT = 128
};

typedef struct channel_group_t
{
    boole is_existing;
    int64 icon_id;
    char name[GROUP_MAX_NAME_LENGTH];
    group_permissions_t permissions;
} channel_group_t;

extern channel_group_t g_channel_groups[CHANNEL_GROUP_LIMIT];
void channel_groups__init(void);
boole channel_groups__supports(permission_t permission);
boole channel_groups__read_permissions(cJSON *object, group_permissions_t *permissions);
cJSON *channel_groups__write_permissions(const group_permissions_t *permissions);
uint64 channel_groups__power(uint64 group_id, permission_power_t power);
cstring channel_groups__power_name(permission_power_t power);
boole channel_groups__can_assign(const client_t *actor, const client_t *target, const channel_t *channel, uint64 group_id);
boole channel_groups__can_edit_definition(const client_t *actor, permission_t permission, const channel_group_t *previous, const channel_group_t *proposed);
boole channel_groups__load_catalog(cJSON *catalog);
cJSON *channel_groups__write_catalog(boole include_permissions);

uint64 channel_groups__role(const client_t *client, const channel_t *channel);
boole channel_groups__allows(uint64 role, permission_t permission);
boole channel_groups__is_allowed(const client_t *client, const channel_t *channel, permission_t permission);
boole channel_groups__can_moderate(const client_t *actor, const client_t *target, const channel_t *channel, permission_t permission);
boole channel_groups__set(channel_t *channel, cstring identity, uint64 role);
void channel_groups__refresh(client_t *client);
void channel_groups__refresh_locked(client_t *client);
boole channel_groups__mute(channel_t *channel, cstring identity, uint64 until_ms);
void channel_groups__clear(channel_t *channel);
void channel_groups__read(channel_t *channel, cJSON *members);
cJSON *channel_groups__write(const channel_t *channel);

uint64 channel_groups__assigned_role(const client_t *client, const channel_t *channel);
