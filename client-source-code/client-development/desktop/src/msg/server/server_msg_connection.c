#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief verifies the RSA challenge and server DH mix before sending any response.
 * @param desktop_session_t* session -> worker-owned connection and local identity
 * @param const cJSON* message -> decoded public_key_challenge payload
 * @return boole -> 1 when the response was accepted by transport
 */
boole server_msg__connection__process_challenge(desktop_session_t* session, const cJSON* message)
{
    char challenge[1025] = {0};
    boole complete = desktop_crypto__challenge(session->crypto, server_msg__read_string(message, "value"), challenge, sizeof(challenge))
        && desktop_crypto__finish_dh(session->crypto, server_msg__read_string(message, "dh_public_mix"));
    if (complete)
    {
        complete = client_msg__connection__send_challenge_response(session, challenge);
    }
    else
    {
        desktop_session__detail(session, "The identity challenge or server key exchange is invalid.", 1);
    }
    mbedtls_platform_zeroize(challenge, sizeof(challenge));
    return complete;
}

/**
 * @brief validates and inserts one server client record; identity is its existing public key.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const cJSON* data -> borrowed server state record
 * @return boole -> 1 when the bounded state accepted this client
 */
boole server_msg__connection__read_client(desktop_session_t* session, const cJSON* data)
{
    desktop_client_t client = {0};
    const char* public_key = server_msg__read_string(data, "public_key");
    int64 slot = -1;
    int64 audio = server_msg__read_number(data, "audio_state");

    client.id = server_msg__read_number(data, "client_id");
    client.channel = server_msg__read_number(data, "channel_id");
    client.audio = audio >= 1 && audio <= 4 ? (int)audio : 3;
    client.idle = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_idle"));
    client.music = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "is_music_bot"));
    server_msg__tree__read_client(&client, data);
    slot = desktop_session__client(session, client.id);
    if (client.id < 0 || !clib__string_copy(client.name, sizeof(client.name), server_msg__read_string(data, "username"))
        || clib__strlen(public_key) >= sizeof(session->client_keys[0])
        || (slot < 0 && session->model.client_count == DESKTOP_SESSION_CLIENTS))
    {
        return 0;
    }
    if (slot < 0)
    {
        slot = (int)session->model.client_count++;
        client.serial = ++session->next_client_serial;
    }
    else if (clib__strcmp(session->client_keys[slot], public_key) == 0)
    {
        client.serial = session->model.clients[slot].serial;
        client.muted = session->model.clients[slot].muted;
        client.ignored = session->model.clients[slot].ignored;
        client.volume_offset = session->model.clients[slot].volume_offset;
        client.direct_received = session->model.clients[slot].direct_received;
    }
    else
    {
        client.serial = ++session->next_client_serial;
    }
    session->model.clients[slot] = client;
    clib__string_copy(session->client_keys[slot], sizeof(session->client_keys[slot]), public_key);
    if (public_key[0] && clib__strcmp(public_key, session->public_key) == 0)
    {
        session->model.local_id = client.id;
        session->model.idle = client.idle;
        if (session->model.channel != client.channel)
        {
            session->model.channel = client.channel;
            desktop_session__keys_reset(session);
        }
    }
    return 1;
}
