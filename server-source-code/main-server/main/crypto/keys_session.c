#include "keys_session.h"
#include "../clib/clib.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"

/**
 * @brief starts the transcript with policy, nonce and client public key
 *
 * @param ubyte [32] transcript -> receives the initial 32 byte transcript hash
 * @param const lemon_session_policy * policy -> session policy to validate and include
 * @param const char * nonce -> 32 character negotiation nonce
 * @param const char * public_key -> terminated client public key
 *
 * @return boole -> TRUE when the policy is valid and both transcript inputs are mixed
 */
boole keys_session__begin_transcript(ubyte transcript[32], const lemon_session_policy *policy, const char *nonce, const char *public_key)
{
    char header[256] = { 0 };
    int64 size = 0;

    // check policy and nonce
    if (policy->exchanges < 1 || policy->exchanges > LEMON_MAX_DH_EXCHANGES || policy->layers < 1 || policy->layers > LEMON_MAX_SESSION_LAYERS || ciphers__name(policy->cipher) == NULL || strlen(nonce) != 32)
    {
        return 0;
    }

    // clear the transcript
    memset(transcript, 0, 32);

    // add header and key
    size = snprintf(header, sizeof(header), "lemonchat-v2|%s|%llu|%llu|%s|%u", nonce, policy->exchanges, policy->layers, ciphers__name(policy->cipher), policy->random_layer ? 1u : 0u);
    return size > 0 && (uint64)size < sizeof(header) && keys_session__mix(transcript, (const ubyte *)header, (uint64)size) && keys_session__mix(transcript, (const ubyte *)public_key, strlen(public_key));
}

/**
 * @brief derives separate keys for each direction and cipher layer
 *
 * @param const ubyte* secret -> combined exchange secret containing 32 bytes
 * @param const ubyte* transcript -> handshake hash containing 32 bytes
 * @param int64 type -> cipher for the required layers
 * @param uint64 count -> number of required layers
 * @param int64 random_type -> extra cipher, or zero to omit the extra layer
 * @param lemon_session_keys* output -> receives the keys. Cleared on failure
 *
 * @return boole -> TRUE if every key was derived successfully
 */
boole keys_session__derive(const ubyte secret[32], const ubyte transcript[32], int64 type, uint64 count, int64 random_type, lemon_session_keys *output)
{
    int64 cipher = 0;
    char purpose[128] = { 0 };
    uint64 index = 0;
    boole status = FALSE;

    // clear the output
    ciphers__clear(output, sizeof(*output));

    // check arguments
    if (count < 1 || count > LEMON_MAX_SESSION_LAYERS || ciphers__name(type) == NULL || (random_type != 0 && ciphers__name(random_type) == NULL))
    {
        return 0;
    }

    // set the layer count
    output->count = count + (random_type != 0);

    // create the MAC keys
    status = keys_session__hkdf(secret, transcript, "lemonchat/v2/client-to-server/mac", output->client_mac) && keys_session__hkdf(secret, transcript, "lemonchat/v2/server-to-client/mac", output->server_mac);

    // create the layer keys
    for (index = 0; status && index < output->count; index++)
    {
        cipher = index < count ? type : random_type;
        output->client_to_server[index].type = cipher;
        output->server_to_client[index].type = cipher;
        snprintf(purpose, sizeof(purpose), "lemonchat/v2/client-to-server/layer/%llu/%s", index, ciphers__name(cipher));
        status = keys_session__hkdf(secret, transcript, purpose, output->client_to_server[index].bytes);
        snprintf(purpose, sizeof(purpose), "lemonchat/v2/server-to-client/layer/%llu/%s", index, ciphers__name(cipher));
        status = status && keys_session__hkdf(secret, transcript, purpose, output->server_to_client[index].bytes);
    }

    // clear output on failure
    if (!status)
    {
        ciphers__clear(output, sizeof(*output));
    }
    return status;
}

/**
 * @brief applies all directional session cipher layers in place
 *
 * @param const lemon_session_keys * session -> derived session layer keys
 * @param boole server_to_client -> TRUE for server to client keys, otherwise client to server keys
 * @param boole encrypt -> TRUE to encrypt or FALSE to decrypt
 * @param const ubyte [16] iv -> message initialization vector
 * @param ubyte * data -> bytes to transform
 * @param uint64 size -> byte count
 *
 * @return boole -> TRUE when the layer count and every cipher operation succeed
 */
boole keys_session__crypt(const lemon_session_keys *session, boole server_to_client, boole encrypt, const ubyte iv[16], ubyte *data, uint64 size)
{
    uint64 selected = 0;
    const lemon_session_layer *layers = server_to_client ? session->server_to_client : session->client_to_server;
    uint64 index = 0;

    // check the layer count
    if (session->count < 1 || session->count > LEMON_MAX_CONNECTION_LAYERS)
    {
        return 0;
    }

    // apply every layer
    for (index = 0; index < session->count; index++)
    {
        selected = encrypt ? index : session->count - index - 1;
        if (!ciphers__crypt(layers[selected].type, layers[selected].bytes, iv, data, size))
        {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief encrypts or decrypts a probe with the selected candidate cipher
 *
 * @param const ubyte* secret -> combined exchange secret containing 32 bytes
 * @param const ubyte* transcript -> handshake hash containing 32 bytes
 * @param int64 type -> candidate cipher identifier
 * @param const ubyte* iv -> starting value containing 16 bytes
 * @param ubyte* data -> probe buffer containing 32 bytes. Transformed in place
 *
 * @return boole -> TRUE if key derivation and the cipher operation succeeded
 */
boole keys_session__random_probe(const ubyte secret[32], const ubyte transcript[32], int64 type, const ubyte iv[16], ubyte data[32])
{
    char purpose[128] = { 0 };
    ubyte key[32] = { 0 };
    boole status = FALSE;
    const char *name = ciphers__name(type);

    // apply cipher to probe
    if (name != NULL)
    {
        snprintf(purpose, sizeof(purpose), "lemonchat/v2/random-cipher/probe/%s", name);
        status = keys_session__hkdf(secret, transcript, purpose, key) && ciphers__crypt(type, key, iv, data, 32);
    }

    // clear the key
    ciphers__clear(key, sizeof(key));
    return status;
}

/**
 * @brief authenticates a cipher probe together with the handshake transcript
 *
 * @param const ubyte [32] secret -> combined 32 byte shared secret
 * @param const ubyte [32] transcript -> 32 byte transcript hash
 * @param const ubyte [32] plaintext -> 32 byte unencrypted probe
 * @param ubyte [32] tag -> receives the 32 byte authentication tag
 *
 * @return boole -> TRUE when key derivation and authentication succeed
 */
boole keys_session__random_probe_tag(const ubyte secret[32], const ubyte transcript[32], const ubyte plaintext[32], ubyte tag[32])
{
    ubyte key[32] = { 0 };
    ubyte input[64] = { 0 };
    boole status = FALSE;

    // join plaintext and transcript
    clib__copy_memory(plaintext, input, 32, sizeof(input));
    clib__copy_memory(transcript, input + 32, 32, sizeof(input) - 32);

    // calculate the tag
    status = keys_session__hkdf(secret, transcript, "lemonchat/v2/random-cipher/probe-check", key) && ciphers__hmac(key, input, sizeof(input), tag);

    // clear the buffers
    ciphers__clear(key, sizeof(key));
    ciphers__clear(input, sizeof(input));
    return status;
}

/**
 * @brief derives one key with HKDF SHA256
 *
 * @param const ubyte* input -> secret containing 32 bytes
 * @param const ubyte* salt -> salt containing 32 bytes
 * @param const char* purpose -> terminated purpose label shorter than 256 bytes
 * @param ubyte* output -> buffer receiving the 32 byte key
 *
 * @return boole -> TRUE if the key was derived successfully
 */
boole keys_session__hkdf(const ubyte input[32], const ubyte salt[32], const char *purpose, ubyte output[32])
{
    ubyte extracted[32] = { 0 };
    ubyte info[256] = { 0 };
    uint64 size = strlen(purpose);
    boole status = FALSE;

    // calculate the output key
    if (size < sizeof(info))
    {
        clib__copy_memory(purpose, info, size, sizeof(info));
        info[size] = 1;
        status = ciphers__hmac(salt, input, 32, extracted) && ciphers__hmac(extracted, info, size + 1, output);
    }

    // clear the buffers
    ciphers__clear(extracted, sizeof(extracted));
    ciphers__clear(info, sizeof(info));
    return status;
}

/**
 * @brief hashes the current state with a field's length and contents
 *
 * @param ubyte* state -> current 32 byte hash. Receives the updated hash
 * @param const ubyte* data -> field bytes to include
 * @param uint64 size -> field length, at most UINT32_MAX bytes
 *
 * @return boole -> TRUE if the hash was updated successfully
 */
boole keys_session__mix(ubyte state[32], const ubyte *data, uint64 size)
{
    mbedtls_sha256_context context = { 0 };
    ubyte length[4] = { 0 };
    boole status = FALSE;

    // check the size
    if (size > UINT32_MAX)
    {
        return 0;
    }

    // write the length bytes
    length[0] = (ubyte)(size >> 24);
    length[1] = (ubyte)(size >> 16);
    length[2] = (ubyte)(size >> 8);
    length[3] = (ubyte)size;

    // hash state, length, data
    mbedtls_sha256_init(&context);
    status = mbedtls_sha256_starts(&context, 0) == 0 && mbedtls_sha256_update(&context, state, 32) == 0 && mbedtls_sha256_update(&context, length, 4) == 0 && mbedtls_sha256_update(&context, data, size) == 0 && mbedtls_sha256_finish(&context, state) == 0;
    mbedtls_sha256_free(&context);
    return status;
}
