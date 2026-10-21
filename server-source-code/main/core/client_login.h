#ifndef CLIENT_LOGIN_H
#define CLIENT_LOGIN_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"


uint64 client_login__get_public_key_bit_length(char* public_key_modulus);

char* client_login__encrypt_string_with_public_key(char* public_key_modulus, unsigned char* bytes, uint64 buffer_length);

#endif
