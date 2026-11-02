#include "clib.h"
#include "attachments/attachments_internal.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

/**
 * @brief updates worker-owned state from one authenticated server message.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const cJSON* message -> borrowed authenticated server message
 * @return void
 */
void server_msg__dispatch(desktop_session_t* session, const cJSON* message)
{
    const char* type = server_msg__read_string(message, "type");
    const cJSON* items = NULL;
    const cJSON* item = NULL;
    int64 id = server_msg__read_number(message, "client_id");
    int64 channel = server_msg__read_number(message, "channel_id");
    int64 slot = desktop_session__client(session, id);
    int64 index = 0;
    int64 audio = -1;
    char* plaintext = NULL;
    boole valid = 1;
    boole client_list = clib__strcmp(type, "client_list") == 0;

    desktop_voice__message(session, message);
    server_msg__actions__receive(session, message);
    server_msg__attachments__receive(session, message);

    if (clib__strcmp(type, "client_list") == 0 || clib__strcmp(type, "channel_list") == 0)
    {
        items = cJSON_GetObjectItemCaseSensitive(message, client_list ? "clients" : "channels");
        valid = cJSON_IsArray(items);
        if (client_list)
        {
            session->model.client_count = 0;
        }
        else
        {
            session->model.channel_count = 0;
        }
        cJSON_ArrayForEach(item, items)
        {
            valid = valid && (client_list ? server_msg__connection__read_client(session, item) : server_msg__channel__read_channel(session, item));
        }
    }
    else if (clib__strcmp(type, "client_connect") == 0)
    {
        valid = server_msg__connection__read_client(session, message);
        session->rotate |= channel == session->model.channel;
    }
    else if (clib__strcmp(type, "client_disconnect") == 0 && slot >= 0)
    {
        session->rotate |= session->model.clients[slot].channel == session->model.channel;
        session->model.client_count--;
        session->model.clients[slot] = session->model.clients[session->model.client_count];
        clib__memcpy(session->client_keys[slot], session->client_keys[session->model.client_count], sizeof(session->client_keys[slot]));
        clib__null_memory(&session->model.clients[session->model.client_count], sizeof(session->model.clients[0]));
        clib__null_memory(session->client_keys[session->model.client_count], sizeof(session->client_keys[0]));
    }
    else if (clib__strcmp(type, "channel_join") == 0 && slot >= 0)
    {
        session->rotate |= session->model.clients[slot].channel == session->model.channel || channel == session->model.channel;
        session->model.clients[slot].channel = channel;
        session->model.clients[slot].access_known = 0;
        session->model.clients[slot].access = 0;
        audio = server_msg__read_number(message, "audio_state");
        session->model.clients[slot].audio = audio >= 1 && audio <= 4 ? (int)audio : 3;
        if (id == session->model.local_id)
        {
            session->model.channel = channel;
            desktop_session__keys_reset(session);
        }
    }
    else if (clib__strcmp(type, "client_rename") == 0 && slot >= 0)
    {
        valid = clib__string_copy(session->model.clients[slot].name, sizeof(session->model.clients[slot].name),
            server_msg__read_string(message, "new_username"));
        if (valid && id == session->model.local_id)
        {
            desktop_session__detail(session, "Username changed.", 0);
        }
    }
    else if (clib__strcmp(type, "audio_state_of_single_client") == 0 && slot >= 0)
    {
        audio = server_msg__read_number(message, "value");
        session->model.clients[slot].audio = audio >= 1 && audio <= 4 ? (int)audio : 3;
    }
    else if (clib__strcmp(type, "current_channel_active_microphone_usage") == 0)
    {
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
        session->model.clients[slot].idle = clib__strcmp(type, "client_going_to_idle_mode") == 0;
        session->rotate |= session->model.clients[slot].channel == session->model.channel;
        session->model.clients[slot].access_known = 0;
        session->model.clients[slot].access = 0;
        if (!session->model.clients[slot].idle && channel >= 0)
        {
            session->model.clients[slot].channel = channel;
            session->rotate |= channel == session->model.channel;
        }
        if (id == session->model.local_id)
        {
            session->model.idle = session->model.clients[slot].idle;
            session->model.channel = session->model.clients[slot].channel;
            desktop_session__keys_reset(session);
            desktop_session__detail(session, session->model.idle ? "You are idle." : "Returned from idle. Waiting for channel keys...", 0);
        }
    }
    else if (clib__strcmp(type, "channel_create") == 0)
    {
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
        session->maintainer = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "has_maintainer")) ?
            server_msg__read_number(message, "maintainer_id") : -1;
        session->generation = server_msg__read_number(message, "maintainer_generation");
        session->rotate = session->maintainer >= 0 && session->maintainer == session->model.local_id;
        session->model.keys_ready = 0;
        session->waiting_since = desktop_system__milliseconds();
    }
    else if (clib__strcmp(type, "direct_chat_message") == 0)
    {
        server_msg__chat__process_direct(session, message);
    }
    else if (clib__strcmp(type, "channel_chat_message") == 0 && channel == session->model.channel && session->model.keys_ready)
    {
        index = desktop_session__client(session, server_msg__read_number(message, "sender_id"));
        if (index >= 0 && session->model.clients[index].ignored)
        {
            return;
        }
        plaintext = desktop_crypto__content_decrypt(&session->keys, server_msg__read_string(message, "value"));
        if (!plaintext && session->have_previous)
        {
            plaintext = desktop_crypto__content_decrypt(&session->previous_keys, server_msg__read_string(message, "value"));
        }
        if (plaintext)
        {
            server_msg__chat__receive_text(session, channel, -1, server_msg__read_string(message, "sender_username"), plaintext, index,
                server_msg__read_number(message, "server_chat_message_id"));
        }
        desktop_crypto__free_text(plaintext);
    }
    else if (clib__strcmp(type, "server_chat_message_id_for_local_message_id") == 0)
    {
        server_msg__chat__acknowledge(session, message);
    }
    else if (clib__strcmp(type, "chat_message_edit") == 0)
    {
        // Legacy outer edits are not content-encrypted and must never alter retained text.
        return;
    }
    else if (clib__strcmp(type, "error") == 0 || clib__strcmp(type, "permission_denied") == 0)
    {
        session->model.action_error_sequence++;
        desktop_session__detail(session, server_msg__read_string(message, "value")[0] ? server_msg__read_string(message, "value") : "The server rejected the action.", 0);
    }
    else if (clib__strcmp(type, "access_denied") == 0)
    {
        session->model.action_error_sequence++;
        desktop_session__detail(session, server_msg__read_string(message, "reason")[0] ? server_msg__read_string(message, "reason") : "Insufficient permissions.", 0);
    }
    else if (clib__strcmp(type, "channel_full") == 0)
    {
        desktop_session__detail(session, "That channel is full.", 0);
    }
    server_msg__tree__receive(session, message);
    if (!valid)
    {
        desktop_session__detail(session, "Server state exceeds the native client's limits or contains invalid fields.", 1);
    }
}
