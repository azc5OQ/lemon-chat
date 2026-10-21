// This file handles the encryption and decryption of Lemon Chat metadata sent
// between a client and the server. Metadata is information such as channel
// names, usernames and message types. The server needs to read this information
// to know what a client is asking it to do.
//
// When sending a message, this file chooses the order of the encryption keys
// and puts the encrypted result into the JSON message sent over the connection.
// When receiving a message, it checks whether the protected message was changed
// and uses the keys in the required order to read it. The encryption functions
// themselves are also here. media_auth.c calls the same functions for audio
// and video, so both kinds of traffic use the same encryption code.
// Private message contents are encrypted and decrypted by the clients; they
// are separate from the metadata handled here.
#include "metadata_crypto.h"
#include "util.h"

#include "../third-party/dave-g-json/cJSON.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/base64.h"
#include "../third-party/kokke-tiny-aes-c/aes.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/hkdf.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/md.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include "base.h"
#include "memory_manager.h"

#include <stdio.h>
#include <string.h>

#define METADATA_IV_SIZE 16
#define METADATA_PADDED_SIZE 1026

#if AES_KEYLEN != 32 || CTR != 1
#error "Application crypto requires tiny-AES configured for AES-256-CTR"
#endif

// declarations
static int _metadata_crypto_internal__tag(const crypto_session_keys_t* session,
    const char* iv, const char* data, unsigned char* out);

/**
 * @brief Copies key bytes into a key structure and records which encryption or checking method uses them.
 *
 * @param crypto_key_t* key -> structure to fill; it is cleared if the type or byte count is unsupported
 * @param crypto_key_type_t type -> method the key will be used with
 * @param const unsigned char* bytes -> key bytes to copy; the caller keeps this buffer
 * @param size_t size -> number of bytes in the supplied key
 *
 * @return int -> 1 when the key is ready, or 0 when an argument, key type, or key size is invalid.
 */
int metadata_crypto__key_init(crypto_key_t* key, crypto_key_type_t type, const unsigned char* bytes, size_t size)
{
    crypto_key_t initialized = {0};

    if (key == NULL)
    {
        return 0;
    }
    if (bytes != NULL)
    {
        switch (type)
        {
            case CRYPTO_KEY_AES_256_CTR:
            {
                if (size == sizeof(initialized.material.aes_256_ctr))
                {
                    memcpy(initialized.material.aes_256_ctr, bytes, size);
                    initialized.type = type;
                }
                break;
            }
            case CRYPTO_KEY_HMAC_SHA256:
            {
                if (size == sizeof(initialized.material.hmac_sha256))
                {
                    memcpy(initialized.material.hmac_sha256, bytes, size);
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
 * @brief Turns a server password into the AES key used to protect messages.
 *
 * @param crypto_key_t* key -> structure to fill; it is cleared if the key cannot be created
 * @param crypto_key_type_t type -> requested key type; currently only CRYPTO_KEY_AES_256_CTR is supported
 * @param const char* password -> zero-terminated password text to hash with SHA256
 *
 * @return int -> 1 when the key was created, or 0 when the input or key type is unsupported or hashing fails.
 */
int metadata_crypto__key_from_password(crypto_key_t* key, crypto_key_type_t type, const char* password)
{
    unsigned char digest[32] = {0};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result = 0;

    if (key == NULL)
    {
        return 0;
    }
    memset(key, 0, sizeof(*key));
    // The server settings currently store passwords as text. SHA256 turns each
    // password into the 32 bytes used by its AES key, just as the client does.
    // If another key type is added, this function must also say how to turn a
    // password into the bytes that the new encryption method needs.
    if (type == CRYPTO_KEY_AES_256_CTR && password != NULL && md != NULL
        && mbedtls_md(md, (const unsigned char*)password, strlen(password), digest) == 0)
    {
        result = metadata_crypto__key_init(key, type, digest, sizeof(digest));
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return result;
}

/**
 * @brief Overwrites the memory containing an AES key and an HMAC key with
 * zeroes. This is used for temporary keys after they are no longer needed and
 * when creating connection keys fails, so the old key bytes do not remain in
 * memory until that memory is reused. mbedtls_platform_zeroize is used because
 * the compiler is not allowed to remove this memory clearing as unnecessary.
 *
 * @param crypto_session_keys_t* keys -> structure containing the key bytes to clear; NULL is allowed
 *
 * @return void
 */
void metadata_crypto__clear_session_keys(crypto_session_keys_t* keys)
{
    if (keys != NULL)
    {
        mbedtls_platform_zeroize(keys, sizeof(*keys));
    }
}

/**
 * @brief Creates the connection encryption key and message-checking key from its agreed shared secret.
 *
 * The client uses the same salt, labels, and text representation to produce the same two keys.
 *
 * @param const char* shared_secret -> nonempty, zero-terminated decimal text of the Diffie-Hellman shared secret
 * @param crypto_session_keys_t* keys -> structure to fill; it is cleared on failure
 *
 * @return int -> 1 when both keys are ready, or 0 when the secret is missing or either key cannot be created.
 */
int metadata_crypto__derive_session_keys(const char* shared_secret, crypto_session_keys_t* keys)
{
    static const unsigned char salt[] = "lemonchat-hkdf-salt-v1";
    static const unsigned char info[] = "lemonchat-dh-keys-v1";
    unsigned char material[64] = {0};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result = 0;

    if (keys == NULL)
    {
        return 0;
    }
    metadata_crypto__clear_session_keys(keys);
    // The client uses the same salt and info strings and writes the shared
    // secret as decimal text too. Both ends must do this exactly the same way
    // to get the same keys: the first 32 bytes are the encryption key, and the
    // next 32 bytes are the key used to check whether a message was changed.
    if (shared_secret != NULL && shared_secret[0] != '\0' && md != NULL
        && mbedtls_hkdf(md, salt, sizeof(salt) - 1,
            (const unsigned char*)shared_secret, strlen(shared_secret),
            info, sizeof(info) - 1, material, sizeof(material)) == 0
        && metadata_crypto__key_init(&keys->encryption, CRYPTO_KEY_AES_256_CTR, material, 32)
        && metadata_crypto__key_init(&keys->authentication, CRYPTO_KEY_HMAC_SHA256, material + 32, 32))
    {
        keys->is_ready = 1;
        result = 1;
    }
    else
    {
        metadata_crypto__clear_session_keys(keys);
    }
    mbedtls_platform_zeroize(material, sizeof(material));
    return result;
}

/**
 * @brief Encrypts or decrypts the supplied bytes using the method recorded in the key.
 *
 * @param const crypto_key_t* key -> key and encryption method to use
 * @param const unsigned char* nonce -> starting value for this operation; AES-256-CTR requires 16 bytes
 * @param size_t nonce_size -> number of bytes in the starting value
 * @param unsigned char* data -> buffer whose contents are replaced with the result
 * @param size_t size -> number of bytes to encrypt or decrypt
 * @param crypto_direction_t direction -> CRYPTO_ENCRYPT to encrypt or CRYPTO_DECRYPT to decrypt
 *
 * @return int -> 1 when the bytes were processed, or 0 when the arguments, key type, or starting value are invalid.
 */
int metadata_crypto__crypt(const crypto_key_t* key, const unsigned char* nonce, size_t nonce_size,
    unsigned char* data, size_t size, crypto_direction_t direction)
{
    if (key == NULL || nonce == NULL || data == NULL
        || (direction != CRYPTO_ENCRYPT && direction != CRYPTO_DECRYPT))
    {
        return 0;
    }
    switch (key->type)
    {
        case CRYPTO_KEY_AES_256_CTR:
        {
            struct AES_ctx aes;

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
 * @param size_t count -> number of keys to apply
 * @param const unsigned char* nonce -> starting value supplied to each encryption method
 * @param size_t nonce_size -> number of bytes in the starting value
 * @param unsigned char* data -> buffer changed by each encryption or decryption step
 * @param size_t size -> number of bytes to process in the buffer
 * @param crypto_direction_t direction -> CRYPTO_ENCRYPT for stored order or CRYPTO_DECRYPT for reverse order
 *
 * @attention On failure, earlier steps may already have changed data; the caller must discard that result.
 *
 * @return int -> 1 when all keys were applied, including an empty key array; 0 when an argument or a step fails.
 */
int metadata_crypto__crypt_layers(const crypto_key_t* keys, size_t count, const unsigned char* nonce, size_t nonce_size,
    unsigned char* data, size_t size, crypto_direction_t direction)
{
    size_t i = 0;
    size_t index = 0;

    if ((count != 0 && keys == NULL) || nonce == NULL || data == NULL
        || (direction != CRYPTO_ENCRYPT && direction != CRYPTO_DECRYPT))
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
        if (!metadata_crypto__crypt(&keys[index], nonce, nonce_size, data, size, direction))
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
 * @param const unsigned char* first -> first piece of data; may be NULL when first_size is zero
 * @param size_t first_size -> number of bytes in the first piece
 * @param const unsigned char* second -> second piece of data; may be NULL when second_size is zero
 * @param size_t second_size -> number of bytes in the second piece
 * @param unsigned char* out_tag -> caller-owned buffer with room for the 32-byte result
 *
 * @return int -> 1 when the check value was written, or 0 when the arguments are invalid or calculation fails.
 */
int metadata_crypto__hmac_sha256(const crypto_key_t* key, const unsigned char* first, size_t first_size,
    const unsigned char* second, size_t second_size, unsigned char* out_tag)
{
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t context;
    int result = 0;

    if (key == NULL || key->type != CRYPTO_KEY_HMAC_SHA256 || out_tag == NULL || md == NULL
        || (first == NULL && first_size != 0) || (second == NULL && second_size != 0))
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
 * @param const unsigned char* left -> first buffer, readable for size bytes
 * @param const unsigned char* right -> second buffer, readable for size bytes
 * @param size_t size -> number of bytes to compare
 *
 * @return int -> 1 when all compared bytes match, including when size is zero; otherwise 0.
 */
int metadata_crypto__equal(const unsigned char* left, const unsigned char* right, size_t size)
{
    unsigned char difference = 0;
    size_t i = 0;

    for (i = 0; i < size; i++)
    {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

/**
 * @brief Calculates the metadata message check over its encoded starting value followed by its encrypted text.
 *
 * @param const crypto_session_keys_t* session -> connection keys containing the message-checking key
 * @param const char* iv -> zero-terminated base64 text of the message starting value
 * @param const char* data -> zero-terminated base64 text of the encrypted message
 * @param unsigned char* out -> caller-owned buffer with room for the 32-byte check value
 *
 * @return int -> 1 when the check value was written, or 0 when its calculation fails.
 */
static int _metadata_crypto_internal__tag(const crypto_session_keys_t* session,
    const char* iv, const char* data, unsigned char* out)
{
    return metadata_crypto__hmac_sha256(&session->authentication,
        (const unsigned char*)iv, strlen(iv), (const unsigned char*)data, strlen(data), out);
}

/**
 * @brief Encrypts metadata with the server keys and optional connection keys, then builds the JSON text to send.
 *
 * @param const char* plaintext -> zero-terminated message text; the caller keeps this buffer
 * @param int64* out_size -> receives the JSON byte count including its final zero byte, or zero on failure
 * @param const crypto_session_keys_t* session -> ready connection keys; NULL sends only the server-key layers before key agreement
 *
 * @attention The server key settings must remain valid throughout this call. The caller supplies connection keys that cannot change during encryption.
 *
 * @return char* -> allocated, zero-terminated JSON text to release with memorymanager__free, or NULL_POINTER on failure.
 */
char* metadata_crypto__encrypt(const char* plaintext, int64* out_size, const crypto_session_keys_t* session)
{
    size_t text_size = 0;
    size_t buffer_size = 0;
    size_t encoded_capacity = 0;
    size_t written = 0;
    size_t envelope_capacity = 0;
    int envelope_size = 0;
    unsigned char iv[METADATA_IV_SIZE];
    unsigned char tag[32];
    unsigned char iv_base64[25] = {0};
    unsigned char tag_base64[45] = {0};
    unsigned char* buffer = NULL_POINTER;
    unsigned char* encoded = NULL_POINTER;
    char* envelope = NULL_POINTER;

    if (out_size == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    *out_size = 0;
    if (plaintext == NULL_POINTER || (session != NULL_POINTER && !session->is_ready)
        || g_server_settings.websocket_message_max_length <= 0)
    {
        return NULL_POINTER;
    }
    text_size = strlen(plaintext);
    buffer_size = text_size < METADATA_PADDED_SIZE ? METADATA_PADDED_SIZE : text_size;
    if (buffer_size > (uint64)g_server_settings.websocket_message_max_length || buffer_size > (SIZE_MAX - 2) / 4)
    {
        return NULL_POINTER;
    }
    encoded_capacity = 4 * ((buffer_size + 2) / 3) + 1;
    if (encoded_capacity > (uint64)g_server_settings.websocket_message_max_length
        || !util__fill_secure_random_bytes(iv, sizeof(iv)))
    {
        return NULL_POINTER;
    }
    // memorymanager__allocate fills new memory with zero bytes. After the text
    // is copied in, those zeros fill the rest of a short message up to 1026
    // bytes. This keeps the same minimum encrypted message size as before.
    buffer = (unsigned char*)memorymanager__allocate(buffer_size, MEMALLOC_TYPE_ENCRYPT);
    encoded = (unsigned char*)memorymanager__allocate(encoded_capacity, MEMALLOC_TYPE_ENCRYPT);
    if (buffer == NULL_POINTER || encoded == NULL_POINTER)
    {
        goto done;
    }
    memcpy(buffer, plaintext, text_size);
    // For a message sent by the server, encrypt with each key from the server
    // settings first. Then encrypt that result with the key that this client
    // and the server made from their shared secret when they connected.
    if (!metadata_crypto__crypt_layers(g_server_settings.keys, g_server_settings.keys_count,
        iv, sizeof(iv), buffer, buffer_size, CRYPTO_ENCRYPT))
    {
        goto done;
    }
    if (session != NULL_POINTER && !metadata_crypto__crypt(&session->encryption,
        iv, sizeof(iv), buffer, buffer_size, CRYPTO_ENCRYPT))
    {
        goto done;
    }
    if (mbedtls_base64_encode(encoded, encoded_capacity, &written, buffer, buffer_size) != 0
        || mbedtls_base64_encode(iv_base64, sizeof(iv_base64), &written, iv, sizeof(iv)) != 0)
    {
        goto done;
    }
    // Make a check value from the message's starting value and encrypted data,
    // using the separate message-checking key. The client calculates it again
    // when the message arrives. If the values differ, it rejects the message.
    // Both ends calculate this over the base64 text sent in the JSON fields.
    if (session != NULL_POINTER)
    {
        if (!_metadata_crypto_internal__tag(session, (char*)iv_base64, (char*)encoded, tag)
            || mbedtls_base64_encode(tag_base64, sizeof(tag_base64), &written, tag, sizeof(tag)) != 0)
        {
            goto done;
        }
    }
    envelope_capacity = encoded_capacity + sizeof(iv_base64) + sizeof(tag_base64) + 40;
    envelope = (char*)memorymanager__allocate(envelope_capacity, MEMALLOC_TYPE_ENCRYPT);
    if (envelope == NULL_POINTER)
    {
        goto done;
    }
    // These values contain only base64 characters, so there are no quotes or
    // backslashes inside them that need special handling. They can be written
    // straight into the JSON string without creating a cJSON object first.
    if (session != NULL_POINTER)
    {
        envelope_size = snprintf(envelope, envelope_capacity, "{\"iv\":\"%s\",\"data\":\"%s\",\"tag\":\"%s\"}", iv_base64, encoded, tag_base64);
    }
    else
    {
        envelope_size = snprintf(envelope, envelope_capacity, "{\"iv\":\"%s\",\"data\":\"%s\"}", iv_base64, encoded);
    }
    if (envelope_size <= 0 || (size_t)envelope_size >= envelope_capacity
        || envelope_size > g_server_settings.websocket_message_max_length)
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
 * @brief Derives temporary connection keys, encrypts metadata, and erases those temporary keys afterward.
 *
 * @param const char* plaintext -> zero-terminated metadata text to encrypt
 * @param int64* out_size -> receives the JSON byte count including its final zero byte, or zero on failure
 * @param const char* shared_secret -> zero-terminated agreed secret; NULL_POINTER uses only the server keys
 *
 * @return char* -> allocated, zero-terminated JSON text to release with memorymanager__free, or NULL_POINTER on failure.
 */
char* metadata_crypto__encrypt_with_secret(const char* plaintext, int64* out_size, const char* shared_secret)
{
    crypto_session_keys_t session = {0};
    char* result = NULL_POINTER;

    if (shared_secret == NULL_POINTER)
    {
        return metadata_crypto__encrypt(plaintext, out_size, NULL_POINTER);
    }
    if (out_size != NULL_POINTER)
    {
        *out_size = 0;
    }
    if (metadata_crypto__derive_session_keys(shared_secret, &session))
    {
        result = metadata_crypto__encrypt(plaintext, out_size, &session);
    }
    metadata_crypto__clear_session_keys(&session);
    return result;
}

/**
 * @brief Checks an incoming metadata message and decrypts it into text the server can read.
 *
 * @param const client_t* client -> existing client whose connection keys and login state apply to this message
 * @param const char* envelope_text -> zero-terminated JSON text containing the encoded starting value, encrypted data, and required check value
 * @param unsigned char* out -> caller-owned buffer that receives zero-terminated plaintext and is cleared on failure
 * @param size_t capacity -> size of out in bytes, including space for the final zero byte
 *
 * @attention The caller must keep the client and its keys stable, normally by holding the clients lock. Messages after key agreement must include a valid check value.
 *
 * @return boole -> TRUE when the message was checked and decrypted, or FALSE when it is invalid or cannot fit.
 */
boole metadata_crypto__decrypt_from_client(const client_t* client, const char* envelope_text,
    unsigned char* out, size_t capacity)
{
    unsigned char iv[METADATA_IV_SIZE];
    unsigned char expected_tag[32];
    unsigned char expected_tag_base64[45];
    size_t written = 0;
    size_t decoded_size = 0;
    cJSON* envelope = NULL_POINTER;
    cJSON* iv_item = NULL_POINTER;
    cJSON* data_item = NULL_POINTER;
    cJSON* tag_item = NULL_POINTER;
    const crypto_session_keys_t* session = NULL_POINTER;
    crypto_session_keys_t temporary_session = {0};
    boole result = FALSE;

    if (out == NULL_POINTER || capacity == 0)
    {
        return FALSE;
    }
    memset(out, 0, capacity);
    if (envelope_text == NULL_POINTER || client == NULL_POINTER || !client->is_existing)
    {
        return FALSE;
    }
    envelope = cJSON_Parse(envelope_text);
    iv_item = cJSON_GetObjectItemCaseSensitive(envelope, "iv");
    data_item = cJSON_GetObjectItemCaseSensitive(envelope, "data");
    if (!cJSON_IsString(iv_item) || !cJSON_IsString(data_item)
        || strlen(iv_item->valuestring) != 24
        || mbedtls_base64_decode(iv, sizeof(iv), &written,
            (const unsigned char*)iv_item->valuestring, strlen(iv_item->valuestring)) != 0
        || written != sizeof(iv))
    {
        goto done;
    }
    if (client->is_dh_shared_secret_agreed_upon)
    {
        session = &client->session_keys;
        if (!session->is_ready)
        {
            if (!metadata_crypto__derive_session_keys(client->dh_shared_secret, &temporary_session))
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
        if (!cJSON_IsString(tag_item) || strlen(tag_item->valuestring) != 44
            || !_metadata_crypto_internal__tag(session, iv_item->valuestring, data_item->valuestring, expected_tag)
            || mbedtls_base64_encode(expected_tag_base64, sizeof(expected_tag_base64), &written,
                expected_tag, sizeof(expected_tag)) != 0
            || !metadata_crypto__equal(expected_tag_base64, (const unsigned char*)tag_item->valuestring, 44))
        {
            goto done;
        }
    }
    // Leave room for the zero byte that marks the end of a C string. If the
    // incoming base64 text is invalid or its decoded bytes will not fit, stop
    // here without trying to decrypt them.
    if (mbedtls_base64_decode(out, capacity - 1, &decoded_size,
        (const unsigned char*)data_item->valuestring, strlen(data_item->valuestring)) != 0)
    {
        goto done;
    }
    // A client encrypts its outgoing message with its own connection key first
    // and then with the keys from the server settings. Undo those steps in the
    // opposite order: use the server keys from last to first, then use the
    // client's connection key to get the original text.
    if (!metadata_crypto__crypt_layers(g_server_settings.keys, g_server_settings.keys_count,
        iv, sizeof(iv), out, decoded_size, CRYPTO_DECRYPT))
    {
        goto done;
    }
    if (session != NULL_POINTER && !metadata_crypto__crypt(&session->encryption,
        iv, sizeof(iv), out, decoded_size, CRYPTO_DECRYPT))
    {
        goto done;
    }
    out[decoded_size] = '\0';
    result = TRUE;

done:
    if (!result)
    {
        memset(out, 0, capacity);
    }
    cJSON_Delete(envelope);
    metadata_crypto__clear_session_keys(&temporary_session);
    return result;
}
