#include "../clib/clib.h"
#include "keys_session.h"
#include "ciphers.h"
// This file handles the encryption and decryption of Lemon Chat metadata sent
// between a client and the server. Metadata is information such as channel
// names, usernames and message types. The server needs to read this information
// to know what a client is asking it to do.
//
// When sending a message, this file chooses the order of the encryption keys
// and puts the encrypted result into the JSON message sent over the connection.
// When receiving a message, it checks whether the protected message was changed
// and uses the keys in the required order to read it. The encryption functions
// themselves are also here. crypto_media.c calls the same functions for audio
// and video, so both kinds of traffic use the same encryption code.
// Private message contents are encrypted and decrypted by the clients; they
// are separate from the metadata handled here.
#include "crypto_metadata.h"
#include "../util.h"

#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/base64.h"
#include "../../third-party/kokke-tiny-aes-c/aes.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/hkdf.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/md.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include "../base.h"
#include "../memory_manager.h"

#include <stdio.h>
#include <string.h>

#define METADATA_IV_SIZE 16
#define METADATA_PADDED_SIZE 1026

#if AES_KEYLEN != 32 || CTR != 1
#error "Application crypto requires tiny-AES configured for AES-256-CTR"
#endif

// A snapshot has one owner on its client and one for each queued encryption job.
// The last owner erases it, including after disconnect or a cancelled queue job.
crypto_password_set_t *crypto_metadata__snapshot_passwords(const crypto_key_t *keys, uint64 count)
{
    crypto_password_set_t *result = NULL_POINTER;

    if (count > 100 || (count != 0 && keys == NULL_POINTER))
    {
        return NULL_POINTER;
    }
    result = (crypto_password_set_t *)memorymanager__allocate(sizeof(*result) + count * sizeof(crypto_key_t), MEMALLOC_TYPE_ENCRYPT);
    if (result != NULL_POINTER)
    {
        atomic_init(&result->references, 1);
        result->count = count;
        if (count != 0)
        {
            clib__memcpy(result->keys, keys, count * sizeof(crypto_key_t));
        }
    }
    return result;
}

crypto_password_set_t *crypto_metadata__retain_passwords(crypto_password_set_t *passwords)
{
    if (passwords != NULL_POINTER)
    {
        atomic_fetch_add_explicit(&passwords->references, 1, memory_order_relaxed);
    }
    return passwords;
}

void crypto_metadata__release_passwords(crypto_password_set_t *passwords)
{
    if (passwords != NULL_POINTER && atomic_fetch_sub_explicit(&passwords->references, 1, memory_order_acq_rel) == 1)
    {
        ciphers__clear(passwords, sizeof(*passwords) + passwords->count * sizeof(crypto_key_t));
        memorymanager__free((nuint)passwords);
    }
}

static char *_crypto_metadata_internal__encrypt(const char *plaintext, int64 *out_size, const crypto_session_keys_t *session, const crypto_key_t *passwords, uint64 password_count);

char *crypto_metadata__encrypt_for_connection(const char *plaintext, int64 *out_size, const crypto_session_keys_t *session, const crypto_password_set_t *passwords)
{
    return _crypto_metadata_internal__encrypt(plaintext, out_size, session, passwords != NULL_POINTER ? passwords->keys : NULL_POINTER, passwords != NULL_POINTER ? passwords->count : 0);
}

// Legacy helper for callers holding the settings lock. Socket queues always use snapshots.
char *crypto_metadata__encrypt(const char *plaintext, int64 *out_size, const crypto_session_keys_t *session)
{
    return _crypto_metadata_internal__encrypt(plaintext, out_size, session, g_server_settings.keys, g_server_settings.keys_count);
}

// declarations
static boole _crypto_metadata_internal__tag(const crypto_session_keys_t *session, const char *iv, const char *data, ubyte *out, boole sending);

/**
 * @brief Copies key bytes into a key structure and records which encryption or checking method uses
 *        them.
 *
 * @param crypto_key_t* key -> structure to fill; it is cleared if the type or byte count is unsupported
 * @param crypto_key_type_t type -> method the key will be used with
 * @param const ubyte* bytes -> key bytes to copy; the caller keeps this buffer
 * @param uint64 size -> number of bytes in the supplied key
 *
 * @return boole -> 1 when the key is ready, or 0 when an argument, key type, or key size is invalid.
 */
boole crypto_metadata__key_init(crypto_key_t *key, crypto_key_type_t type, const ubyte *bytes, uint64 size)
{
    crypto_key_t initialized = { 0 };

    if (key == NULL)
    {
        return 0;
    }
    if (bytes != NULL)
    {
        switch (type)
        {
        case CRYPTO_KEY_CHACHA20:
        case CRYPTO_KEY_CAMELLIA_256_CTR:
        case CRYPTO_KEY_TWOFISH_256_CTR:
        case CRYPTO_KEY_SERPENT_256_CTR:
        case CRYPTO_KEY_TRIVIUM_80:
        case CRYPTO_KEY_AES_256_CTR:
        {
            if (size == sizeof(initialized.material.aes_256_ctr))
            {
                clib__memcpy(initialized.material.aes_256_ctr, bytes, size);
                initialized.type = type;
            }
            break;
        }
        case CRYPTO_KEY_HMAC_SHA256:
        {
            if (size == sizeof(initialized.material.hmac_sha256))
            {
                clib__memcpy(initialized.material.hmac_sha256, bytes, size);
                initialized.type = type;
            }
            break;
        }
        default:
        {
            break;
        }
        }
    }
    *key = initialized;
    mbedtls_platform_zeroize(&initialized, sizeof(initialized));
    return key->type != CRYPTO_KEY_NONE;
}

/**
 * @brief Hashes a server password into key bytes for the selected encryption method.
 *
 * @param crypto_key_t* key -> structure to fill; it is cleared if the key cannot be created
 * @param crypto_key_type_t type -> supported encryption method to record with the key
 * @param const char* password -> zero-terminated password text to hash with SHA256
 *
 * @return boole -> 1 when the key was created, or 0 when the input or key type is unsupported or hashing fails.
 */
boole crypto_metadata__key_from_password(crypto_key_t *key, crypto_key_type_t type, const char *password)
{
    ubyte digest[32] = { 0 };
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int64 result = 0;

    if (key == NULL)
    {
        return 0;
    }
    clib__memset(key, 0, sizeof(*key));

    // The server settings currently store passwords as text. SHA256 turns each
    // password into its 32 stored key bytes, just as the client does. The key
    // type records which supported encryption method will use those bytes.
    if (ciphers__name(type) != NULL && password != NULL && md != NULL)
    {
        if (mbedtls_md(md, (const ubyte *)password, clib__strlen(password), digest) == 0)
        {
            result = crypto_metadata__key_init(key, type, digest, sizeof(digest));
        }
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return result;
}

/**
 * @brief Overwrites the session's AES and HMAC keys with zeroes.
 *
 * This clears temporary keys after use and connection keys when their creation
 * fails, so the old key bytes do not remain until the memory is reused.
 * mbedtls_platform_zeroize prevents the compiler from removing the wipe.
 *
 * @param crypto_session_keys_t* keys -> structure containing the key bytes to clear; NULL is allowed
 *
 * @return void
 */
void crypto_metadata__clear_session_keys(crypto_session_keys_t *keys)
{
    if (keys != NULL)
    {
        mbedtls_platform_zeroize(keys, sizeof(*keys));
    }
}

/**
 * @brief Creates the connection encryption key and message-checking key from its agreed shared
 *        secret.
 *
 * The client uses the same salt, labels, and text representation to produce the same two keys.
 *
 * @param const char* shared_secret -> nonempty, zero-terminated decimal text of the Diffie-Hellman shared secret
 * @param crypto_session_keys_t* keys -> structure to fill; it is cleared on failure
 *
 * @return boole -> 1 when both keys are ready, or 0 when the secret is missing or either key cannot be created.
 */
boole crypto_metadata__derive_session_keys(const char *shared_secret, crypto_session_keys_t *keys)
{
    static const ubyte salt[] = "lemonchat-hkdf-salt-v1";
    static const ubyte info[] = "lemonchat-dh-keys-v1";
    ubyte material[64] = { 0 };
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    boole result = FALSE;
    boole status = FALSE;

    if (keys == NULL)
    {
        return 0;
    }
    crypto_metadata__clear_session_keys(keys);

    // The client uses the same salt and info strings and writes the shared
    // secret as decimal text too. Both ends must do this exactly the same way
    // to get the same keys: the first 32 bytes are the encryption key, and the
    // next 32 bytes are the key used to check whether a message was changed.
    status = shared_secret != NULL;
    if (status == TRUE)
    {
        status = shared_secret[0] != '\0';
    }
    if (status == TRUE)
    {
        status = md != NULL;
    }
    if (status == TRUE)
    {
        status = mbedtls_hkdf(md, salt, sizeof(salt) - 1, (const ubyte *)shared_secret, clib__strlen(shared_secret), info, sizeof(info) - 1, material, sizeof(material)) == 0;
    }
    if (status == TRUE)
    {
        status = crypto_metadata__key_init(&keys->encryption, CRYPTO_KEY_AES_256_CTR, material, 32) != 0;
    }
    if (status == TRUE)
    {
        status = crypto_metadata__key_init(&keys->authentication, CRYPTO_KEY_HMAC_SHA256, material + 32, 32) != 0;
    }
    if (status == TRUE)
    {
        keys->is_ready = 1;
        result = TRUE;
    }
    else
    {
        crypto_metadata__clear_session_keys(keys);
    }
    mbedtls_platform_zeroize(material, sizeof(material));
    return result;
}

/**
 * @brief Encrypts or decrypts the supplied bytes using the method recorded in the key.
 *
 * @param const crypto_key_t* key -> key and encryption method to use
 * @param const ubyte* nonce -> starting value for this operation; AES-256-CTR requires 16 bytes
 * @param uint64 nonce_size -> number of bytes in the starting value
 * @param ubyte* data -> buffer whose contents are replaced with the result
 * @param uint64 size -> number of bytes to encrypt or decrypt
 * @param crypto_direction_t direction -> CRYPTO_ENCRYPT to encrypt or CRYPTO_DECRYPT to decrypt
 *
 * @return boole -> 1 when the bytes were processed, or 0 when the arguments, key type, or starting value are invalid.
 */
boole crypto_metadata__crypt(const crypto_key_t *key, const ubyte *nonce, uint64 nonce_size, ubyte *data, uint64 size, crypto_direction_t direction)
{
    struct AES_ctx aes = { 0 };

    if (key == NULL || nonce == NULL || data == NULL || size > SIZE_MAX || direction != CRYPTO_ENCRYPT && direction != CRYPTO_DECRYPT)
    {
        return 0;
    }
    if (key->type != CRYPTO_KEY_AES_256_CTR)
    {
        return nonce_size == 16 && ciphers__crypt(key->type, key->material.raw, nonce, data, size);
    }
    switch (key->type)
    {
    case CRYPTO_KEY_AES_256_CTR:
    {
        if (nonce_size != AES_BLOCKLEN)
        {
            return 0;
        }

        // Use tiny-AES to encrypt or decrypt the supplied bytes. With this
        // AES mode, applying the same key and starting value a second time
        // turns the encrypted bytes back into the original bytes, so both
        // directions call the same function.
        AES_init_ctx_iv(&aes, key->material.aes_256_ctr, nonce);
        AES_CTR_xcrypt_buffer(&aes, data, size);
        mbedtls_platform_zeroize(&aes, sizeof(aes));
        return 1;
    }
    default:
    {
        return 0;
    }
    }
}

/**
 * @brief Applies every key to the same buffer, reversing their order when decrypting.
 *
 * @param const crypto_key_t* keys -> array of keys in encryption order; may be NULL when count is zero
 * @param uint64 count -> number of keys to apply
 * @param const ubyte* nonce -> starting value supplied to each encryption method
 * @param uint64 nonce_size -> number of bytes in the starting value
 * @param ubyte* data -> buffer changed by each encryption or decryption step
 * @param uint64 size -> number of bytes to process in the buffer
 * @param crypto_direction_t direction -> CRYPTO_ENCRYPT for stored order or CRYPTO_DECRYPT for reverse order
 *
 * @attention On failure, earlier steps may already have changed data; the caller must discard that result.
 *
 * @return boole -> 1 when all keys were applied, including an empty key array; 0 when an argument or a step fails.
 */
boole crypto_metadata__crypt_layers(const crypto_key_t *keys, uint64 count, const ubyte *nonce, uint64 nonce_size, ubyte *data, uint64 size, crypto_direction_t direction)
{
    uint64 i = 0;
    uint64 index = 0;

    if (count != 0 &&
        keys == NULL ||
        count > SIZE_MAX / sizeof(*keys) ||
        nonce == NULL ||
        data == NULL ||
        size > SIZE_MAX ||
        direction != CRYPTO_ENCRYPT &&
        direction != CRYPTO_DECRYPT)
    {
        return 0;
    }

    // Encrypt with the keys in their stored order, then decrypt in the opposite
    // order. For example, data encrypted with A, then B, then C must be opened
    // with C, then B, then A. Keep this rule even though the current AES mode
    // also happens to work when the order changes; another method might not.
    for (i = 0; i < count; i++)
    {
        index = direction == CRYPTO_ENCRYPT ? i : count - 1 - i;
        if (!crypto_metadata__crypt(&keys[index], nonce, nonce_size, data, size, direction))
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Calculates the message-checking value for two consecutive pieces of data.
 *
 * @param const crypto_key_t* key -> HMAC-SHA256 key used to check the data
 * @param const ubyte* first -> first piece of data; may be NULL when first_size is zero
 * @param uint64 first_size -> number of bytes in the first piece
 * @param const ubyte* second -> second piece of data; may be NULL when second_size is zero
 * @param uint64 second_size -> number of bytes in the second piece
 * @param ubyte* out_tag -> caller-owned buffer with room for the 32-byte result
 *
 * @return boole -> 1 when the check value was written, or 0 when the arguments are invalid or calculation fails.
 */
boole crypto_metadata__hmac_sha256(const crypto_key_t *key, const ubyte *first, uint64 first_size, const ubyte *second, uint64 second_size, ubyte *out_tag)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t context;
    int64 result = 0;

    if (key == NULL)
    {
        return 0;
    }
    if (key->type != CRYPTO_KEY_HMAC_SHA256 ||
        out_tag == NULL ||
        md == NULL ||
        first == NULL &&
        first_size != 0 ||
        first_size > SIZE_MAX ||
        second == NULL &&
        second_size != 0 ||
        second_size > SIZE_MAX)
    {
        return 0;
    }
    mbedtls_md_init(&context);
    result = mbedtls_md_setup(&context, md, 1);
    if (result == 0)
    {
        result = mbedtls_md_hmac_starts(&context, key->material.hmac_sha256, sizeof(key->material.hmac_sha256));
    }
    if (result == 0 && first_size != 0)
    {
        result = mbedtls_md_hmac_update(&context, first, first_size);
    }
    if (result == 0 && second_size != 0)
    {
        result = mbedtls_md_hmac_update(&context, second, second_size);
    }
    if (result == 0)
    {
        result = mbedtls_md_hmac_finish(&context, out_tag);
    }
    mbedtls_md_free(&context);
    return result == 0;
}

/**
 * @brief Compares every byte of two buffers without stopping at the first difference.
 *
 * @param const ubyte* left -> first buffer, readable for size bytes
 * @param const ubyte* right -> second buffer, readable for size bytes
 * @param uint64 size -> number of bytes to compare
 *
 * @return boole -> 1 when all compared bytes match, including when size is zero; otherwise 0.
 */
boole crypto_metadata__equal(const ubyte *left, const ubyte *right, uint64 size)
{
    ubyte difference = 0;
    uint64 i = 0;

    for (i = 0; i < size; i++)
    {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

/**
 * @brief Calculates the metadata message check over its encoded starting value followed by its
 *        encrypted text.
 *
 * @param const crypto_session_keys_t* session -> connection keys containing the message-checking key
 * @param const char* iv -> zero-terminated base64 text of the message starting value
 * @param const char* data -> zero-terminated base64 text of the encrypted message
 * @param ubyte* out -> caller-owned buffer with room for the 32-byte check value
 *
 * @return boole -> 1 when the check value was written, or 0 when its calculation fails.
 */
static boole _crypto_metadata_internal__tag(const crypto_session_keys_t *session, const char *iv, const char *data, ubyte *out, boole sending)
{
    crypto_key_t incoming;
    const crypto_key_t *key = &session->authentication;
    boole result = FALSE;

    if (session->negotiated.count != 0 && sending == FALSE)
    {
        crypto_metadata__key_init(&incoming, CRYPTO_KEY_HMAC_SHA256, session->negotiated.client_mac, 32);
        key = &incoming;
    }
    result = crypto_metadata__hmac_sha256(key, (const ubyte *)iv, clib__strlen(iv), (const ubyte *)data, clib__strlen(data), out);
    ciphers__clear(&incoming, sizeof(incoming));
    return result;
}

/**
 * @brief Encrypts metadata with the server keys and optional connection keys, then builds the JSON
 *        text to send.
 *
 * @param const char* plaintext -> zero-terminated message text; the caller keeps this buffer
 * @param int64* out_size -> receives the JSON byte count including its final zero byte, or zero on failure
 * @param const crypto_session_keys_t* session -> ready connection keys; NULL sends only the server-key layers before key agreement
 *
 * @attention The server key settings must remain valid throughout this call. The caller supplies connection keys that cannot change during encryption.
 *
 * @return char* -> allocated, zero-terminated JSON text to release with memorymanager__free, or NULL_POINTER on failure.
 */
static char *_crypto_metadata_internal__encrypt(const char *plaintext, int64 *out_size, const crypto_session_keys_t *session, const crypto_key_t *passwords, uint64 password_count)
{
    uint64 text_size = 0;
    uint64 buffer_size = 0;
    uint64 encoded_capacity = 0;
    size_t written = 0; // mbedTLS writes through size_t*.
    uint64 envelope_capacity = 0;
    int64 envelope_size = 0;
    ubyte iv[METADATA_IV_SIZE] = { 0 };
    ubyte tag[32] = { 0 };
    ubyte iv_base64[25] = { 0 };
    ubyte tag_base64[45] = { 0 };
    ubyte *buffer = NULL_POINTER;
    ubyte *encoded = NULL_POINTER;
    char *envelope = NULL_POINTER;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;

    if (out_size == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    *out_size = 0;
    status0 = plaintext == NULL_POINTER;
    if (status0 == TRUE)
    {
        return NULL_POINTER;
    }
    if (session != NULL_POINTER)
    {
        status0 = session->is_ready == FALSE;
    }
    status1 = g_server_settings.websocket_message_max_length <= 0;
    if (status0 == TRUE || status1 == TRUE)
    {
        return NULL_POINTER;
    }

    // Check the padded size before calculating the Base64 buffer size.
    text_size = clib__strlen(plaintext);
    buffer_size = text_size < METADATA_PADDED_SIZE ? METADATA_PADDED_SIZE : text_size;
    status0 = buffer_size > (uint64)g_server_settings.websocket_message_max_length;
    status1 = buffer_size > (SIZE_MAX - 2) / 4;
    if (status0 == TRUE || status1 == TRUE)
    {
        return NULL_POINTER;
    }
    encoded_capacity = 4 * ((buffer_size + 2) / 3) + 1;
    status0 = encoded_capacity > (uint64)g_server_settings.websocket_message_max_length;
    if (status0 == TRUE)
    {
        return NULL_POINTER;
    }

    // Generate a fresh IV for this encrypted request.
    status0 = util__fill_secure_random_bytes(iv, sizeof(iv)) == FALSE;
    if (status0 == TRUE)
    {
        return NULL_POINTER;
    }

    // memorymanager__allocate fills new memory with zero bytes. After the text
    // is copied in, those zeros fill the rest of a short message up to 1026
    // bytes. This keeps the same minimum encrypted message size as before.
    buffer = (ubyte *)memorymanager__allocate(buffer_size, MEMALLOC_TYPE_ENCRYPT);
    encoded = (ubyte *)memorymanager__allocate(encoded_capacity, MEMALLOC_TYPE_ENCRYPT);
    status0 = buffer == NULL_POINTER;
    status1 = encoded == NULL_POINTER;
    if (status0 == TRUE || status1 == TRUE)
    {
        goto done;
    }
    clib__memcpy(buffer, plaintext, text_size);

    // For a message sent by the server, encrypt with each key from the server
    // settings first. Then encrypt that result with the key that this client
    // and the server made from their shared secret when they connected.
    if (!crypto_metadata__crypt_layers(passwords, password_count, iv, sizeof(iv), buffer, buffer_size, CRYPTO_ENCRYPT))
    {
        goto done;
    }
    if (session != NULL_POINTER)
    {
        status0 = crypto_metadata__crypt_session(session, iv, sizeof(iv), buffer, buffer_size, CRYPTO_ENCRYPT) != 0;
        if (status0 == FALSE)
        {
            goto done;
        }
    }

    // Encode the encrypted bytes as text for the JSON envelope.
    status0 = mbedtls_base64_encode(encoded, encoded_capacity, &written, buffer, buffer_size) == 0;
    if (status0 == FALSE)
    {
        goto done;
    }
    status0 = mbedtls_base64_encode(iv_base64, sizeof(iv_base64), &written, iv, sizeof(iv)) == 0;
    if (status0 == FALSE)
    {
        goto done;
    }

    // Make a check value from the message's starting value and encrypted data,
    // using the separate message-checking key. The client calculates it again
    // when the message arrives. If the values differ, it rejects the message.
    // Both ends calculate this over the base64 text sent in the JSON fields.
    if (session != NULL_POINTER)
    {
        status0 = _crypto_metadata_internal__tag(session, (char *)iv_base64, (char *)encoded, tag, TRUE) != 0;
        if (status0 == FALSE)
        {
            goto done;
        }
        status0 = mbedtls_base64_encode(tag_base64, sizeof(tag_base64), &written, tag, sizeof(tag)) == 0;
        if (status0 == FALSE)
        {
            goto done;
        }
    }
    envelope_capacity = encoded_capacity + sizeof(iv_base64) + sizeof(tag_base64) + 40;
    envelope = (char *)memorymanager__allocate(envelope_capacity, MEMALLOC_TYPE_ENCRYPT);
    if (envelope == NULL_POINTER)
    {
        goto done;
    }

    // These values contain only base64 characters, so there are no quotes or
    // backslashes inside them that need special handling. They can be written
    // straight into the JSON string without creating a cJSON object first.
    if (session != NULL_POINTER)
    {
        envelope_size = clib__string_format(envelope, envelope_capacity, "{\"iv\":\"%s\",\"data\":\"%s\",\"tag\":\"%s\"}", iv_base64, encoded, tag_base64);
    }
    else
    {
        envelope_size = clib__string_format(envelope, envelope_capacity, "{\"iv\":\"%s\",\"data\":\"%s\"}", iv_base64, encoded);
    }
    status0 = envelope_size <= 0;
    status1 = (uint64)envelope_size >= envelope_capacity;
    status2 = envelope_size > g_server_settings.websocket_message_max_length;
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE)
    {
        memorymanager__free((nuint)envelope);
        envelope = NULL_POINTER;
        goto done;
    }
    *out_size = envelope_size + 1;

done:
    if (buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)buffer);
    }
    if (encoded != NULL_POINTER)
    {
        memorymanager__free((nuint)encoded);
    }
    return envelope;
}

/**
 * @brief Derives temporary connection keys, encrypts metadata, and erases those temporary keys
 *        afterward.
 *
 * @param const char* plaintext -> zero-terminated metadata text to encrypt
 * @param int64* out_size -> receives the JSON byte count including its final zero byte, or zero on failure
 * @param const char* shared_secret -> zero-terminated agreed secret; NULL_POINTER uses only the server keys
 *
 * @return char* -> allocated, zero-terminated JSON text to release with memorymanager__free, or NULL_POINTER on failure.
 */
char *crypto_metadata__encrypt_with_secret(const char *plaintext, int64 *out_size, const char *shared_secret)
{
    crypto_session_keys_t session = { 0 };
    char *result = NULL_POINTER;

    if (shared_secret == NULL_POINTER)
    {
        return crypto_metadata__encrypt(plaintext, out_size, NULL_POINTER);
    }
    if (out_size != NULL_POINTER)
    {
        *out_size = 0;
    }
    if (crypto_metadata__derive_session_keys(shared_secret, &session))
    {
        result = crypto_metadata__encrypt(plaintext, out_size, &session);
    }
    crypto_metadata__clear_session_keys(&session);
    return result;
}

/** @brief Rejects non-flat or oversized envelope structure before JSON allocates its tree. */
static boole _crypto_metadata_internal__is_flat_envelope(const char *text)
{
    const char *cursor = NULL_POINTER;
    ubyte value = 0;
    boole quoted = FALSE;
    boole escaped = FALSE;
    uint64 depth = 0;
    uint64 strings = 0;
    uint64 fields = 0;

    for (cursor = text; *cursor != 0; cursor++)
    {
        value = (ubyte)*cursor;
        if (value < 32 && (quoted || (value != '\t' && value != '\r' && value != '\n')))
        {
            return FALSE;
        }
        if (quoted)
        {
            if (escaped)
            {
                if (strncmp(cursor, "u0000", 5) == 0)
                {
                    return FALSE;
                }
                escaped = FALSE;
            }
            else if (value == '\\')
            {
                escaped = TRUE;
            }
            else if (value == '"')
            {
                quoted = FALSE;
            }
        }
        else if (value == '"')
        {
            if (++strings > 6)
            {
                return FALSE;
            }
            quoted = TRUE;
        }
        else if (value == '[' || value == ']')
        {
            return FALSE;
        }
        else if (value == '{')
        {
            if (++depth > 1)
            {
                return FALSE;
            }
        }
        else if (value == '}')
        {
            if (depth == 0)
            {
                return FALSE;
            }
            depth--;
        }
        else if (value == ':' && ++fields > 3)
        {
            return FALSE;
        }
    }
    return !quoted && depth == 0;
}

/**
 * @brief Checks an incoming metadata message and decrypts it into text the server can read.
 *
 * @param const client_t* client -> existing client whose connection keys and login state apply to this message
 * @param const char* envelope_text -> zero-terminated JSON text containing the encoded starting value, encrypted data, and required check value
 * @param ubyte* out -> caller-owned buffer that receives zero-terminated plaintext and is cleared on failure
 * @param uint64 capacity -> size of out in bytes, including space for the final zero byte
 *
 * @attention The caller must keep the client and its keys stable, normally by holding the clients lock. Messages after key agreement must include a valid check value.
 *
 * @return boole -> TRUE when the message was checked and decrypted, or FALSE when it is invalid or cannot fit.
 */
boole crypto_metadata__decrypt_from_client(const client_t *client, const char *envelope_text, ubyte *out, uint64 capacity)
{
    cJSON *field = NULL_POINTER;
    uint64 fields = 0;
    ubyte iv[METADATA_IV_SIZE] = { 0 };
    ubyte expected_tag[32] = { 0 };
    ubyte expected_tag_base64[45] = { 0 };
    size_t written = 0; // mbedTLS writes through size_t*.
    size_t decoded_size = 0; // mbedTLS writes through size_t*.
    cJSON *envelope = NULL_POINTER;
    cJSON *iv_item = NULL_POINTER;
    cJSON *data_item = NULL_POINTER;
    cJSON *tag_item = NULL_POINTER;
    const crypto_session_keys_t *session = NULL_POINTER;
    crypto_session_keys_t temporary_session = { 0 };
    boole result = FALSE;
    boole status0 = FALSE;
    boole status1 = FALSE;

    status0 = out == NULL_POINTER;
    status1 = capacity == 0 || capacity > SIZE_MAX;
    if (status0 == TRUE || status1 == TRUE)
    {
        return FALSE;
    }
    clib__memset(out, 0, capacity);
    status0 = envelope_text == NULL_POINTER;
    status1 = client == NULL_POINTER;
    if (status0 == TRUE || status1 == TRUE)
    {
        return FALSE;
    }
    status0 = client->is_existing == FALSE;
    if (status0 == TRUE)
    {
        return FALSE;
    }
    if (!_crypto_metadata_internal__is_flat_envelope(envelope_text))
    {
        goto done;
    }
    envelope = cJSON_ParseWithOpts(envelope_text, NULL_POINTER, TRUE);
    if (!cJSON_IsObject(envelope))
    {
        goto done;
    }
    cJSON_ArrayForEach(field, envelope)
    {
        if (!field->string || !cJSON_IsString(field) || cJSON_GetObjectItemCaseSensitive(envelope, field->string) != field || (strcmp(field->string, "iv") != 0 && strcmp(field->string, "data") != 0 && strcmp(field->string, "tag") != 0))
        {
            goto done;
        }
        fields++;
    }
    if (fields < 2 || fields > 3)
    {
        goto done;
    }
    iv_item = cJSON_GetObjectItemCaseSensitive(envelope, "iv");
    data_item = cJSON_GetObjectItemCaseSensitive(envelope, "data");
    status0 = cJSON_IsString(iv_item) != 0;
    status1 = cJSON_IsString(data_item) != 0;
    if (status0 == FALSE || status1 == FALSE)
    {
        goto done;
    }
    status0 = clib__strlen(iv_item->valuestring) == 24;
    if (status0 == FALSE)
    {
        goto done;
    }
    status0 = mbedtls_base64_decode(iv, sizeof(iv), &written, (const ubyte *)iv_item->valuestring, clib__strlen(iv_item->valuestring)) == 0;
    if (status0 == FALSE)
    {
        goto done;
    }
    status0 = written == sizeof(iv);
    if (status0 == FALSE)
    {
        goto done;
    }
    if (client->is_dh_shared_secret_agreed_upon)
    {
        session = &client->session_keys;
        if (!session->is_ready)
        {
            if (!crypto_metadata__derive_session_keys(client->dh_shared_secret, &temporary_session))
            {
                goto done;
            }
            session = &temporary_session;
        }
        tag_item = cJSON_GetObjectItemCaseSensitive(envelope, "tag");

        // Once the client and server have agreed on a secret, every message
        // must include this check value, called the tag. Reject a message that
        // leaves it out; otherwise someone could remove the tag to skip the
        // check that the message has not been changed.
        status0 = cJSON_IsString(tag_item) != 0;
        if (status0 == FALSE)
        {
            goto done;
        }
        status0 = clib__strlen(tag_item->valuestring) == 44;
        if (status0 == FALSE)
        {
            goto done;
        }
        status0 = _crypto_metadata_internal__tag(session, iv_item->valuestring, data_item->valuestring, expected_tag, FALSE) != 0;
        if (status0 == FALSE)
        {
            goto done;
        }
        status0 = mbedtls_base64_encode(expected_tag_base64, sizeof(expected_tag_base64), &written, expected_tag, sizeof(expected_tag)) == 0;
        if (status0 == FALSE)
        {
            goto done;
        }
        status0 = crypto_metadata__equal(expected_tag_base64, (const ubyte *)tag_item->valuestring, 44) != 0;
        if (status0 == FALSE)
        {
            goto done;
        }
    }

    // Leave room for the zero byte that marks the end of a C string. If the
    // incoming base64 text is invalid or its decoded bytes will not fit, stop
    // here without trying to decrypt them.
    if (mbedtls_base64_decode(out, capacity - 1, &decoded_size, (const ubyte *)data_item->valuestring, clib__strlen(data_item->valuestring)) != 0)
    {
        goto done;
    }

    // A client encrypts its outgoing message with its own connection key first
    // and then with the keys from the server settings. Undo those steps in the
    // opposite order: use the server keys from last to first, then use the
    // client's connection key to get the original text.
    if (!crypto_metadata__crypt_layers(client->password_keys != NULL_POINTER ? client->password_keys->keys : NULL_POINTER, client->password_keys != NULL_POINTER ? client->password_keys->count : 0, iv, sizeof(iv), out, decoded_size, CRYPTO_DECRYPT))
    {
        goto done;
    }
    if (session != NULL_POINTER)
    {
        status0 = crypto_metadata__crypt_session(session, iv, sizeof(iv), out, decoded_size, CRYPTO_DECRYPT) != 0;
        if (status0 == FALSE)
        {
            goto done;
        }
    }
    out[decoded_size] = '\0';
    result = TRUE;

done:
    if (!result)
    {
        clib__memset(out, 0, capacity);
    }
    cJSON_Delete(envelope);
    crypto_metadata__clear_session_keys(&temporary_session);
    return result;
}

/** @brief Applies negotiated session layers, preserving the legacy one-layer format. */
boole crypto_metadata__crypt_session(const crypto_session_keys_t *session, const ubyte *nonce, uint64 nonce_size, ubyte *data, uint64 size, crypto_direction_t direction)
{
    if (session->negotiated.count != 0)
    {
        return nonce_size == 16 && keys_session__crypt(&session->negotiated, direction == CRYPTO_ENCRYPT, direction == CRYPTO_ENCRYPT, nonce, data, size);
    }
    return crypto_metadata__crypt(&session->encryption, nonce, nonce_size, data, size, direction);
}
