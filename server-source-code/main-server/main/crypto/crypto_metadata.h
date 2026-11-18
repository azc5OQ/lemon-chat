#ifndef CRYPTO_METADATA_H
#define CRYPTO_METADATA_H

#include "../definitions.h"

boole crypto_metadata__key_init(crypto_key_t *key, crypto_key_type_t type, const ubyte *bytes, uint64 size);

boole crypto_metadata__key_from_password(crypto_key_t *key, crypto_key_type_t type, const char *password);

boole crypto_metadata__derive_session_keys(const char *shared_secret, crypto_session_keys_t *keys);

void crypto_metadata__clear_session_keys(crypto_session_keys_t *keys);

boole crypto_metadata__crypt(const crypto_key_t *key, const ubyte *nonce, uint64 nonce_size, ubyte *data, uint64 size, crypto_direction_t direction);
boole crypto_metadata__crypt_layers(const crypto_key_t *keys, uint64 count, const ubyte *nonce, uint64 nonce_size, ubyte *data, uint64 size, crypto_direction_t direction);

boole crypto_metadata__hmac_sha256(const crypto_key_t *key, const ubyte *first, uint64 first_size, const ubyte *second, uint64 second_size, ubyte *out_tag);

boole crypto_metadata__equal(const ubyte *left, const ubyte *right, uint64 size);

crypto_password_set_t *crypto_metadata__snapshot_passwords(const crypto_key_t *keys, uint64 count);
crypto_password_set_t *crypto_metadata__retain_passwords(crypto_password_set_t *passwords);
void crypto_metadata__release_passwords(crypto_password_set_t *passwords);
char *crypto_metadata__encrypt_for_connection(const char *plaintext, int64 *out_size, const crypto_session_keys_t *session, const crypto_password_set_t *passwords);

char *crypto_metadata__encrypt(const char *plaintext, int64 *out_size, const crypto_session_keys_t *session);
char *crypto_metadata__encrypt_with_secret(const char *plaintext, int64 *out_size, const char *shared_secret);

boole crypto_metadata__decrypt_from_client(const client_t *client, const char *envelope, ubyte *out, uint64 capacity);

boole crypto_metadata__crypt_session(const crypto_session_keys_t *session, const ubyte *nonce, uint64 nonce_size, ubyte *data, uint64 size, crypto_direction_t direction);

#endif
