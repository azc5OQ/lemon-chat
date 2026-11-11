#include "auth_handshake.h"
#include "keys_session.h"
#include "ciphers.h"
#include "../base.h"
#include "../util.h"
#include "crypto_metadata.h"
#include "../msg/server/server_msg.h"
#include <string.h>

static void auth_handshake__hex(const ubyte *bytes, uint64 size, char *output)
{
    uint64 index = 0;
    static const char digits[] = "0123456789abcdef";

    // write the hex digits
    for (index = 0; index < size; index++)
    {
        output[index * 2] = digits[bytes[index] >> 4];
        output[index * 2 + 1] = digits[bytes[index] & 15];
    }

    // end the string
    output[size * 2] = 0;
}

// Caller holds the clients lock. -1 rejects; 0 is legacy; 1 is negotiated;
// 2 means a policy reply was sent and the client must submit its fresh mixes.
int64 auth_handshake__prepare(client_t *client, cJSON *message)
{
    cJSON *root = NULL_POINTER;
    cJSON *reply = NULL_POINTER;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "crypto_version");
    cJSON *nonce = cJSON_GetObjectItemCaseSensitive(message, "crypto_nonce");
    cJSON *mixes = cJSON_GetObjectItemCaseSensitive(message, "dh_public_mixes");
    cJSON *mix = NULL_POINTER;
    ubyte random[16] = { 0 };
    uint64 length = 0;
    uint64 index = 0;

    // old protocol without version
    if (version == NULL_POINTER)
    {
        return client->crypto_policy_nonce[0] == 0 && g_server_settings.dh_exchange_count == 1 && g_server_settings.session_encryption_layer_count == 1 && g_server_settings.session_cipher == CRYPTO_KEY_AES_256_CTR && g_server_settings.is_random_cipher_layer_enabled == FALSE ? 0 : -1;
    }

    // check the version
    if (!cJSON_IsNumber(version) || version->valuedouble != 2)
    {
        return -1;
    }

    // message without nonce
    if (nonce == NULL_POINTER)
    {
        // create the nonce
        if (client->crypto_policy_nonce[0] != 0 || !util__fill_secure_random_bytes(random, sizeof(random)))
        {
            return -1;
        }
        auth_handshake__hex(random, sizeof(random), client->crypto_policy_nonce);
        ciphers__clear(random, sizeof(random));

        // store the policy
        client->crypto_policy.exchanges = (uint64)g_server_settings.dh_exchange_count;
        client->crypto_policy.layers = (uint64)g_server_settings.session_encryption_layer_count;
        client->crypto_policy.cipher = g_server_settings.session_cipher;
        client->crypto_policy.random_layer = g_server_settings.is_random_cipher_layer_enabled;

        // build the answer
        root = server_msg__delivery__new_message("crypto_policy", &reply);
        if (root == NULL_POINTER || reply == NULL_POINTER || !cJSON_AddNumberToObject(reply, "version", 2) || !cJSON_AddStringToObject(reply, "nonce", client->crypto_policy_nonce) || !cJSON_AddNumberToObject(reply, "dh_exchange_count", client->crypto_policy.exchanges) || !cJSON_AddNumberToObject(reply, "session_encryption_layer_count", client->crypto_policy.layers) || !cJSON_AddStringToObject(reply, "session_cipher", ciphers__name(client->crypto_policy.cipher)) || !cJSON_AddBoolToObject(reply, "is_random_cipher_layer_enabled", client->crypto_policy.random_layer))
        {
            cJSON_Delete(root);
            return -1;
        }

        // send the answer
        server_msg__delivery__send_json(client, NULL_POINTER, root);
        cJSON_Delete(root);
        return 2;
    }

    // check nonce and mixes
    if (!cJSON_IsString(nonce) || strlen(nonce->valuestring) != 32 || client->crypto_policy_nonce[0] == 0 || !crypto_metadata__equal((const ubyte *)nonce->valuestring, (const ubyte *)client->crypto_policy_nonce, 32) || !cJSON_IsArray(mixes) || (uint64)cJSON_GetArraySize(mixes) != client->crypto_policy.exchanges)
    {
        return -1;
    }

    // check every mix
    cJSON_ArrayForEach(mix, mixes)
    {
        // check the type
        if (!cJSON_IsString(mix))
        {
            return -1;
        }

        // check the length
        length = strlen(mix->valuestring);
        if (length < 1 || length >= SHARED_SECRET_LENGTH)
        {
            return -1;
        }

        // allow digits only
        for (index = 0; index < length; index++)
        {
            if (mix->valuestring[index] < '0' || mix->valuestring[index] > '9')
            {
                return -1;
            }
        }
    }

    // compare the first mix
    mix = cJSON_GetObjectItemCaseSensitive(message, "dh_public_mix");
    return cJSON_IsString(mix) && strcmp(mix->valuestring, cJSON_GetArrayItem(mixes, 0)->valuestring) == 0 ? 1 : -1;
}

boole auth_handshake__finish(const lemon_session_policy *policy, const ubyte secret[32], const ubyte transcript[32], crypto_session_keys_t *session, cJSON *reply)
{
    static const int64 candidates[] = { LEMON_AES256, LEMON_CHACHA20, LEMON_CAMELLIA256, LEMON_TWOFISH256, LEMON_SERPENT256, LEMON_TRIVIUM80 };
    ubyte selection = 0;
    ubyte probe[32] = { 0 };
    ubyte iv[16] = { 0 };
    ubyte tag[32] = { 0 };
    char hex[65] = { 0 };
    cJSON *random_probe = NULL_POINTER;
    int64 cipher = 0;
    boole status = FALSE;

    // clear the session keys
    crypto_metadata__clear_session_keys(session);

    // add the random layer
    if (policy->random_layer)
    {
        // choose a random cipher
        do
        {
            if (!util__fill_secure_random_bytes(&selection, 1))
            {
                goto done;
            }
        } while (selection >= 256 - (256 % (sizeof(candidates) / sizeof(candidates[0]))));
        cipher = candidates[selection % (sizeof(candidates) / sizeof(candidates[0]))];

        // create the encrypted probe
        if (!util__fill_secure_random_bytes(probe, sizeof(probe)) || !util__fill_secure_random_bytes(iv, sizeof(iv)) || !keys_session__random_probe_tag(secret, transcript, probe, tag) || !keys_session__random_probe(secret, transcript, cipher, iv, probe))
        {
            goto done;
        }

        // add the probe IV
        random_probe = cJSON_AddObjectToObject(reply, "random_probe");
        auth_handshake__hex(iv, sizeof(iv), hex);
        if (random_probe == NULL_POINTER || !cJSON_AddStringToObject(random_probe, "iv", hex))
        {
            goto done;
        }

        // add the probe data
        auth_handshake__hex(probe, sizeof(probe), hex);
        if (!cJSON_AddStringToObject(random_probe, "data", hex))
        {
            goto done;
        }

        // add the probe tag
        auth_handshake__hex(tag, sizeof(tag), hex);
        if (!cJSON_AddStringToObject(random_probe, "tag", hex))
        {
            goto done;
        }
    }

    // create the session keys
    if (!keys_session__derive(secret, transcript, policy->cipher, policy->layers, cipher, &session->negotiated) || !crypto_metadata__key_init(&session->authentication, CRYPTO_KEY_HMAC_SHA256, session->negotiated.server_mac, 32) || !crypto_metadata__key_init(&session->encryption, policy->cipher, session->negotiated.server_to_client[0].bytes, 32) || !ciphers__hmac(session->negotiated.server_mac, transcript, 32, tag))
    {
        goto done;
    }

    // add version and tag
    auth_handshake__hex(tag, sizeof(tag), hex);
    if (!cJSON_AddNumberToObject(reply, "version", 2) || !cJSON_AddStringToObject(reply, "finished", hex))
    {
        goto done;
    }

    // mark the session ready
    session->is_ready = TRUE;
    status = 1;
done:
    // clear the buffers
    ciphers__clear(probe, sizeof(probe));
    ciphers__clear(iv, sizeof(iv));
    ciphers__clear(tag, sizeof(tag));

    // clear keys on failure
    if (!status)
    {
        crypto_metadata__clear_session_keys(session);
    }
    return status;
}
