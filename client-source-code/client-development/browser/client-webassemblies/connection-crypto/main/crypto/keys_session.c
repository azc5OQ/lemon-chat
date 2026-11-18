#include "keys_session.h"
#include "../../../common/clib.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"

/**
 * @brief starts the handshake hash with the policy, nonce and public key
 *
 * @param unsigned char[32] transcript -> receives the initial handshake hash
 * @param const lemon_session_policy* policy -> negotiated exchange count and cipher layers
 * @param const char* nonce -> server nonce containing exactly 32 characters
 * @param const char* public_key -> client's public identity key
 *
 * @return int -> 1 on success, otherwise 0
 */
int keys_session__begin_transcript(unsigned char transcript[32], const lemon_session_policy *policy, const char *nonce, const char *public_key)
{
    char header[256];
    int size;
    if (policy->exchanges < 1 || policy->exchanges > LEMON_MAX_DH_EXCHANGES || policy->layers < 1 || policy->layers > LEMON_MAX_SESSION_LAYERS || ciphers__name(policy->cipher) == NULL || strlen(nonce) != 32)
    {
        return 0;
    }
    memset(transcript, 0, 32);
    size = snprintf(header, sizeof(header), "lemonchat-v2|%s|%u|%u|%s|%u", nonce, policy->exchanges, policy->layers, ciphers__name(policy->cipher), policy->random_layer ? 1u : 0u);
    return size > 0 && (size_t)size < sizeof(header) && keys_session__mix(transcript, (const unsigned char *)header, (size_t)size) && keys_session__mix(transcript, (const unsigned char *)public_key, strlen(public_key));
}

/**
 * @brief derives separate keys for each direction, cipher and layer
 *
 * @param const unsigned char secret[32] -> combined exchange secret
 * @param const unsigned char transcript[32] -> handshake hash
 * @param int type -> cipher for the configured layers
 * @param unsigned int count -> number of configured layers
 * @param int random_type -> extra cipher, or zero to omit it
 * @param lemon_session_keys* output -> receives the keys. Cleared on failure
 *
 * @return int -> 1 if all keys were derived, otherwise 0
 */
int keys_session__derive(const unsigned char secret[32], const unsigned char transcript[32], int type, unsigned int count, int random_type, lemon_session_keys *output)
{
    char purpose[128];
    unsigned int index;
    int status = 0;

    ciphers__clear(output, sizeof(*output));
    if (count < 1 || count > LEMON_MAX_SESSION_LAYERS || ciphers__name(type) == NULL || (random_type != 0 && ciphers__name(random_type) == NULL))
    {
        return 0;
    }
    output->count = count + (random_type != 0);
    status = keys_session__hkdf(secret, transcript, "lemonchat/v2/client-to-server/mac", output->client_mac) && keys_session__hkdf(secret, transcript, "lemonchat/v2/server-to-client/mac", output->server_mac);
    for (index = 0; status && index < output->count; index++)
    {
        int cipher = index < count ? type : random_type;
        output->client_to_server[index].type = cipher;
        output->server_to_client[index].type = cipher;
        snprintf(purpose, sizeof(purpose), "lemonchat/v2/client-to-server/layer/%u/%s", index, ciphers__name(cipher));
        status = keys_session__hkdf(secret, transcript, purpose, output->client_to_server[index].bytes);
        snprintf(purpose, sizeof(purpose), "lemonchat/v2/server-to-client/layer/%u/%s", index, ciphers__name(cipher));
        status = status && keys_session__hkdf(secret, transcript, purpose, output->server_to_client[index].bytes);
    }
    if (!status)
    {
        ciphers__clear(output, sizeof(*output));
    }
    return status;
}

/**
 * @brief applies the session layers, reversing their order for decryption
 *
 * @param const lemon_session_keys* session -> keys for both traffic directions
 * @param int server_to_client -> nonzero selects incoming server keys
 * @param int encrypt -> nonzero encrypts, zero decrypts
 * @param const unsigned char[16] iv -> message IV shared by the layers
 * @param unsigned char* data -> buffer updated in place
 * @param size_t size -> number of bytes in the buffer
 *
 * @return int -> 1 if every layer succeeds, otherwise 0
 */
int keys_session__crypt(const lemon_session_keys *session, int server_to_client, int encrypt, const unsigned char iv[16], unsigned char *data, size_t size)
{
    const lemon_session_layer *layers = server_to_client ? session->server_to_client : session->client_to_server;
    unsigned int index;

    if (session->count < 1 || session->count > LEMON_MAX_CONNECTION_LAYERS)
    {
        return 0;
    }
    for (index = 0; index < session->count; index++)
    {
        unsigned int selected = encrypt ? index : session->count - index - 1;
        if (!ciphers__crypt(layers[selected].type, layers[selected].bytes, iv, data, size))
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief applies a candidate cipher to the probe for a separate HMAC check
 *
 * @param const unsigned char secret[32] -> combined exchange secret
 * @param const unsigned char transcript[32] -> handshake hash
 * @param int type -> candidate cipher
 * @param const unsigned char iv[16] -> initialization vector
 * @param unsigned char data[32] -> probe updated in place
 *
 * @return int -> 1 if derivation and cipher processing succeeded, otherwise 0
 */
int keys_session__random_probe(const unsigned char secret[32], const unsigned char transcript[32], int type, const unsigned char iv[16], unsigned char data[32])
{
    char purpose[128];
    unsigned char key[32];
    int status = 0;
    const char *name = ciphers__name(type);
    if (name != NULL)
    {
        snprintf(purpose, sizeof(purpose), "lemonchat/v2/random-cipher/probe/%s", name);
        status = keys_session__hkdf(secret, transcript, purpose, key) && ciphers__crypt(type, key, iv, data, 32);
    }
    ciphers__clear(key, sizeof(key));
    return status;
}

/**
 * @brief authenticates the recovered cipher probe and handshake hash
 *
 * @param const unsigned char[32] secret -> combined exchange secret
 * @param const unsigned char[32] transcript -> handshake hash
 * @param const unsigned char[32] plaintext -> recovered probe bytes
 * @param unsigned char[32] tag -> receives the authentication tag
 *
 * @return int -> 1 on success, otherwise 0
 */
int keys_session__random_probe_tag(const unsigned char secret[32], const unsigned char transcript[32], const unsigned char plaintext[32], unsigned char tag[32])
{
    unsigned char key[32];
    unsigned char input[64];
    int status;
    clib__copy_memory(plaintext, input, 32, sizeof(input));
    clib__copy_memory(transcript, input + 32, 32, sizeof(input) - 32);
    status = keys_session__hkdf(secret, transcript, "lemonchat/v2/random-cipher/probe-check", key) && ciphers__hmac(key, input, sizeof(input), tag);
    ciphers__clear(key, sizeof(key));
    ciphers__clear(input, sizeof(input));
    return status;
}

/**
 * @brief derives one key with HKDF using SHA256
 *
 * @param const unsigned char input[32] -> input key material
 * @param const unsigned char salt[32] -> salt
 * @param const char* purpose -> purpose label shorter than 256 bytes
 * @param unsigned char output[32] -> receives the derived key
 *
 * @return int -> 1 if derivation succeeded, otherwise 0
 */
int keys_session__hkdf(const unsigned char input[32], const unsigned char salt[32], const char *purpose, unsigned char output[32])
{
    unsigned char extracted[32];
    unsigned char info[256];
    size_t size = strlen(purpose);
    int status = 0;

    if (size < sizeof(info))
    {
        clib__copy_memory(purpose, info, size, sizeof(info));
        info[size] = 1;
        status = ciphers__hmac(salt, input, 32, extracted) && ciphers__hmac(extracted, info, size + 1, output);
    }
    ciphers__clear(extracted, sizeof(extracted));
    ciphers__clear(info, sizeof(info));
    return status;
}

/**
 * @brief hashes the previous state, field length and field bytes together
 *
 * @param unsigned char state[32] -> hash updated in place
 * @param const unsigned char* data -> field bytes
 * @param size_t size -> field length, at most UINT32_MAX
 *
 * @return int -> 1 if hashing succeeded, otherwise 0
 */
int keys_session__mix(unsigned char state[32], const unsigned char *data, size_t size)
{
    mbedtls_sha256_context context;
    unsigned char length[4];
    int status;

    if (size > UINT32_MAX)
    {
        return 0;
    }
    length[0] = (unsigned char)(size >> 24);
    length[1] = (unsigned char)(size >> 16);
    length[2] = (unsigned char)(size >> 8);
    length[3] = (unsigned char)size;
    mbedtls_sha256_init(&context);
    status = mbedtls_sha256_starts(&context, 0) == 0 && mbedtls_sha256_update(&context, state, 32) == 0 && mbedtls_sha256_update(&context, length, 4) == 0 && mbedtls_sha256_update(&context, data, size) == 0 && mbedtls_sha256_finish(&context, state) == 0;
    mbedtls_sha256_free(&context);
    return status;
}
