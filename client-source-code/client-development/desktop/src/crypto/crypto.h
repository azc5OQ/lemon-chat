#ifndef LEMON_DESKTOP_CRYPTO_H
#define LEMON_DESKTOP_CRYPTO_H

#include "clib.h"
#include "crypto/media.h"

#define DESKTOP_DH_TEXT_CAPACITY 4112
#define DESKTOP_SERVER_KEY_LIMIT 8

typedef struct desktop_crypto_t desktop_crypto_t;
struct cJSON;

typedef struct desktop_content_keys_t
{
    unsigned char keys[4][32];
    unsigned char ivs[4][16];
} desktop_content_keys_t;

desktop_crypto_t* desktop_crypto__create(void);
desktop_crypto_t* desktop_crypto__copy_identity(const desktop_crypto_t* source);
void desktop_crypto__destroy(desktop_crypto_t* crypto);
boole desktop_crypto__new_identity(char* identity, uint64 identity_capacity);
boole desktop_crypto__identity(desktop_crypto_t* crypto, const char* identity, int64 bits, char* public_key, uint64 public_key_capacity);
boole desktop_crypto__server_keys(desktop_crypto_t* crypto, const char* keys);
boole desktop_crypto__begin_dh(desktop_crypto_t* crypto, char* public_mix, uint64 public_mix_capacity);
boole desktop_crypto__finish_dh(desktop_crypto_t* crypto, const char* public_mix);
boole desktop_crypto__challenge(desktop_crypto_t* crypto, const char* encoded, char* plaintext, uint64 plaintext_capacity);
char* desktop_crypto__encrypt(desktop_crypto_t* crypto, const char* plaintext);
char* desktop_crypto__decrypt(desktop_crypto_t* crypto, const char* envelope, uint64 length);
void desktop_crypto__free_text(char* text);
void desktop_crypto__free_json(struct cJSON* json);
boole desktop_crypto__media_keys(desktop_crypto_t* crypto, desktop_media_keys_t* keys);
boole desktop_crypto__random(desktop_crypto_t* crypto, unsigned char* bytes, uint64 size);
boole desktop_crypto__content_keys(desktop_crypto_t* crypto, desktop_content_keys_t* keys);
char* desktop_crypto__keys_json(const desktop_content_keys_t* keys);
boole desktop_crypto__read_keys(const char* json, desktop_content_keys_t* keys);
char* desktop_crypto__content_encrypt(desktop_crypto_t* crypto, const desktop_content_keys_t* keys, const char* text);
char* desktop_crypto__content_decrypt(const desktop_content_keys_t* keys, const char* text);
char* desktop_crypto__attachment_encrypt(const desktop_content_keys_t* keys, const char* text, uint64 padding);
char* desktop_crypto__picture_encrypt(desktop_crypto_t* crypto, const desktop_content_keys_t* keys, const char* text, uint64 padding);
char* desktop_crypto__attachment_decrypt(const desktop_content_keys_t* keys, const char* text);
char* desktop_crypto__hybrid_encrypt(desktop_crypto_t* crypto, const char* public_key, const char* text);
char* desktop_crypto__hybrid_decrypt(desktop_crypto_t* crypto, const char* text);

#endif
