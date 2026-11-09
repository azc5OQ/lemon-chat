#include "crypto/crypto_session.h"
#include "clib.h"
#include "network/protocol.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

// declarations
static void _server_chat_internal__edit(desktop_session_t *session, int64 channel, int64 peer_slot, int64 author_slot, int64 server_id, const cJSON *edit);

/**
 * @brief retains bounded plain chat text; markup is never interpreted as UI layout.
 * @param desktop_session_t* session -> session state; worker-owned unless this is a GUI entry point
 * @param int64 channel -> server-assigned channel ID
 * @param const char* sender -> display name copied into the retained message
 * @param const char* text -> borrowed NUL-terminated text; copied before returning when retained
 * @param uint64 local_id -> nonzero local message ID for pending outgoing text, zero for incoming text
 * @return void
 */
void server_msg__chat__append(desktop_session_t *session, int64 channel, const char *sender, const char *text, uint64 local_id)
{
    desktop_chat_t *chat = NULL_POINTER;
    if (session->model.chat_count == DESKTOP_SESSION_HISTORY)
    {
        clib__memmove(session->model.chats, session->model.chats + 1, sizeof(session->model.chats[0]) * (DESKTOP_SESSION_HISTORY - 1));
        session->model.chat_count--;
    }
    chat = &session->model.chats[session->model.chat_count++];
    clib__null_memory(chat, sizeof(*chat));
    desktop_chat_format__default(&chat->format);
    chat->sequence = ++session->next_chat_sequence;
    chat->connection_serial = session->model.connection_serial;
    chat->server_id = -1;
    chat->latest_edit_sequence = -1;
    chat->channel = channel;
    chat->peer = -1;
    chat->local_id = local_id;
    chat->pending = local_id != 0;
    if (!local_id)
    {
        desktop_voice__sound(session->voice, DESKTOP_SOUND_MESSAGE_RECEIVED);
    }
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
void server_msg__chat__append_direct(desktop_session_t *session, int64 slot, const char *text, uint64 local_id)
{
    desktop_chat_t *chat = NULL_POINTER;
    int64 local = desktop_session__client(session, session->model.local_id);
    desktop_client_t *peer = &session->model.clients[slot];
    if (!local_id && peer->direct_received < 0xffffffffffffffffULL)
    {
        peer->direct_received++;
    }
    server_msg__chat__append(session, -1, local_id ? (local >= 0 ? session->model.clients[local].name : "You") : peer->name, text, local_id);
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
void server_msg__chat__receive_text(desktop_session_t *session, int64 channel, int64 slot, const char *sender, const char *payload, int64 author_slot, int64 server_id)
{
    cJSON *wrapper = payload ? desktop_protocol__parse_json(payload, clib__strlen(payload), DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT) : NULL;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(wrapper, "value");
    const char *text = cJSON_IsString(value) ? value->valuestring : payload;
    const char *id = server_msg__read_string(wrapper, "reaction_id");
    desktop_chat_t *chat = NULL_POINTER;
    if (clib__strcmp(server_msg__read_string(wrapper, "type"), "chat_message_edit") == 0)
    {
        _server_chat_internal__edit(session, channel, slot, author_slot, server_id, wrapper);
        crypto_session__free_json(wrapper);
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
        desktop_chat_format__read(&chat->format, wrapper);
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
    crypto_session__free_json(wrapper);
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
static void _server_chat_internal__edit(desktop_session_t *session, int64 channel, int64 peer_slot, int64 author_slot, int64 server_id, const cJSON *edit)
{
    const char *id = server_msg__read_string(edit, "message_id");
    const char *author = server_msg__read_string(edit, "author_public_key");
    const char *replacement = server_msg__read_string(edit, "new_message_value");
    const desktop_client_t *sender = NULL_POINTER;
    desktop_chat_t *target = NULL_POINTER;
    desktop_chat_t *chat = NULL_POINTER;
    int64 index = 0;
    if (author_slot < 0 || author_slot >= session->model.client_count || server_id < 0 || !desktop_chat__valid_id(id) || !desktop_chat__valid_text(replacement) || !author[0] || clib__strcmp(author, session->client_keys[author_slot]) != 0)
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
        if (chat->attachment_id || chat->connection_serial != session->model.connection_serial || chat->author_serial != sender->serial || clib__strcmp(chat->reaction_id, id) != 0 || clib__strcmp(chat->author_public_key, author) != 0 || (peer_slot >= 0 ? chat->peer != sender->id || chat->peer_serial != sender->serial : chat->peer >= 0 || chat->channel != channel))
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
void server_msg__chat__acknowledge(desktop_session_t *session, const cJSON *message)
{
    int64 id = server_msg__read_number(message, "local_message_id");
    int64 server_id = server_msg__read_number(message, "server_chat_message_id");
    int64 index = 0;
    desktop_chat_t *chat = NULL_POINTER;
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
void server_msg__chat__process_direct(desktop_session_t *session, const cJSON *message)
{
    cJSON *envelope = NULL_POINTER;
    cJSON *body = NULL_POINTER;
    char *encrypted_body = NULL_POINTER;
    char *plaintext = NULL_POINTER;
    desktop_content_keys_t direct_keys = { 0 };
    desktop_content_keys_t channel_keys = { 0 };
    boole complete = 0;
    int64 sender = server_msg__read_number(message, "sender_id");
    int64 slot = desktop_session__client(session, sender);

    if (slot < 0 || sender == session->model.local_id || session->model.clients[slot].music)
    {
        return;
    }
    envelope = desktop_protocol__parse_json(server_msg__read_string(message, "value"), clib__strlen(server_msg__read_string(message, "value")), DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT);
    encrypted_body = crypto_session__direct_open(session->crypto, envelope, &direct_keys);
    if (encrypted_body)
    {
        plaintext = crypto_session__direct_content_decrypt(&direct_keys, encrypted_body, FALSE);
        body = plaintext ? desktop_protocol__parse_json(plaintext, clib__strlen(plaintext), DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT) : NULL;
        complete = sender == session->maintainer && session->maintainer >= 0 && clib__strcmp(server_msg__read_string(body, "type"), "channel_keys_from_maintainer") == 0 && server_msg__read_number(body, "channel_id") == session->model.channel && server_msg__read_number(body, "maintainer_generation") == session->generation && crypto_session__read_keys(server_msg__read_string(body, "value"), &channel_keys);
    }
    if (complete)
    {
        if (session->model.keys_ready)
        {
            session->previous_keys = session->keys;
            session->have_previous = TRUE;
        }
        session->keys = channel_keys;
        session->model.keys_ready = 1;
        desktop_session__detail(session, "Connected. Channel keys are ready.", 0);
    }
    if (clib__strcmp(server_msg__read_string(body, "type"), "direct_chat_message") == 0)
    {
        server_msg__chat__receive_text(session, -1, slot, session->model.clients[slot].name, server_msg__read_string(body, "value"), slot, server_msg__read_number(message, "server_chat_message_id"));
    }
    crypto_session__free_json(body);
    crypto_session__free_json(envelope);
    crypto_session__free_text(encrypted_body);
    crypto_session__free_text(plaintext);
    mbedtls_platform_zeroize(&direct_keys, sizeof(direct_keys));
    mbedtls_platform_zeroize(&channel_keys, sizeof(channel_keys));
}
