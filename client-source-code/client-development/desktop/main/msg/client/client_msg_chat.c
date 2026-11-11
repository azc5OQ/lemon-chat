#include "crypto/crypto_session.h"
#include "clib.h"
#include "session/session_internal.h"
#include "client_msg.h"
#include "msg/server/server_msg.h"
#include "mbedtls/platform_util.h"

// declarations
static char *_client_chat_internal__text(desktop_session_t *session, const char *text, char *id, uint64 capacity, const desktop_chat_format_t *format);
static void _client_chat_internal__remember(desktop_session_t *session, const char *id, const desktop_chat_format_t *format);
static boole _client_chat_internal__channel(desktop_session_t *session, int64 channel, const char *payload, uint64 local_id);
static boole _client_chat_internal__direct(desktop_session_t *session, int64 peer, uint64 serial, const char *payload, uint64 local_id, boole is_closed_notice);

/**
 * @brief creates the text wrapper understood by browser clients inside channel/private encryption.
 * @param desktop_session_t* session -> worker-owned secure random source
 * @param const char* text -> plain user text
 * @param char* id -> caller-owned identifier output
 * @param uint64 capacity -> identifier output capacity including terminator
 * @return char* -> owned JSON, or NULL; release with crypto_session__free_text
 */
static char *_client_chat_internal__text(desktop_session_t *session, const char *text, char *id, uint64 capacity, const desktop_chat_format_t *format)
{
    static const char hex[] = "0123456789abcdef";
    ubyte random[16] = { 0 };
    cJSON *wrapper = NULL_POINTER;
    char *result = NULL_POINTER;
    int64 index = 0;
    if (capacity < 33 || !crypto_session__random(session->crypto, random, sizeof(random)))
    {
        mbedtls_platform_zeroize(random, sizeof(random));
        return NULL;
    }
    clib__null_memory(id, capacity);
    for (index = 0; index < 16; index++)
    {
        id[index * 2] = hex[random[index] >> 4];
        id[index * 2 + 1] = hex[random[index] & 15];
    }
    wrapper = cJSON_CreateObject();
    if (wrapper && cJSON_AddStringToObject(wrapper, "value", text) && cJSON_AddStringToObject(wrapper, "reaction_id", id) && desktop_chat_format__write(format, wrapper))
    {
        result = cJSON_PrintUnformatted(wrapper);
    }
    crypto_session__free_json(wrapper);
    mbedtls_platform_zeroize(random, sizeof(random));
    return result;
}

/**
 * @brief encrypts serialized text or edit content with current channel keys, never plaintext.
 * @param desktop_session_t* session -> worker-owned connection with current channel keys
 * @param int64 channel -> channel captured when the GUI queued the text
 * @param const char* payload -> borrowed serialized content
 * @param uint64 local_id -> fresh delivery identifier
 * @return boole -> TRUE when the encrypted message was queued
 */
static boole _client_chat_internal__channel(desktop_session_t *session, int64 channel, const char *payload, uint64 local_id)
{
    char *encrypted = NULL_POINTER;
    char *envelope = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    boole complete = 0;
    boole sent = FALSE;

    if (channel != session->model.channel || !session->model.keys_ready || !payload)
    {
        desktop_session__detail(session, "Message was not sent: the channel changed or its keys are not ready.", 0);
        return FALSE;
    }
    encrypted = payload ? crypto_session__content_encrypt(session->crypto, &session->keys, payload) : NULL;
    envelope = encrypted ? crypto_session__channel_seal(&session->keys, encrypted) : NULL;
    message = client_msg__delivery__new_message("channel_chat_message");
    complete = envelope && message && cJSON_AddStringToObject(message, "value", envelope) && cJSON_AddNumberToObject(message, "receiver_id", (double)channel) && cJSON_AddNumberToObject(message, "local_message_id", (double)local_id);
    sent = client_msg__delivery__send(session, message, complete);
    crypto_session__free_text(envelope);
    crypto_session__free_text(encrypted);
    return sent;
}

/**
 * @brief encrypts one private text or edit with fresh content keys protected for the exact recipient.
 *
 * @param desktop_session_t* session -> worker-owned authenticated session
 * @param int64 peer -> recipient client ID
 * @param uint64 serial -> recipient lifetime captured by the GUI; rejects disconnect/ID reuse
 * @param const char* payload -> serialized content placed inside direct_chat_message.value
 * @param uint64 local_id -> fresh delivery identifier
 * @param boole is_closed_notice -> TRUE sends the notice that this user closed the conversation: content without text, no status line on failure
 *
 * @return boole -> TRUE when the complete encrypted envelope was queued
 */
static boole _client_chat_internal__direct(desktop_session_t *session, int64 peer, uint64 serial, const char *payload, uint64 local_id, boole is_closed_notice)
{
    int64 slot = desktop_session__client(session, peer);
    desktop_content_keys_t keys = { 0 };
    cJSON *body = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    char *body_json = NULL_POINTER;
    char *encrypted_body = NULL_POINTER;
    char *envelope_json = NULL_POINTER;
    boole complete = 0;
    boole sent = FALSE;
    boole keys_ready = FALSE;
    if (slot < 0 || session->model.clients[slot].serial != serial || peer == session->model.local_id || session->model.clients[slot].music || !session->client_keys[slot][0] || (!payload && !is_closed_notice))
    {
        if (!is_closed_notice)
        {
            desktop_session__detail(session, "Private message was not sent: that client is no longer available.", 0);
        }
        return FALSE;
    }
    keys_ready = crypto_session__content_keys(session->crypto, &keys);
    // the notice has no text: its type alone tells the receiving client that the conversation was closed
    body = client_msg__delivery__new_message(is_closed_notice ? "direct_chat_closed" : "direct_chat_message");
    if (body && (is_closed_notice || cJSON_AddStringToObject(body, "value", payload)))
    {
        body_json = cJSON_PrintUnformatted(body);
    }
    if (keys_ready && body_json)
    {
        encrypted_body = crypto_session__direct_content_encrypt(&keys, body_json, 1024, FALSE);
    }
    if (encrypted_body)
    {
        envelope_json = crypto_session__direct_seal(session->crypto, session->client_keys[slot], &keys, encrypted_body);
    }
    message = client_msg__delivery__new_message("direct_chat_message");
    complete = message && envelope_json && cJSON_AddStringToObject(message, "purpose", "conversation") && cJSON_AddStringToObject(message, "value", envelope_json) && cJSON_AddNumberToObject(message, "receiver_id", (double)peer) && cJSON_AddNumberToObject(message, "local_message_id", (double)local_id);
    sent = client_msg__delivery__send(session, message, complete);
    if (!sent && !is_closed_notice)
    {
        desktop_session__detail(session, "Private message could not be encrypted or queued.", 0);
    }
    crypto_session__free_json(body);
    crypto_session__free_text(body_json);
    crypto_session__free_text(encrypted_body);
    crypto_session__free_text(envelope_json);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    return sent;
}

/**
 * @brief records the local author's identity on the message just appended by this worker.
 * @param desktop_session_t* session -> worker-owned session with a retained outgoing message
 * @param const char* id -> identifier included in its encrypted content
 * @return void
 */
static void _client_chat_internal__remember(desktop_session_t *session, const char *id, const desktop_chat_format_t *format)
{
    desktop_chat_t *chat = &session->model.chats[session->model.chat_count - 1];
    int64 local = desktop_session__client(session, session->model.local_id);
    chat->author_serial = local >= 0 ? session->model.clients[local].serial : 0;
    clib__string_copy(chat->author_public_key, sizeof(chat->author_public_key), session->public_key);
    clib__string_copy(chat->reaction_id, sizeof(chat->reaction_id), id);
    if (desktop_chat_format__valid(format))
    {
        chat->format = *format;
    }
}

/**
 * @brief sends an original channel message and retains its stable encrypted-content identity.
 * @param desktop_session_t* session -> worker-owned session
 * @param int64 channel -> selected channel
 * @param const char* text -> borrowed bounded plain text
 * @return void
 */
void client_msg__chat__send_channel_message(desktop_session_t *session, int64 channel, const char *text, const desktop_chat_format_t *format)
{
    char id[33] = { 0 };
    char *payload = _client_chat_internal__text(session, text, id, sizeof(id), format);
    uint64 local_id = ++session->next_local_id;
    int64 local = desktop_session__client(session, session->model.local_id);
    if (_client_chat_internal__channel(session, channel, payload, local_id))
    {
        server_msg__chat__append(session, channel, local >= 0 ? session->model.clients[local].name : "You", text, local_id);
        _client_chat_internal__remember(session, id, format);
    }
    crypto_session__free_text(payload);
    mbedtls_platform_zeroize(id, sizeof(id));
}

/**
 * @brief sends an original private message under the recipient's current connection lifetime.
 * @param desktop_session_t* session -> worker-owned session
 * @param int64 peer -> recipient client ID
 * @param uint64 serial -> captured recipient lifetime
 * @param const char* text -> borrowed bounded plain text
 * @return void
 */
void client_msg__chat__send_direct_message(desktop_session_t *session, int64 peer, uint64 serial, const char *text, const desktop_chat_format_t *format)
{
    char id[33] = { 0 };
    char *payload = _client_chat_internal__text(session, text, id, sizeof(id), format);
    uint64 local_id = ++session->next_local_id;
    if (_client_chat_internal__direct(session, peer, serial, payload, local_id, FALSE))
    {
        server_msg__chat__append_direct(session, desktop_session__client(session, peer), text, local_id);
        _client_chat_internal__remember(session, id, format);
    }
    crypto_session__free_text(payload);
    mbedtls_platform_zeroize(id, sizeof(id));
}

/**
 * @brief tells one client that the local user closed the private conversation with that client.
 *        The notice is an encrypted direct message without text, so the server cannot read what it is.
 *
 * @param desktop_session_t* session -> worker-owned session
 * @param int64 peer -> client ID of the other side of the conversation
 * @param uint64 serial -> that client's connection lifetime captured by the GUI
 *
 * @return void
 */
void client_msg__chat__send_direct_closed_notice(desktop_session_t *session, int64 peer, uint64 serial)
{
    _client_chat_internal__direct(session, peer, serial, NULL_POINTER, ++session->next_local_id, TRUE);
}

/**
 * @brief sends an encrypted replacement while preserving the original message identifier and row.
 * @param desktop_session_t* session -> sole protocol worker
 * @param uint64 sequence -> original retained message sequence
 * @param uint64 connection_serial -> connection captured when editing began
 * @param const char* text -> bounded replacement text
 * @return void
 */
void client_msg__chat__send_edit(desktop_session_t *session, uint64 sequence, uint64 connection_serial, const char *text)
{
    desktop_chat_t *chat = NULL_POINTER;
    cJSON *edit = NULL_POINTER;
    char *payload = NULL_POINTER;
    int64 index = 0;
    uint64 local_id = 0;
    boole sent = FALSE;
    for (index = 0; index < session->model.chat_count; index++)
    {
        if (session->model.chats[index].sequence == sequence)
        {
            chat = &session->model.chats[index];
            break;
        }
    }
    if (!chat || connection_serial != session->model.connection_serial || !desktop_session__can_edit(&session->model, chat) || !desktop_chat__valid_text(text) || clib__strcmp(chat->author_public_key, session->public_key) != 0)
    {
        desktop_session__detail(session, "Edit was not sent: the message, connection, recipient or encryption keys are no longer available.", FALSE);
        return;
    }
    if (clib__strcmp(chat->text, text) == 0)
    {
        return;
    }
    edit = client_msg__delivery__new_message("chat_message_edit");
    if (edit && cJSON_AddStringToObject(edit, "message_id", chat->reaction_id) && cJSON_AddStringToObject(edit, "author_public_key", chat->author_public_key) && cJSON_AddStringToObject(edit, "new_message_value", text))
    {
        payload = cJSON_PrintUnformatted(edit);
    }
    local_id = ++session->next_local_id;
    sent = chat->peer >= 0 ? _client_chat_internal__direct(session, chat->peer, chat->peer_serial, payload, local_id, FALSE) : _client_chat_internal__channel(session, chat->channel, payload, local_id);
    if (sent)
    {
        clib__null_memory(chat->text, sizeof(chat->text));
        clib__string_copy(chat->text, sizeof(chat->text), text);
        chat->edited = TRUE;
        chat->edit_local_id = local_id;
    }
    crypto_session__free_text(payload);
    crypto_session__free_json(edit);
}
