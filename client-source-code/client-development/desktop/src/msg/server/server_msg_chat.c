#include "crypto/crypto.h"
#include "clib.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

// declarations
static void _server_chat_internal__edit(desktop_session_t* session, int64 channel, int64 peer_slot, int64 author_slot, int64 server_id, const cJSON* edit);

/**
 * @brief retains bounded plain chat text; markup is never interpreted as UI layout.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param int64 channel -> server-assigned channel ID
 * @param const char* sender -> display name copied into the retained message
 * @param const char* text -> borrowed NUL-terminated text; copied before returning when retained
 * @param uint64 local_id -> nonzero local message ID for pending outgoing text, zero for incoming text
 * @return void
 */
void server_msg__chat__append(desktop_session_t* session, int64 channel, const char* sender, const char* text, uint64 local_id)
{
    desktop_chat_t* chat = NULL;
    if (session->model.chat_count == DESKTOP_SESSION_HISTORY)
    {
        clib__memmove(session->model.chats, session->model.chats + 1, sizeof(session->model.chats[0]) * (DESKTOP_SESSION_HISTORY - 1));
        session->model.chat_count--;
    }
    chat = &session->model.chats[session->model.chat_count++];
    clib__null_memory(chat, sizeof(*chat));
    chat->sequence = ++session->next_chat_sequence;
    chat->connection_serial = session->model.connection_serial;
    chat->server_id = -1;
    chat->latest_edit_sequence = -1;
    chat->channel = channel;
    chat->peer = -1;
    chat->local_id = local_id;
    chat->pending = local_id != 0;
    clib__string_copy(chat->sender, sizeof(chat->sender), sender);
    if (!clib__string_copy(chat->text, sizeof(chat->text), text))
    {
        clib__string_copy(chat->text, sizeof(chat->text), "Message exceeds the native text limit.");
    }
}

/**
 * @brief retains a private message under this connected peer's lifetime, never a reused numeric client ID.
 * @param desktop_session_t* session -> worker-owned session
 * @param int64 slot -> validated index of the connected peer
 * @param const char* text -> plain message text
 * @param uint64 local_id -> pending outgoing ID, or zero for incoming text
 * @return void
 */
void server_msg__chat__append_direct(desktop_session_t* session, int64 slot, const char* text, uint64 local_id)
{
    desktop_chat_t* chat = NULL;
    int64 local = desktop_session__client(session, session->model.local_id);
    desktop_client_t* peer = &session->model.clients[slot];
    if (!local_id && peer->direct_received < 0xffffffffffffffffULL)
    {
        peer->direct_received++;
    }
    server_msg__chat__append(session, -1,
        local_id ? (local >= 0 ? session->model.clients[local].name : "You") : peer->name, text, local_id);
    chat = &session->model.chats[session->model.chat_count - 1];
    chat->peer = peer->id;
    chat->peer_serial = peer->serial;
    clib__string_copy(chat->peer_name, sizeof(chat->peer_name), peer->name);
}

/**
 * @brief extracts the browser's text wrapper while displaying its contents as plain text.
 * @param desktop_session_t* session -> worker-owned session
 * @param int64 channel -> destination channel, ignored for private text
 * @param int64 slot -> connected private peer index, or -1 for channel text
 * @param const char* sender -> channel sender name provided by the server
 * @param const char* payload -> decrypted text wrapper; legacy raw text remains readable
 * @param int64 author_slot -> connected sender index, or -1 when unavailable
 * @param int64 server_id -> authenticated server delivery sequence, or -1 when missing
 * @return void
 */
void server_msg__chat__receive_text(desktop_session_t* session, int64 channel, int64 slot, const char* sender, const char* payload, int64 author_slot, int64 server_id)
{
    cJSON* wrapper = payload ? cJSON_Parse(payload) : NULL;
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(wrapper, "value");
    const char* text = cJSON_IsString(value) ? value->valuestring : payload;
    const char* id = server_msg__read_string(wrapper, "reaction_id");
    desktop_chat_t* chat = NULL;
    if (clib__strcmp(server_msg__read_string(wrapper, "type"), "chat_message_edit") == 0)
    {
        _server_chat_internal__edit(session, channel, slot, author_slot, server_id, wrapper);
        desktop_crypto__free_json(wrapper);
        return;
    }
    if (text && text[0] && (slot < 0 || !session->model.clients[slot].ignored))
    {
        if (slot >= 0)
        {
            server_msg__chat__append_direct(session, slot, text, 0);
        }
        else
        {
            server_msg__chat__append(session, channel, sender, text, 0);
        }
        chat = &session->model.chats[session->model.chat_count - 1];
        chat->server_id = server_id;
        chat->latest_edit_sequence = server_id;
        if (desktop_chat__valid_id(id))
        {
            clib__string_copy(chat->reaction_id, sizeof(chat->reaction_id), id);
        }
        if (author_slot >= 0 && author_slot < session->model.client_count)
        {
            chat->author_serial = session->model.clients[author_slot].serial;
            clib__string_copy(chat->author_public_key, sizeof(chat->author_public_key), session->client_keys[author_slot]);
        }
    }
    desktop_crypto__free_json(wrapper);
}

/**
 * @brief updates one retained message only for its original author, conversation and connection.
 * @param desktop_session_t* session -> worker-owned session
 * @param int64 channel -> channel route, ignored for private messages
 * @param int64 peer_slot -> private peer index, or -1 for a channel route
 * @param int64 author_slot -> authenticated sender index
 * @param int64 server_id -> server's monotonic delivery sequence
 * @param const cJSON* edit -> decrypted control body; invalid controls are silently discarded
 * @return void
 */
static void _server_chat_internal__edit(desktop_session_t* session, int64 channel, int64 peer_slot, int64 author_slot, int64 server_id, const cJSON* edit)
{
    const char* id = server_msg__read_string(edit, "message_id");
    const char* author = server_msg__read_string(edit, "author_public_key");
    const char* replacement = server_msg__read_string(edit, "new_message_value");
    const desktop_client_t* sender = NULL;
    desktop_chat_t* target = NULL;
    desktop_chat_t* chat = NULL;
    int64 index = 0;
    if (author_slot < 0 || author_slot >= session->model.client_count || server_id < 0
        || !desktop_chat__valid_id(id) || !desktop_chat__valid_text(replacement)
        || !author[0] || clib__strcmp(author, session->client_keys[author_slot]) != 0)
    {
        return;
    }
    sender = &session->model.clients[author_slot];
    if (sender->ignored || sender->id == session->model.local_id || (peer_slot >= 0 && peer_slot != author_slot))
    {
        return;
    }
    for (index = 0; index < session->model.chat_count; index++)
    {
        chat = &session->model.chats[index];
        if (chat->attachment_id || chat->connection_serial != session->model.connection_serial
            || chat->author_serial != sender->serial || clib__strcmp(chat->reaction_id, id) != 0
            || clib__strcmp(chat->author_public_key, author) != 0
            || (peer_slot >= 0 ? chat->peer != sender->id || chat->peer_serial != sender->serial
                : chat->peer >= 0 || chat->channel != channel))
        {
            continue;
        }
        if (target)
        {
            // A repeated original ID must not make the edit target ambiguous.
            return;
        }
        target = chat;
    }
    if (target && server_id > target->latest_edit_sequence && server_id > target->server_id)
    {
        clib__null_memory(target->text, sizeof(target->text));
        clib__string_copy(target->text, sizeof(target->text), replacement);
        target->latest_edit_sequence = server_id;
        target->edited = TRUE;
    }
}

/**
 * @brief confirms original messages or edits using their distinct local delivery identifiers.
 * @param desktop_session_t* session -> worker-owned session
 * @param const cJSON* message -> authenticated server acknowledgement
 * @return void
 */
void server_msg__chat__acknowledge(desktop_session_t* session, const cJSON* message)
{
    int64 id = server_msg__read_number(message, "local_message_id");
    int64 server_id = server_msg__read_number(message, "server_chat_message_id");
    int64 index = 0;
    desktop_chat_t* chat = NULL;
    if (id <= 0 || server_id < 0)
    {
        return;
    }
    for (index = 0; index < session->model.chat_count; index++)
    {
        chat = &session->model.chats[index];
        if (chat->connection_serial != session->model.connection_serial)
        {
            continue;
        }
        if (chat->local_id == (uint64)id && chat->pending)
        {
            chat->pending = FALSE;
            chat->server_id = server_id;
            chat->latest_edit_sequence = server_id;
        }
        else if (chat->edit_local_id == (uint64)id && server_id > chat->latest_edit_sequence)
        {
            chat->edit_local_id = 0;
            chat->latest_edit_sequence = server_id;
        }
    }
}

/**
 * @brief decrypts online private messages and accepts channel keys only from the current maintainer.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param const cJSON* message -> borrowed authenticated server message
 * @return void
 */
void server_msg__chat__process_direct(desktop_session_t* session, const cJSON* message)
{
    cJSON* envelope = NULL;
    cJSON* body = NULL;
    char* keys_json = NULL;
    char* plaintext = NULL;
    desktop_content_keys_t direct_keys = {0};
    desktop_content_keys_t channel_keys = {0};
    boole complete = 0;
    int64 sender = server_msg__read_number(message, "sender_id");
    int64 slot = desktop_session__client(session, sender);

    if (slot < 0 || sender == session->model.local_id || session->model.clients[slot].music)
    {
        return;
    }
    envelope = cJSON_Parse(server_msg__read_string(message, "value"));
    keys_json = desktop_crypto__hybrid_decrypt(session->crypto, server_msg__read_string(envelope, "message_keys"));
    if (keys_json && desktop_crypto__read_keys(keys_json, &direct_keys))
    {
        plaintext = desktop_crypto__content_decrypt(&direct_keys, server_msg__read_string(envelope, "message_value"));
        body = plaintext ? cJSON_Parse(plaintext) : NULL;
        complete = sender == session->maintainer && session->maintainer >= 0
            && clib__strcmp(server_msg__read_string(body, "type"), "channel_keys_from_maintainer") == 0
            && server_msg__read_number(body, "channel_id") == session->model.channel
            && server_msg__read_number(body, "maintainer_generation") == session->generation
            && desktop_crypto__read_keys(server_msg__read_string(body, "value"), &channel_keys);
    }
    if (complete)
    {
        session->previous_keys = session->keys;
        session->have_previous = session->model.keys_ready;
        session->keys = channel_keys;
        session->model.keys_ready = 1;
        desktop_session__detail(session, "Connected. Channel keys are ready.", 0);
    }
    if (clib__strcmp(server_msg__read_string(body, "type"), "direct_chat_message") == 0)
    {
        server_msg__chat__receive_text(session, -1, slot, session->model.clients[slot].name, server_msg__read_string(body, "value"), slot,
            server_msg__read_number(message, "server_chat_message_id"));
    }
    desktop_crypto__free_json(body);
    desktop_crypto__free_json(envelope);
    desktop_crypto__free_text(keys_json);
    desktop_crypto__free_text(plaintext);
    mbedtls_platform_zeroize(&direct_keys, sizeof(direct_keys));
    mbedtls_platform_zeroize(&channel_keys, sizeof(channel_keys));
}
