#include "session/session_internal.h"
#include "platform/memory.h"

/**
 * @brief checks the original random identifier without accepting server message numbers.
 * @param const char* id -> bounded lowercase hexadecimal identifier
 * @return boole -> TRUE for exactly sixteen encoded bytes
 */
boole desktop_chat__valid_id(const char* id)
{
    int64 index = 0;
    if (clib__string_length(id, 33) != 32)
    {
        return FALSE;
    }
    for (index = 0; index < 32; index++)
    {
        if (!((id[index] >= '0' && id[index] <= '9') || (id[index] >= 'a' && id[index] <= 'f')))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief validates bounded UTF-8 replacement text and rejects whitespace-only edits.
 * @param const char* text -> NUL-terminated replacement text
 * @return boole -> TRUE when text fits without truncation and has visible content
 */
boole desktop_chat__valid_text(const char* text)
{
    int64 length = clib__string_length(text, DESKTOP_CHAT_CAPACITY);
    int64 index = 0;
    int64 remaining = 0;
    uint64 codepoint = 0;
    uint64 minimum = 0;
    unsigned char byte = 0;
    boole visible = FALSE;
    if (length <= 0)
    {
        return FALSE;
    }
    while (index < length)
    {
        byte = (unsigned char)text[index++];
        codepoint = byte;
        remaining = 0;
        minimum = 0;
        if (byte >= 0xc2 && byte <= 0xdf)
        {
            codepoint = byte & 0x1f;
            remaining = 1;
            minimum = 0x80;
        }
        else if (byte >= 0xe0 && byte <= 0xef)
        {
            codepoint = byte & 0x0f;
            remaining = 2;
            minimum = 0x800;
        }
        else if (byte >= 0xf0 && byte <= 0xf4)
        {
            codepoint = byte & 7;
            remaining = 3;
            minimum = 0x10000;
        }
        else if (byte >= 0x80)
        {
            return FALSE;
        }
        while (remaining-- > 0)
        {
            if (index == length || ((unsigned char)text[index] & 0xc0) != 0x80)
            {
                return FALSE;
            }
            codepoint = (codepoint << 6) | ((unsigned char)text[index++] & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        {
            return FALSE;
        }
        if (!(codepoint <= 0x20 || codepoint == 0xa0 || codepoint == 0x1680
            || (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 || codepoint == 0x2029
            || codepoint == 0x202f || codepoint == 0x205f || codepoint == 0x3000 || codepoint == 0xfeff))
        {
            visible = TRUE;
        }
    }
    return visible;
}

/**
 * @brief checks whether an outgoing text still belongs to the current connection and recipient.
 * @param const desktop_session_view_t* view -> immutable GUI snapshot or worker-owned model
 * @param const desktop_chat_t* chat -> retained text in that snapshot
 * @return boole -> TRUE when the message can be edited now
 */
boole desktop_session__can_edit(const desktop_session_view_t* view, const desktop_chat_t* chat)
{
    int64 index = 0;
    boole author = FALSE;
    boole recipient = FALSE;
    if (!view || !chat || !view->connected || view->idle || !chat->local_id || chat->attachment_id
        || chat->pending || chat->edit_local_id || !chat->author_public_key[0]
        || !desktop_chat__valid_id(chat->reaction_id) || chat->connection_serial != view->connection_serial)
    {
        return FALSE;
    }
    for (index = 0; index < view->client_count; index++)
    {
        author |= view->clients[index].id == view->local_id && view->clients[index].serial == chat->author_serial;
        recipient |= view->clients[index].id == chat->peer && view->clients[index].serial == chat->peer_serial
            && !view->clients[index].music;
    }
    return author && (chat->peer >= 0 ? recipient && view->policy_private
        : chat->channel == view->channel && view->keys_ready);
}

/**
 * @brief queues a replacement for one exact retained message without changing the GUI snapshot.
 * @param desktop_session_t* session -> GUI-owned session
 * @param uint64 sequence -> retained message sequence, not its wire identifier
 * @param uint64 connection_serial -> connection captured when the editor opened
 * @param const char* text -> replacement copied into the bounded command queue
 * @return boole -> TRUE when accepted; worker rechecks identity and encryption before sending
 */
boole desktop_session__edit(desktop_session_t* session, uint64 sequence, uint64 connection_serial, const char* text)
{
    desktop_session_command_t command = {0};
    const desktop_chat_t* chat = NULL;
    int64 index = 0;
    boole queued = FALSE;
    if (!session || !desktop_chat__valid_text(text))
    {
        return FALSE;
    }
    command.edit = TRUE;
    command.chat_sequence = sequence;
    command.connection_serial = connection_serial;
    clib__string_copy(command.text, sizeof(command.text), text);
    desktop_mutex__lock(session->mutex);
    for (index = 0; index < session->published.chat_count; index++)
    {
        chat = &session->published.chats[index];
        if (chat->sequence == sequence && chat->connection_serial == connection_serial)
        {
            if (session->running && !session->stop && session->command_count < 8
                && desktop_session__can_edit(&session->published, chat))
            {
                command.peer = chat->peer;
                command.peer_serial = chat->peer_serial;
                command.channel = chat->channel;
                session->commands[session->command_count++] = command;
                queued = TRUE;
            }
            break;
        }
    }
    desktop_mutex__unlock(session->mutex);
    desktop_memory__erase(&command, sizeof(command));
    return queued;
}
