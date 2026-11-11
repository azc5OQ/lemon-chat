#include "clib.h"
#include "attachments/attachments_internal.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief reads channel key reuse settings at login and when the operator changes them.
 * @param desktop_session_t* session -> worker-owned connection and policy
 * @param const cJSON* message -> authentication_status or server_policy message
 * @return void
 */
void server_msg__channel__read_key_policy(desktop_session_t *session, const cJSON *message)
{
    const char *type = server_msg__read_string(message, "type");
    const char *flags[3] = { "is_channel_key_reuse_enabled", "is_channel_key_reuse_on_join_allowed", "is_channel_key_reuse_on_leave_allowed" };
    const char *limits[2] = { "channel_key_reuse_join_min_clients", "channel_key_reuse_leave_min_clients" };
    boole *values[3] = { &session->key_reuse_enabled, &session->key_reuse_on_join, &session->key_reuse_on_leave };
    int64 *thresholds[2] = { &session->key_reuse_join_min_clients, &session->key_reuse_leave_min_clients };
    boole previous_flags[3] = { session->key_reuse_enabled, session->key_reuse_on_join, session->key_reuse_on_leave };
    int64 previous_limits[2] = { session->key_reuse_join_min_clients, session->key_reuse_leave_min_clients };
    const cJSON *value = NULL_POINTER;
    int64 number = 0;
    int64 index = 0;
    boole changed = FALSE;
    boole authentication = clib__strcmp(type, "authentication_status") == 0;

    // check the message type
    if (!authentication && clib__strcmp(type, "server_policy") != 0)
    {
        return;
    }

    // use the default values
    if (authentication)
    {
        session->key_reuse_enabled = TRUE;
        session->key_reuse_on_join = TRUE;
        session->key_reuse_on_leave = TRUE;
        session->key_reuse_join_min_clients = 50;
        session->key_reuse_leave_min_clients = 100;
    }

    // apply valid announced values
    for (index = 0; index < 3; index++)
    {
        value = cJSON_GetObjectItemCaseSensitive(message, flags[index]);
        if (cJSON_IsBool(value))
        {
            *values[index] = cJSON_IsTrue(value);
        }
        changed |= previous_flags[index] != *values[index];
    }

    // read the client thresholds
    for (index = 0; index < 2; index++)
    {
        number = server_msg__read_number(message, limits[index]);
        if (number >= 30 && number <= 1000000)
        {
            *thresholds[index] = number;
        }
        changed |= previous_limits[index] != *thresholds[index];
    }

    // apply changed rules to keys already in use
    if (changed)
    {
        client_msg__channel__refresh_keys(session);
    }
}

/**
 * @brief updates worker-owned state from one authenticated server message.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const cJSON* message -> borrowed authenticated server message
 * @return void
 */
void server_msg__dispatch(desktop_session_t *session, const cJSON *message)
{
    const char *type = server_msg__read_string(message, "type");
    char refusal[160] = { 0 };
    const cJSON *items = NULL_POINTER;
    const cJSON *item = NULL_POINTER;
    int64 id = server_msg__read_number(message, "client_id");
    int64 channel = server_msg__read_number(message, "channel_id");
    int64 slot = desktop_session__client(session, id);
    int64 index = 0;
    int64 audio = -1;
    int64 maintainer = -1;
    int64 generation = -1;
    desktop_client_t previous = { 0 };
    boole had_client = slot >= 0;
    char *plaintext = NULL_POINTER;
    char *encrypted_body = NULL_POINTER;
    boole valid = 1;
    boole client_list = clib__strcmp(type, "client_list") == 0;

    // copy the previous client
    if (had_client)
    {
        previous = session->model.clients[slot];
    }

    // call the receive functions
    server_msg__sounds__receive(session, message);
    desktop_voice__message(session, message);
    server_msg__actions__receive(session, message);
    server_msg__attachments__receive(session, message);

    // choose by message type
    if (clib__strcmp(type, "client_list") == 0 || clib__strcmp(type, "channel_list") == 0)
    {
        // read the list array
        items = cJSON_GetObjectItemCaseSensitive(message, client_list ? "clients" : "channels");
        valid = cJSON_IsArray(items);

        // clear the stored list
        if (client_list)
        {
            session->model.client_count = 0;
        }
        else
        {
            session->model.channel_count = 0;
        }

        // read every item
        cJSON_ArrayForEach(item, items)
        {
            valid = valid && (client_list ? server_msg__connection__read_client(session, item) : server_msg__channel__read_channel(session, item));
        }

        // request new keys
        if (client_list && session->maintainer >= 0)
        {
            client_msg__channel__refresh_keys(session);
        }
    }
    else if (clib__strcmp(type, "server_policy") == 0 || clib__strcmp(type, "authentication_status") == 0)
    {
        server_msg__channel__read_key_policy(session, message);
    }
    else if (clib__strcmp(type, "client_connect") == 0)
    {
        // add the connected client
        valid = server_msg__connection__read_client(session, message);
        slot = desktop_session__client(session, id);
        if (valid && slot >= 0)
        {
            client_msg__channel__member_changed(session, had_client ? &previous : NULL, &session->model.clients[slot]);
        }
    }
    else if (clib__strcmp(type, "client_disconnect") == 0 && slot >= 0)
    {
        // remove the client
        session->model.client_count--;
        session->model.clients[slot] = session->model.clients[session->model.client_count];
        clib__memcpy(session->client_keys[slot], session->client_keys[session->model.client_count], sizeof(session->client_keys[slot]));
        clib__null_memory(&session->model.clients[session->model.client_count], sizeof(session->model.clients[0]));
        clib__null_memory(session->client_keys[session->model.client_count], sizeof(session->client_keys[0]));

        // apply the member change
        if (id == session->model.local_id)
        {
            desktop_session__keys_reset(session);
        }
        else
        {
            client_msg__channel__member_changed(session, &previous, NULL);
        }
    }
    else if (clib__strcmp(type, "channel_join") == 0 && slot >= 0)
    {
        // store the new channel
        session->model.clients[slot].channel = channel;
        session->model.clients[slot].access_known = 0;
        session->model.clients[slot].access = 0;
        audio = server_msg__read_number(message, "audio_state");
        session->model.clients[slot].audio = audio >= 1 && audio <= 4 ? (int)audio : 3;

        // apply the member change
        if (id == session->model.local_id)
        {
            session->model.channel = channel;
            desktop_session__keys_reset(session);
        }
        else
        {
            client_msg__channel__member_changed(session, &previous, &session->model.clients[slot]);
        }
    }
    else if (clib__strcmp(type, "client_rename") == 0 && slot >= 0)
    {
        // copy the new name
        valid = clib__string_copy(session->model.clients[slot].name, sizeof(session->model.clients[slot].name), server_msg__read_string(message, "new_username"));
        if (valid && id == session->model.local_id)
        {
            desktop_session__detail(session, "Username changed.", 0);
        }
    }
    else if (clib__strcmp(type, "audio_state_of_single_client") == 0 && slot >= 0)
    {
        // store the audio state
        audio = server_msg__read_number(message, "value");
        session->model.clients[slot].audio = audio >= 1 && audio <= 4 ? (int)audio : 3;
    }
    else if (clib__strcmp(type, "current_channel_active_microphone_usage") == 0)
    {
        // read the audio states
        items = cJSON_GetObjectItemCaseSensitive(message, "clients");
        cJSON_ArrayForEach(item, items)
        {
            slot = desktop_session__client(session, server_msg__read_number(item, "client_id"));
            audio = server_msg__read_number(item, "audio_state");
            if (slot >= 0 && session->model.clients[slot].channel == session->model.channel && audio >= 1 && audio <= 4)
            {
                session->model.clients[slot].audio = (int)audio;
            }
        }
    }
    else if ((clib__strcmp(type, "client_going_to_idle_mode") == 0 || clib__strcmp(type, "client_coming_back_from_idle_mode") == 0) && slot >= 0)
    {
        // store the idle state
        session->model.clients[slot].idle = clib__strcmp(type, "client_going_to_idle_mode") == 0;
        session->model.clients[slot].access_known = 0;
        session->model.clients[slot].access = 0;
        if (!session->model.clients[slot].idle && channel >= -1)
        {
            session->model.clients[slot].channel = channel;
        }

        // apply the member change
        if (id == session->model.local_id)
        {
            session->model.idle = session->model.clients[slot].idle;
            session->model.channel = session->model.clients[slot].channel;
            desktop_session__keys_reset(session);
            desktop_session__detail(session, session->model.idle ? "You are idle." : "Returned from idle. Waiting for channel keys...", 0);
        }
        else
        {
            client_msg__channel__member_changed(session, &previous, &session->model.clients[slot]);
        }
    }
    else if (clib__strcmp(type, "channel_create") == 0)
    {
        // add the created channel
        valid = server_msg__channel__read_channel(session, message);
        if (valid && server_msg__read_number(message, "channel_creator_id") == session->model.local_id)
        {
            session->model.channel_result_sequence++;
            session->model.channel_result_id = channel;
            session->model.channel_result_action = DESKTOP_CHANNEL_CREATE;
            desktop_session__detail(session, "Channel created.", 0);
        }
    }
    else if (clib__strcmp(type, "channel_delete") == 0)
    {
        // remove the deleted channel
        slot = desktop_session__channel(session, channel);
        if (slot >= 0)
        {
            session->model.channel_count--;
            session->model.channels[slot] = session->model.channels[session->model.channel_count];
            if (server_msg__read_number(message, "channel_deletor_id") == session->model.local_id)
            {
                session->model.channel_result_sequence++;
                session->model.channel_result_id = channel;
                session->model.channel_result_action = DESKTOP_CHANNEL_REMOVE;
                desktop_session__detail(session, "Channel deleted.", 0);
            }
        }
    }
    else if (clib__strcmp(type, "channel_edit") == 0)
    {
        // update the edited channel
        valid = server_msg__channel__read_channel(session, message);
        if (valid && server_msg__read_number(message, "channel_editor_id") == session->model.local_id)
        {
            session->model.channel_result_sequence++;
            session->model.channel_result_id = channel;
            session->model.channel_result_action = DESKTOP_CHANNEL_EDIT;
            desktop_session__detail(session, "Channel changes saved.", 0);
        }
    }
    else if (clib__strcmp(type, "channel_maintainer_id") == 0 && channel == session->model.channel)
    {
        // read the new maintainer
        maintainer = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "has_maintainer")) ? server_msg__read_number(message, "maintainer_id") : -1;
        generation = server_msg__read_number(message, "maintainer_generation");
        slot = desktop_session__client(session, maintainer);

        // check the maintainer client
        if (generation < 0 || (maintainer >= 0 && (slot < 0 || session->model.clients[slot].channel != channel || session->model.clients[slot].idle || session->model.clients[slot].music)))
        {
            return;
        }

        // store the maintainer
        session->maintainer = maintainer;
        session->generation = generation;
        client_msg__channel__refresh_keys(session);
    }
    else if (clib__strcmp(type, "direct_chat_message") == 0)
    {
        server_msg__chat__process_direct(session, message);
    }
    else if (clib__strcmp(type, "channel_chat_message") == 0 && channel == session->model.channel && session->model.keys_ready)
    {
        // skip ignored senders
        index = desktop_session__client(session, server_msg__read_number(message, "sender_id"));
        if (index >= 0 && session->model.clients[index].ignored)
        {
            return;
        }

        // decrypt with current keys
        encrypted_body = crypto_session__channel_open(&session->keys, server_msg__read_string(message, "value"));
        plaintext = encrypted_body ? crypto_session__content_decrypt(&session->keys, encrypted_body) : NULL;
        crypto_session__free_text(encrypted_body);
        encrypted_body = NULL;

        // try the previous keys
        if (!plaintext && session->have_previous)
        {
            encrypted_body = crypto_session__channel_open(&session->previous_keys, server_msg__read_string(message, "value"));
            plaintext = encrypted_body ? crypto_session__content_decrypt(&session->previous_keys, encrypted_body) : NULL;
            crypto_session__free_text(encrypted_body);
            encrypted_body = NULL;
        }

        // add the chat message
        if (plaintext)
        {
            server_msg__chat__receive_text(session, channel, -1, server_msg__read_string(message, "sender_username"), plaintext, index, server_msg__read_number(message, "server_chat_message_id"));
        }

        // free the plain text
        crypto_session__free_text(plaintext);
    }
    else if (clib__strcmp(type, "server_chat_message_id_for_local_message_id") == 0)
    {
        server_msg__chat__acknowledge(session, message);
    }
    else if (clib__strcmp(type, "chat_message_edit") == 0)
    {
        // legacy outer edits are not content-encrypted and must never alter retained text
        return;
    }
    else if (clib__strcmp(type, "error") == 0 || clib__strcmp(type, "permission_denied") == 0)
    {
        // report the server error
        session->model.action_error_sequence++;
        desktop_session__detail(session, server_msg__read_string(message, "value")[0] ? server_msg__read_string(message, "value") : "The server rejected the action.", 0);
    }
    else if (clib__strcmp(type, "access_denied") == 0)
    {
        // report the refusal
        session->model.action_error_sequence++;
        if (server_msg__read_string(message, "permission")[0])
        {
            // the server names the permission or power that refused the request
            clib__string_format(refusal, sizeof(refusal), "Insufficient permissions (%s).", server_msg__read_string(message, "permission"));
            desktop_session__detail(session, refusal, 0);
        }
        else
        {
            desktop_session__detail(session, server_msg__read_string(message, "reason")[0] ? server_msg__read_string(message, "reason") : "Insufficient permissions.", 0);
        }
    }
    else if (clib__strcmp(type, "channel_full") == 0)
    {
        desktop_session__detail(session, "That channel is full.", 0);
    }

    // update the tree data
    server_msg__tree__receive(session, message);

    // report invalid state
    if (!valid)
    {
        desktop_session__detail(session, "Server state exceeds the native client's limits or contains invalid fields.", 1);
    }
}
