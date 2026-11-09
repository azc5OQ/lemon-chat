#ifndef UTIL_H
#define UTIL_H

#include "definitions.h"
#include "../third-party/dave-g-json/cJSON.h"

void util__json_delete_secure(cJSON *root);
void util__json_delete_item_secure(cJSON *object, const char *name);
boole util__json_add_owned_object(cJSON *object, const char *name, cJSON *child);
boole util__json_add_owned_array(cJSON *array, cJSON *child);
boole util__json_read_int64(const cJSON *value, int64 minimum, int64 maximum, int64 *result);
boole util__json_read_uint64(const cJSON *value, uint64 maximum, uint64 *result);
boole util__get_process_ram_usage(uint64 *bytes);
boole util__find_executable(cstring command, char *path, uint64 capacity);
boole util__start_detached_thread(void *(*entry)(void *), void *argument);
boole util__normalize_country_code(cJSON *json_entry, char *out_code);
uint64 util__get_timestamp_ms(void);
boole util__get_monotonic_ms(uint64 *milliseconds);
void util__sleep_for_milliseconds(uint64 milliseconds);

void util__get_executable_directory(char *out_directory, uint64 out_directory_size);

boole util__fill_secure_random_bytes(ubyte *out_buffer, uint64 length);
boole util__fill_block_of_data_with_ascii_characters(char *block, uint64 length);
void util__free_json_message(cJSON *json_root_object1, char *json_root_object1_string);
void util__hash_password_to_base64(cstring plaintext, char *out, int64 out_size);
boole util__password_matches(char *plaintext, char *stored_base64_hash);

#endif
