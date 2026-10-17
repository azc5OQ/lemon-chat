#ifndef METADATA_CRYPTO_H
#define METADATA_CRYPTO_H

#include "definitions.h"

// The functions below that return int use 1 for success and 0 for failure.
// They fail if the input is invalid or the encryption library reports an error.
// An unknown key type also fails, so adding a type without implementing it
// cannot accidentally make the server treat that key as an AES key.

// Copy the supplied key bytes and remember their type. If the type or byte
// count is wrong, clear the key and report failure.
int metadata_crypto__key_init(crypto_key_t* key, crypto_key_type_t type, const unsigned char* bytes, size_t size);

// Turn a password from the server settings into a key the encryption code can use.
int metadata_crypto__key_from_password(crypto_key_t* key, crypto_key_type_t type, const char* password);

// Make both connection keys from the secret agreed on with the client: one to
// encrypt data and one to check that the encrypted data has not been changed.
int metadata_crypto__derive_session_keys(const char* shared_secret, crypto_session_keys_t* keys);

// Overwrite the memory containing an AES key and an HMAC key with zeroes. This
// is used for temporary keys after they are no longer needed and when creating
// connection keys fails, so old key bytes do not remain in memory until that
// memory is reused. The clearing function cannot be removed by the compiler.
void metadata_crypto__clear_session_keys(crypto_session_keys_t* keys);

// Encrypt or decrypt the bytes in the supplied data buffer. The result replaces
// those bytes and has the same length. The nonce is the starting value for this
// encryption operation; the code preparing a message supplies it separately
// from the key. metadata_crypto__crypt checks that the starting value has the
// size required by the key type. metadata_crypto__crypt_layers applies several
// keys, in stored order when encrypting and in reverse order when decrypting.
// If either function fails, the buffer may have been partly changed and the
// caller must not send or use its contents.
int metadata_crypto__crypt(const crypto_key_t* key, const unsigned char* nonce, size_t nonce_size,
    unsigned char* data, size_t size, crypto_direction_t direction);
int metadata_crypto__crypt_layers(const crypto_key_t* keys, size_t count, const unsigned char* nonce, size_t nonce_size,
    unsigned char* data, size_t size, crypto_direction_t direction);

// Calculate a 32-byte message-checking value, called HMAC-SHA256, using the key
// and the supplied data. The receiver uses the same key to calculate it again;
// a different result means the message must be rejected. There are two data
// arguments so metadata can pass its IV text and encrypted text separately,
// without first copying them into one buffer. The calculation treats them as
// one continuous sequence. Audio and video pass their packet as the first
// argument and use NULL with length 0 for the unused second argument.
int metadata_crypto__hmac_sha256(const crypto_key_t* key, const unsigned char* first, size_t first_size,
    const unsigned char* second, size_t second_size, unsigned char* out_tag);

// Compare every byte before returning the result. Do not stop at the first
// mismatch, because the time that takes could reveal how much of a tag matched.
int metadata_crypto__equal(const unsigned char* left, const unsigned char* right, size_t size);

// Encrypt metadata such as a channel name, username or message type, and return
// the JSON text to send over the connection. Its "iv" field holds the encryption
// starting value, "data" holds the encrypted bytes, and "tag" holds the check
// value that lets the receiver detect changes. Each field uses base64 text.
//
// Before the client and server agree on a secret, pass NULL for session; only
// the keys from the server settings are used and there is no tag yet. Afterward,
// pass the connection's prepared keys. Every message then includes a tag, and
// the receiving end must check it. The function with_secret prepares those keys
// from a shared secret when the caller does not already have them stored.
// The caller must free the returned string with memorymanager__free.
char* metadata_crypto__encrypt(const char* plaintext, int64* out_size, const crypto_session_keys_t* session);
char* metadata_crypto__encrypt_with_secret(const char* plaintext, int64* out_size, const char* shared_secret);

// Check and decrypt metadata received from a client. The caller must hold the
// clients read lock so the client's keys cannot change while they are being
// used. Return FALSE and clear the output buffer if any check fails. The buffer
// must have room for the decoded bytes and the zero byte that ends a C string.
boole metadata_crypto__decrypt_from_client(const client_t* client, const char* envelope,
    unsigned char* out, size_t capacity);

#endif
