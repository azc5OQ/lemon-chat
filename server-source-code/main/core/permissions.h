#pragma once

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

extern uint64 g_guest_group_id;

void permissions__refresh_client_powers(client_t* client);
void permissions__refresh_all_client_powers(void);
boole permissions__can_use_channel(const client_t* client, const channel_t* channel, permission_power_t power);

void permissions__set_preset(group_type_t type, group_permissions_t* permissions);
boole permissions__parse_group_type(cstring name, group_type_t* type);
cstring permissions__group_type_name(group_type_t type);
boole permissions__refresh_guest_membership(client_t* client);
boole permissions__can_create_group(const client_t* sender, const group_permissions_t* proposed);

boole permissions__is_allowed(const client_t* client, permission_t permission);
boole permissions__can_moderate(const client_t* sender, const client_t* target, permission_t permission);
boole permissions__read(cJSON* object, group_permissions_t* permissions);
cJSON* permissions__write(const group_permissions_t* permissions);

boole permissions__is_allowed_in_groups(const client_t* client, permission_t permission);
boole permissions__can_manage_group(const client_t* sender, uint64 group_id, permission_t permission);
boole permissions__can_change_member(const client_t* sender, const client_t* target, uint64 group_id, boole add);
boole permissions__can_edit_group_permissions(const client_t* sender, uint64 group_id, const group_permissions_t* proposed);
boole permissions__can_delete_group(const client_t* sender, uint64 group_id);
cJSON* permissions__write_access(const client_t* client);
cJSON* permissions__write_group_access(const client_t* client, uint64 group_id);

boole permissions__is_stored_client_allowed(const client_stored_data_t* stored, permission_t permission);
boole permissions__is_identity_allowed(const char* identity_hash, permission_t permission);
