#ifndef LEMON_KEYS_SESSION_H
#define LEMON_KEYS_SESSION_H

#include "ciphers.h"

#define LEMON_MAX_DH_EXCHANGES 4
#define LEMON_MAX_SESSION_LAYERS 8
#define LEMON_MAX_CONNECTION_LAYERS (LEMON_MAX_SESSION_LAYERS + 1)

typedef struct lemon_session_policy
{
    unsigned int exchanges;
    unsigned int layers;
    int cipher;
    int random_layer;
} lemon_session_policy;

typedef struct lemon_session_layer
{
    int type;
    unsigned char bytes[32];
} lemon_session_layer;

typedef struct lemon_session_keys
{
    unsigned int count;
    lemon_session_layer client_to_server[LEMON_MAX_CONNECTION_LAYERS];
    lemon_session_layer server_to_client[LEMON_MAX_CONNECTION_LAYERS];
    unsigned char client_mac[32];
    unsigned char server_mac[32];
} lemon_session_keys;

int keys_session__derive(const unsigned char secret[32], const unsigned char transcript[32],
    int type, unsigned int count, int random_type, lemon_session_keys* output);
int keys_session__crypt(const lemon_session_keys* session, int server_to_client, int encrypt,
    const unsigned char iv[16], unsigned char* data, size_t size);
int keys_session__random_probe(const unsigned char secret[32], const unsigned char transcript[32], int type,
    const unsigned char iv[16], unsigned char data[32]);
int keys_session__random_probe_tag(const unsigned char secret[32], const unsigned char transcript[32],
    const unsigned char plaintext[32], unsigned char tag[32]);
int keys_session__begin_transcript(unsigned char transcript[32], const lemon_session_policy* policy,
    const char* nonce, const char* public_key);

int keys_session__hkdf(const unsigned char input[32], const unsigned char salt[32], const char* purpose, unsigned char output[32]);
int keys_session__mix(unsigned char state[32], const unsigned char* data, size_t size);

#endif
