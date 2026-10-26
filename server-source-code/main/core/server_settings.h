#ifndef SERVER_SETTINGS_H
#define SERVER_SETTINGS_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

void settings__read_attachment_receive_policy(cJSON* message);
void settings__init_channel_list(void);
void settings__load_persisted_state(void);
void settings__init_groups_and_icons(void);
boole settings__ensure_default_groups(void);
void settings__read_wireguard_fields(cJSON* json_root);
void settings__load(void);
boole settings__write_file_atomically(char* path, char* contents);
boole settings__save_server_settings_to_file(void);
void settings__save_initial(char plaintext_keys[][256], uint64 keys_count);

#endif
