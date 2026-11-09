#ifndef EMOJIS_H
#define EMOJIS_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "server_settings.h"

// Keep room in the settings document for channels, identities, groups and icons.
#define SERVER_EMOJI_STORAGE_MAX_BYTES (SERVER_SETTINGS_MAX_FILE_BYTES / 2)

cstring emojis__add(cstring name, cstring base64, char *id);
boole emojis__remove(cstring id);
boole emojis__is_id_valid(cstring id);
cJSON *emojis__catalogue(void);
cJSON *emojis__image(cstring id);
cJSON *emojis__write(void);
void emojis__load(cJSON *array);

#endif
