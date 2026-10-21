#ifndef ADMINISTRATION_H
#define ADMINISTRATION_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

boole server_admin__is_ip_banned(char* ip_address);
boole server_admin__is_identity_banned(char* public_key);
boole server_admin__is_country_blocked(char* country_iso_code);
boole server_admin__add_ban(char* ip_address, char* country_iso_code, char* identity, char* extra_data);
boole server_admin__remove_ban_by_ip(char* ip_address);
boole server_admin__is_tag_id_real(uint64 tag_id);
boole server_admin__is_client_already_assigned_this_tag_id(uint64 client_id, uint64 this_tag_id);
int64 server_admin__get_index_of_tag_id_of_client(uint64 client_id, uint64 this_tag_id);
boole server_admin__is_server_banner_valid(cstring banner);
boole server_admin__set_icon_base64(icon_t* icon, cstring base64);
void server_admin__free_icon_base64(icon_t* icon);

boole server_admin__allow_admin_password_attempt(char* ip_address, uint64 now);

#endif
