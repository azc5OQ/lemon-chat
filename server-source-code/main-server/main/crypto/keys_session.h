#ifndef LEMON_KEYS_SESSION_H
#define LEMON_KEYS_SESSION_H

#include "../definitions.h"

#include "ciphers.h"

boole keys_session__derive(const ubyte secret[32], const ubyte transcript[32], int64 type, uint64 count, int64 random_type, lemon_session_keys *output);
boole keys_session__crypt(const lemon_session_keys *session, boole server_to_client, boole encrypt, const ubyte iv[16], ubyte *data, uint64 size);
boole keys_session__random_probe(const ubyte secret[32], const ubyte transcript[32], int64 type, const ubyte iv[16], ubyte data[32]);
boole keys_session__random_probe_tag(const ubyte secret[32], const ubyte transcript[32], const ubyte plaintext[32], ubyte tag[32]);
boole keys_session__begin_transcript(ubyte transcript[32], const lemon_session_policy *policy, const char *nonce, const char *public_key);

boole keys_session__hkdf(const ubyte input[32], const ubyte salt[32], const char *purpose, ubyte output[32]);
boole keys_session__mix(ubyte state[32], const ubyte *data, uint64 size);

#endif
