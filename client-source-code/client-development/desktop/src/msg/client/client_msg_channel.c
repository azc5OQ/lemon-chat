#include "crypto/crypto.h"
#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief requests a channel move; the server remains responsible for passwords and permissions.
 * @param desktop_session_t* session -> worker-owned connection
 * @param int64 channel -> destination server channel ID
 * @param const char* password -> password for this request only
 * @return boole -> 1 when accepted by transport
 */
boole client_msg__channel__send_join_request(desktop_session_t* session, int64 channel, const char* password)
{
    cJSON* message = client_msg__delivery__new_message("join_channel_request");
    boole complete = message && cJSON_AddNumberToObject(message, "channel_id", (double)channel)
        && cJSON_AddStringToObject(message, "channel_password", password);
    return client_msg__delivery__send(session, message, complete);
}

/**
 * @brief asks the server to recover a channel whose keys have not arrived within the timeout.
 * @param desktop_session_t* session -> worker-owned connection
 * @return boole -> 1 when accepted by transport
 */
boole client_msg__channel__send_reset_maintainer(desktop_session_t* session)
{
    return client_msg__delivery__send(session, client_msg__delivery__new_message("reset_channel_maintainer"), 1);
}

/**
 * @brief sends at most one recipient's keys per tick so keepalives and cancellation remain responsive.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return boole -> 1 on success or when no recipient is due, 0 on failure
 */
boole client_msg__channel__distribute_keys(desktop_session_t* session)
{
    desktop_client_t* client = NULL;
    desktop_content_keys_t direct_keys = {0};
    cJSON* body = NULL;
    cJSON* envelope = NULL;
    cJSON* message = NULL;
    char* keys_json = NULL;
    char* direct_json = NULL;
    char* body_json = NULL;
    char* wrapped_keys = NULL;
    char* encrypted_body = NULL;
    char* envelope_json = NULL;
    boole complete = 0;
    int64 slot = 0;

    if (session->model.idle || session->maintainer != session->model.local_id || session->maintainer < 0)
    {
        return 1;
    }
    if (session->rotate)
    {
        session->previous_keys = session->keys;
        session->have_previous = session->model.keys_ready;
        if (!desktop_crypto__content_keys(session->crypto, &session->keys))
        {
            return 0;
        }
        session->model.keys_ready = 1;
        session->rotate = 0;
        session->distribute = 0;
        desktop_session__detail(session, "Connected. Channel keys are ready.", 0);
    }
    while (session->distribute < session->model.client_count)
    {
        slot = session->distribute++;
        client = &session->model.clients[slot];
        if (client->channel == session->model.channel && client->id != session->model.local_id
            && !client->idle && !client->music && session->client_keys[slot][0])
        {
            break;
        }
        client = NULL;
    }
    if (!client)
    {
        return 1;
    }
    keys_json = desktop_crypto__keys_json(&session->keys);
    if (desktop_crypto__content_keys(session->crypto, &direct_keys))
    {
        direct_json = desktop_crypto__keys_json(&direct_keys);
    }
    body = client_msg__delivery__new_message("channel_keys_from_maintainer");
    complete = keys_json && direct_json && body && cJSON_AddNumberToObject(body, "channel_id", (double)session->model.channel)
        && cJSON_AddNumberToObject(body, "maintainer_generation", (double)session->generation)
        && cJSON_AddStringToObject(body, "value", keys_json);
    body_json = complete ? cJSON_PrintUnformatted(body) : NULL;
    if (body_json)
    {
        wrapped_keys = desktop_crypto__hybrid_encrypt(session->crypto, session->client_keys[slot], direct_json);
        encrypted_body = desktop_crypto__content_encrypt(session->crypto, &direct_keys, body_json);
    }
    envelope = cJSON_CreateObject();
    complete = envelope && wrapped_keys && encrypted_body && cJSON_AddStringToObject(envelope, "message_keys", wrapped_keys)
        && cJSON_AddStringToObject(envelope, "message_value", encrypted_body);
    envelope_json = complete ? cJSON_PrintUnformatted(envelope) : NULL;
    message = client_msg__delivery__new_message("direct_chat_message");
    complete = envelope_json && message && cJSON_AddStringToObject(message, "value", envelope_json)
        && cJSON_AddNullToObject(message, "receiver_type") && cJSON_AddNumberToObject(message, "receiver_id", (double)client->id)
        && cJSON_AddNumberToObject(message, "local_message_id", (double)++session->next_local_id);
    complete = client_msg__delivery__send(session, message, complete);
    desktop_crypto__free_json(body);
    desktop_crypto__free_json(envelope);
    desktop_crypto__free_text(keys_json);
    desktop_crypto__free_text(direct_json);
    desktop_crypto__free_text(body_json);
    desktop_crypto__free_text(wrapped_keys);
    desktop_crypto__free_text(encrypted_body);
    desktop_crypto__free_text(envelope_json);
    mbedtls_platform_zeroize(&direct_keys, sizeof(direct_keys));
    return complete;
}
