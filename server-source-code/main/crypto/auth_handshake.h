#ifndef AUTH_HANDSHAKE_H
#define AUTH_HANDSHAKE_H
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"

int64 auth_handshake__prepare(client_t *client, cJSON *message);
boole auth_handshake__finish(const lemon_session_policy *policy, const ubyte secret[32], const ubyte transcript[32], crypto_session_keys_t *session, cJSON *reply);

#endif
