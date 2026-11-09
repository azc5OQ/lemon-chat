#ifndef LEMON_DESKTOP_CRYPTO_SESSION_H
#define LEMON_DESKTOP_CRYPTO_SESSION_H

#include "clib.h"
#include "crypto/crypto_media.h"

#define DESKTOP_DH_TEXT_CAPACITY 4112
#define DESKTOP_SERVER_KEY_LIMIT 8

typedef struct desktop_crypto_t desktop_crypto_t;
struct cJSON;

typedef struct desktop_content_keys_t
{
    ubyte keys[4][32];
    ubyte ivs[4][16];
} desktop_content_keys_t;

desktop_crypto_t *crypto_session__create(void);
desktop_crypto_t *crypto_session__copy_identity(const desktop_crypto_t *source);
void crypto_session__destroy(desktop_crypto_t *crypto);
boole crypto_session__new_identity(char *identity, uint64 identity_capacity);
boole crypto_session__identity(desktop_crypto_t *crypto, const char *identity, int64 bits, char *public_key, uint64 public_key_capacity);
boole crypto_session__server_keys(desktop_crypto_t *crypto, const char *keys);
boole crypto_session__begin_dh(desktop_crypto_t *crypto, char *public_mix, uint64 public_mix_capacity);
struct cJSON *crypto_session__negotiate(desktop_crypto_t *crypto, const struct cJSON *policy, const char *public_key);
boole crypto_session__finish_negotiation(desktop_crypto_t *crypto, const struct cJSON *message);
boole crypto_session__finish_dh(desktop_crypto_t *crypto, const char *public_mix);
boole crypto_session__challenge(desktop_crypto_t *crypto, const char *encoded, char *plaintext, uint64 plaintext_capacity);
char *crypto_session__encrypt(desktop_crypto_t *crypto, const char *plaintext);
char *crypto_session__decrypt(desktop_crypto_t *crypto, const char *envelope, uint64 length);
void crypto_session__free_text(char *text);
void crypto_session__free_json(struct cJSON *json);
boole crypto_session__media_keys(desktop_crypto_t *crypto, desktop_media_keys_t *keys);
boole crypto_session__random(desktop_crypto_t *crypto, ubyte *bytes, uint64 size);
boole crypto_session__content_keys(desktop_crypto_t *crypto, desktop_content_keys_t *keys);
char *crypto_session__keys_json(const desktop_content_keys_t *keys);
boole crypto_session__read_keys(const char *json, desktop_content_keys_t *keys);
char *crypto_session__content_encrypt(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text);
char *crypto_session__content_decrypt(const desktop_content_keys_t *keys, const char *text);
char *crypto_session__attachment_encrypt(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, uint64 padding);
char *crypto_session__picture_encrypt(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, uint64 padding);
char *crypto_session__attachment_decrypt(const desktop_content_keys_t *keys, const char *text);
char *crypto_session__file_decrypt(const desktop_content_keys_t *keys, const char *text);
char *crypto_session__channel_seal(const desktop_content_keys_t *keys, const char *content);
char *crypto_session__channel_open(const desktop_content_keys_t *keys, const char *envelope);
char *crypto_session__direct_content_encrypt(const desktop_content_keys_t *keys, const char *text, uint64 padding, boole authenticated);
char *crypto_session__direct_content_decrypt(const desktop_content_keys_t *keys, const char *content, boole authenticated);
char *crypto_session__direct_seal(desktop_crypto_t *crypto, const char *public_key, const desktop_content_keys_t *keys, const char *content);
char *crypto_session__direct_open(desktop_crypto_t *crypto, const struct cJSON *envelope, desktop_content_keys_t *keys);
char *crypto_session__hybrid_encrypt(desktop_crypto_t *crypto, const char *public_key, const char *text);
char *crypto_session__hybrid_decrypt(desktop_crypto_t *crypto, const char *text);

#endif
