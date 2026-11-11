#include "keys_session.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"

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

/** @brief Derives independent keys per direction, cipher and layer from all exchanges. */
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

/** @brief Encrypts/decrypts one candidate probe; a separate HMAC verifies its plaintext. */
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

boole keys_session__random_probe_tag(const ubyte secret[32], const ubyte transcript[32], const ubyte plaintext[32], ubyte tag[32])
{
    ubyte key[32] = { 0 };
    ubyte input[64] = { 0 };
    boole status = FALSE;

    // join plaintext and transcript
    memcpy(input, plaintext, 32);
    memcpy(input + 32, transcript, 32);

    // calculate the tag
    status = keys_session__hkdf(secret, transcript, "lemonchat/v2/random-cipher/probe-check", key) && ciphers__hmac(key, input, sizeof(input), tag);

    // clear the buffers
    ciphers__clear(key, sizeof(key));
    ciphers__clear(input, sizeof(input));
    return status;
}

/** @brief HKDF-SHA256 extract/expand for one 32-byte output, with a bounded purpose label. */
boole keys_session__hkdf(const ubyte input[32], const ubyte salt[32], const char *purpose, ubyte output[32])
{
    ubyte extracted[32] = { 0 };
    ubyte info[256] = { 0 };
    uint64 size = strlen(purpose);
    boole status = FALSE;

    // calculate the output key
    if (size < sizeof(info))
    {
        memcpy(info, purpose, size);
        info[size] = 1;
        status = ciphers__hmac(salt, input, 32, extracted) && ciphers__hmac(extracted, info, size + 1, output);
    }

    // clear the buffers
    ciphers__clear(extracted, sizeof(extracted));
    ciphers__clear(info, sizeof(info));
    return status;
}

/** @brief Hash-chains a length-prefixed field; used for secrets and the handshake transcript. */
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
