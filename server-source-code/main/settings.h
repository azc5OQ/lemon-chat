#ifndef SETTINGS_H
#define SETTINGS_H 1

void settings__load(void);
void settings__read_attachment_receive_policy(cJSON* message);
void settings__read_wireguard_fields(cJSON* json_root);
void settings__init_channel_list(void);
void settings__init_tags_and_icons(void);
void settings__load_persisted_state(void);

#endif
