#ifndef MUSICBOT_STORE_H
#define MUSICBOT_STORE_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

boole musicbot_store__prepare(char filename[64]);
void musicbot_store__commit(void);
boole musicbot_store__remove_file(const char *filename);
boole musicbot_store__restore(const crypto_key_t *keys, uint64 count);
boole musicbot_store__action(cJSON *message);
void musicbot_store__write_status(cJSON *message);
void musicbot_store__load_reference(cJSON *settings);

#endif
