#include "crypto/crypto_session.h"
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
boole client_msg__channel__send_join_request(desktop_session_t *session, int64 channel, const char *password)
{
    cJSON *message = client_msg__delivery__new_message("join_channel_request");
    boole complete = message && cJSON_AddNumberToObject(message, "channel_id", (double)channel) && cJSON_AddStringToObject(message, "channel_password", password);
    return client_msg__delivery__send(session, message, complete);
}

/**
 * @brief asks the server to recover a channel whose keys have not arrived within the timeout.
 * @param desktop_session_t* session -> worker-owned connection
 * @return boole -> 1 when accepted by transport
 */
boole client_msg__channel__send_reset_maintainer(desktop_session_t *session)
{
    return client_msg__delivery__send(session, client_msg__delivery__new_message("reset_channel_maintainer"), 1);
}

/**
 * @brief counts people currently using this channel, excluding idle clients and music bots.
 * @param desktop_session_t* session -> worker-owned channel and client state
 * @return int64 -> active people, including the local client
 */
static int64 _client_msg_channel_internal__member_count(desktop_session_t *session)
{
    int64 count = 0;
    int64 index = 0;
    desktop_client_t *client = NULL_POINTER;

    // count active channel members
    for (index = 0; index < session->model.client_count; index++)
    {
        client = &session->model.clients[index];
        if (client->channel == session->model.channel && !client->idle && !client->music)
        {
            count++;
        }
    }
    return count;
}

/**
 * @brief checks the server's permission and size limit after a membership change.
 * @param desktop_session_t* session -> worker-owned channel and policy state
 * @param boole joining -> TRUE for an arrival, FALSE for a departure
 * @return boole -> TRUE when existing keys may remain in use
 */
static boole _client_msg_channel_internal__may_reuse(desktop_session_t *session, boole joining)
{
    int64 threshold = joining ? session->key_reuse_join_min_clients : session->key_reuse_leave_min_clients;
    boole allowed = joining ? session->key_reuse_on_join : session->key_reuse_on_leave;

    return session->key_reuse_enabled && allowed && threshold >= 30 && threshold <= 1000000 && _client_msg_channel_internal__member_count(session) >= threshold;
}

/**
 * @brief removes unfinished delivery for a person who left or changed identity.
 * @param desktop_session_t* session -> worker-owned recipient queue
 * @param int64 id -> server client ID to remove
 * @return void
 */
static void _client_msg_channel_internal__remove_recipient(desktop_session_t *session, int64 id)
{
    int64 index = 0;

    // remove matching recipients
    while (index < session->key_recipient_count)
    {
        if (session->key_recipients[index].id == id)
        {
            session->key_recipient_count--;
            clib__memmove(&session->key_recipients[index], &session->key_recipients[index + 1], (session->key_recipient_count - index) * sizeof(session->key_recipients[0]));
        }
        else
        {
            index++;
        }
    }
}

/**
 * @brief queues one current identity without duplicating arrivals already waiting for keys.
 * @param desktop_session_t* session -> worker-owned recipient queue
 * @param const desktop_client_t* client -> current channel member
 * @return void
 */
static void _client_msg_channel_internal__queue_recipient(desktop_session_t *session, const desktop_client_t *client)
{
    int64 index = 0;

    // only active channel members
    if (client->id == session->model.local_id || client->channel != session->model.channel || client->idle || client->music)
    {
        return;
    }

    // update an existing entry
    for (index = 0; index < session->key_recipient_count; index++)
    {
        if (session->key_recipients[index].id == client->id)
        {
            session->key_recipients[index].serial = client->serial;
            return;
        }
    }

    // add the recipient
    if (session->key_recipient_count < DESKTOP_SESSION_CLIENTS)
    {
        session->key_recipients[session->key_recipient_count].id = client->id;
        session->key_recipients[session->key_recipient_count].serial = client->serial;
        session->key_recipient_count++;
    }
}

/**
 * @brief cancels unfinished delivery and requests new keys from the current maintainer.
 * @param desktop_session_t* session -> worker-owned connection
 * @return void
 */
void client_msg__channel__refresh_keys(desktop_session_t *session)
{
    // keep the previous keys
    if (session->model.keys_ready)
    {
        session->previous_keys = session->keys;
        session->have_previous = TRUE;
    }

    // reset the key state
    session->model.keys_ready = FALSE;
    session->key_recipient_count = 0;
    session->rotate = session->model.connected && !session->model.idle && session->maintainer >= 0 && session->maintainer == session->model.local_id;
    session->waiting_since = desktop_system__milliseconds();
}

/**
 * @brief applies join and departure rules after the client list has been updated.
 * @param desktop_session_t* session -> worker-owned channel and current client list
 * @param const desktop_client_t* previous -> previous member state, or NULL for a new connection
 * @param const desktop_client_t* current -> current member state, or NULL for a disconnection
 * @return void
 */
void client_msg__channel__member_changed(desktop_session_t *session, const desktop_client_t *previous, const desktop_client_t *current)
{
    boole was_member = previous && previous->channel == session->model.channel && !previous->idle && !previous->music;
    boole is_member = current && current->channel == session->model.channel && !current->idle && !current->music;
    boole identity_changed = previous && current && previous->serial != current->serial;
    int64 id = current ? current->id : (previous ? previous->id : -1);

    // skip unrelated changes
    if (id < 0 || id == session->model.local_id || (!was_member && !is_member))
    {
        return;
    }

    // apply the departure rules
    if (was_member && (!is_member || identity_changed))
    {
        // remove from the queue
        _client_msg_channel_internal__remove_recipient(session, id);

        // the maintainer left
        if (id == session->maintainer)
        {
            session->maintainer = -1;
            client_msg__channel__refresh_keys(session);
            return;
        }

        // refresh unless reuse allowed
        if (identity_changed || !_client_msg_channel_internal__may_reuse(session, FALSE))
        {
            client_msg__channel__refresh_keys(session);
        }
    }

    // apply the join rules
    if (is_member && (!was_member || identity_changed))
    {
        if (identity_changed || !session->model.keys_ready || !_client_msg_channel_internal__may_reuse(session, TRUE))
        {
            client_msg__channel__refresh_keys(session);
        }
        else if (session->maintainer == session->model.local_id && session->maintainer >= 0 && !session->model.idle)
        {
            _client_msg_channel_internal__queue_recipient(session, current);
        }
    }
}

/**
 * @brief sends at most one recipient's keys per tick so keepalives and cancellation remain responsive.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @return boole -> 1 on success or when no recipient is due, 0 on failure
 */
boole client_msg__channel__distribute_keys(desktop_session_t *session)
{
    desktop_client_t *client = NULL_POINTER;
    desktop_content_keys_t direct_keys = { 0 };
    cJSON *body = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    char *keys_json = NULL_POINTER;
    char *body_json = NULL_POINTER;
    char *encrypted_body = NULL_POINTER;
    char *envelope_json = NULL_POINTER;
    boole complete = 0;
    int64 slot = 0;
    int64 index = 0;
    desktop_channel_key_recipient_t recipient = { 0 };

    // check the maintainer role
    if (!session->model.connected || session->model.idle || session->maintainer != session->model.local_id || session->maintainer < 0)
    {
        session->rotate = FALSE;
        session->key_recipient_count = 0;
        return 1;
    }

    // rotate the channel keys
    if (session->rotate)
    {
        // keep the previous keys
        if (session->model.keys_ready)
        {
            session->previous_keys = session->keys;
            session->have_previous = TRUE;
        }

        // create new content keys
        if (!crypto_session__content_keys(session->crypto, &session->keys))
        {
            return 0;
        }

        // mark the keys ready
        session->model.keys_ready = 1;
        session->rotate = 0;
        session->key_recipient_count = 0;

        // queue all recipients
        for (index = 0; index < session->model.client_count; index++)
        {
            _client_msg_channel_internal__queue_recipient(session, &session->model.clients[index]);
        }

        // show the status text
        desktop_session__detail(session, "Connected. Channel keys are ready.", 0);
    }

    // find the next recipient
    while (session->model.keys_ready && session->key_recipient_count > 0)
    {
        // take the first recipient
        recipient = session->key_recipients[0];
        session->key_recipient_count--;
        clib__memmove(session->key_recipients, session->key_recipients + 1, session->key_recipient_count * sizeof(session->key_recipients[0]));

        // find the client slot
        slot = desktop_session__client(session, recipient.id);
        if (slot < 0)
        {
            continue;
        }

        // check the client state
        client = &session->model.clients[slot];
        if (client->serial == recipient.serial && client->channel == session->model.channel && client->id != session->model.local_id && !client->idle && !client->music && session->client_keys[slot][0])
        {
            break;
        }
        client = NULL;
    }

    // nothing to send
    if (!client)
    {
        return 1;
    }

    // build the keys body
    keys_json = crypto_session__keys_json(&session->keys);
    complete = crypto_session__content_keys(session->crypto, &direct_keys);
    body = client_msg__delivery__new_message("channel_keys_from_maintainer");
    complete = complete && keys_json && body && cJSON_AddNumberToObject(body, "channel_id", (double)session->model.channel) && cJSON_AddNumberToObject(body, "maintainer_generation", (double)session->generation) && cJSON_AddStringToObject(body, "value", keys_json);
    body_json = complete ? cJSON_PrintUnformatted(body) : NULL;

    // encrypt the body
    if (body_json)
    {
        encrypted_body = crypto_session__direct_content_encrypt(&direct_keys, body_json, 1024, FALSE);
    }

    // build the envelope
    if (encrypted_body)
    {
        envelope_json = crypto_session__direct_seal(session->crypto, session->client_keys[slot], &direct_keys, encrypted_body);
    }

    // build the direct message
    message = client_msg__delivery__new_message("direct_chat_message");
    complete = envelope_json && message && cJSON_AddStringToObject(message, "purpose", "channel_keys") && cJSON_AddStringToObject(message, "value", envelope_json) && cJSON_AddNullToObject(message, "receiver_type") && cJSON_AddNumberToObject(message, "receiver_id", (double)client->id) && cJSON_AddNumberToObject(message, "local_message_id", (double)++session->next_local_id);

    // send the message
    complete = client_msg__delivery__send(session, message, complete);

    // free the buffers
    crypto_session__free_json(body);
    crypto_session__free_text(keys_json);
    crypto_session__free_text(body_json);
    crypto_session__free_text(encrypted_body);
    crypto_session__free_text(envelope_json);

    // erase the direct keys
    mbedtls_platform_zeroize(&direct_keys, sizeof(direct_keys));
    return complete;
}
