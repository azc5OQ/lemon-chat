#ifndef AUTH_RSA_H
#define AUTH_RSA_H

#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

uint64 auth_rsa__get_public_key_bit_length(char *public_key_modulus);

char *auth_rsa__encrypt_string_with_public_key(char *public_key_modulus, ubyte *bytes, uint64 buffer_length);

#endif
