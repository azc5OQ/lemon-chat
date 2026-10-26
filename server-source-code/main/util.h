#ifndef UTIL_H
#define UTIL_H

#include "definitions.h"
#include "../third-party/dave-g-json/cJSON.h"

boole util__get_process_ram_usage(uint64* bytes);
boole util__find_executable(cstring command, char* path, uint64 capacity);
boole util__start_detached_thread(void* (*entry)(void*), void* argument);
boole util__normalize_country_code(cJSON* json_entry, char* out_code);
uint64 util__get_timestamp_ms(void);
boole util__get_monotonic_ms(uint64* milliseconds);
void util__sleep_for_milliseconds(uint64 milliseconds);

void util__get_executable_directory(char* out_directory, uint64 out_directory_size);

boole util__fill_secure_random_bytes(unsigned char* out_buffer, uint64 length);
boole util__fill_block_of_data_with_ascii_characters(char* block, uint64 length);
void util__free_json_message(cJSON* json_root_object1, char* json_root_object1_string);
void util__hash_password_to_base64(char* plaintext, char* out, int64 out_size);
boole util__password_matches(char* plaintext, char* stored_base64_hash);

#endif
