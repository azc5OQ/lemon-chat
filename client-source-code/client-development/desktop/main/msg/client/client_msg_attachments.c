#include "attachments/attachments_internal.h"
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

// declarations
static const char *_client_attachments_internal__intent(const desktop_attachment_record_t *record);
static boole _client_attachments_internal__extra(cJSON *message, const desktop_attachment_record_t *record);
static boole _client_attachments_internal__valid(desktop_session_t *session, const desktop_attachment_record_t *record);

/**
 * @brief selects an existing browser upload intent without inferring the destination from current UI state.
 * @param const desktop_attachment_record_t* record -> locked transfer
 * @return const char* -> static wire intent
 */
static const char *_client_attachments_internal__intent(const desktop_attachment_record_t *record)
{
    if (record->view.peer >= 0)
    {
        return record->view.picture ? "direct_chat_picture_file" : "direct_chat_file";
    }
    return record->view.picture ? "channel_chat_picture_file" : "channel_chat_file";
}

/**
 * @brief attaches all required routing fields or reports an incomplete allocation.
 * @param cJSON* message -> owned message being constructed
 * @param const desktop_attachment_record_t* record -> locked transfer
 * @return boole -> TRUE when every required field exists
 */
static boole _client_attachments_internal__extra(cJSON *message, const desktop_attachment_record_t *record)
{
    cJSON *extra = cJSON_AddObjectToObject(message, "file_send_intent_extra_data");
    return extra && cJSON_AddStringToObject(message, "file_send_intent", _client_attachments_internal__intent(record)) && cJSON_AddNumberToObject(extra, "receiver_id", (double)(record->view.peer >= 0 ? record->view.peer : record->view.channel)) && cJSON_AddNumberToObject(extra, "local_message_id", (double)record->view.local_id) && (record->view.picture || cJSON_AddStringToObject(extra, "file_header", record->header)) && (record->view.peer < 0 || cJSON_AddStringToObject(extra, "offer_id", record->view.offer));
}

/**
 * @brief prevents a selected destination from changing after a reconnect, channel move or client-ID reuse.
 * @param desktop_session_t* session -> protocol worker state
 * @param const desktop_attachment_record_t* record -> transfer metadata
 * @return boole -> TRUE when the originally selected destination remains available
 */
static boole _client_attachments_internal__valid(desktop_session_t *session, const desktop_attachment_record_t *record)
{
    int64 slot = desktop_session__client(session, record->view.peer);
    if (record->view.peer < 0)
    {
        return !session->model.idle && session->model.channel == record->view.channel;
    }
    return slot >= 0 && session->model.clients[slot].serial == record->view.peer_serial;
}

/**
 * @brief queues bounded file preparation with a copied destination and independent content keys.
 * @param desktop_session_t* session -> sole protocol worker
 * @param const desktop_attachment_request_t* request -> copied GUI selection
 * @return void
 */
void client_msg__attachments__send(desktop_session_t *session, const desktop_attachment_request_t *request)
{
    desktop_attachments_t *attachments = session->attachments;
    desktop_attachment_record_t *record = NULL;
    int64 slot = desktop_session__client(session, request->peer);
    int64 local = desktop_session__client(session, session->model.local_id);
    int64 index = 0;
    boole valid = FALSE;
    valid = (request->picture ? session->model.policy_pictures : session->model.policy_files) && (request->peer < 0 ? session->model.keys_ready && !session->model.idle && request->channel == session->model.channel : session->model.policy_private && slot >= 0 && session->model.clients[slot].serial == request->peer_serial && request->peer != session->model.local_id && !session->model.clients[slot].music && session->client_keys[slot][0]);
    if (!valid)
    {
        desktop_session__detail(session, "Attachment not sent: the selected destination or server feature is unavailable.", FALSE);
        return;
    }
    desktop_mutex__lock(attachments->mutex);
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        record = &attachments->records[index];
        if (record->view.outgoing && record->view.state > DESKTOP_ATTACHMENT_EMPTY && record->view.state < DESKTOP_ATTACHMENT_READY)
        {
            break;
        }
    }
    record = index == DESKTOP_ATTACHMENT_COUNT && !attachments->uploading ? desktop_attachments__reserve(attachments) : NULL;
    if (record)
    {
        record->view.outgoing = TRUE;
        record->view.picture = request->picture;
        record->view.peer = request->peer;
        record->view.peer_serial = request->peer_serial;
        record->view.channel = request->peer < 0 ? request->channel : -1;
        record->view.local_id = ++session->next_local_id;
        record->raw_limit = request->picture ? session->model.picture_limit : session->model.file_limit;
        if (record->raw_limit > (request->picture ? DESKTOP_ATTACHMENT_IMAGE_LIMIT : DESKTOP_ATTACHMENT_RAW_LIMIT))
        {
            record->raw_limit = request->picture ? DESKTOP_ATTACHMENT_IMAGE_LIMIT : DESKTOP_ATTACHMENT_RAW_LIMIT;
        }
        record->keys = session->keys;
        clib__string_copy(record->path, sizeof(record->path), request->path);
        if (request->peer >= 0)
        {
            clib__string_copy(record->public_key, sizeof(record->public_key), session->client_keys[slot]);
            server_msg__chat__append_direct(session, slot, "Attachment", record->view.local_id);
        }
        else
        {
            server_msg__chat__append(session, request->channel, local >= 0 ? session->model.clients[local].name : "You", "Attachment", record->view.local_id);
        }
        session->model.chats[session->model.chat_count - 1].attachment_id = record->view.id;
        record->view.state = DESKTOP_ATTACHMENT_PREPARING;
        clib__string_copy(record->view.detail, sizeof(record->view.detail), "Preparing attachment");
        record->job = ATTACHMENT_JOB_PACK;
    }
    desktop_mutex__unlock(attachments->mutex);
    if (!record)
    {
        desktop_session__detail(session, "Finish or cancel the current upload before sending another attachment.", FALSE);
    }
}

/**
 * @brief pumps at most one bounded control or 16 KiB chunk per session iteration; never waits for transport space.
 * @param desktop_session_t* session -> sole protocol worker
 * @return void
 */
void client_msg__attachments__tick(desktop_session_t *session)
{
    desktop_attachments_t *attachments = session->attachments;
    desktop_attachment_record_t *record = NULL;
    desktop_chat_preferences_t *prefs = &attachments->preferences;
    cJSON *message = NULL;
    char chunk[16385] = { 0 };
    uint64 count = 0;
    uint64 now = desktop_system__milliseconds();
    int64 index = 0;
    int64 operation = 0;
    boole complete = FALSE;
    boole sent = FALSE;
    desktop_mutex__lock(attachments->mutex);
    if (attachments->preferences_sent != attachments->preferences_revision)
    {
        message = client_msg__delivery__new_message("set_attachment_receive_preferences");
        complete = message && cJSON_AddBoolToObject(message, "block_direct_chat_pictures", prefs->block_direct_pictures) && cJSON_AddBoolToObject(message, "block_direct_chat_files", prefs->block_direct_files) && cJSON_AddBoolToObject(message, "block_channel_chat_pictures", prefs->block_channel_pictures) && cJSON_AddBoolToObject(message, "block_channel_chat_files", prefs->block_channel_files);
        operation = 1;
    }
    for (index = 0; !message && index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        record = &attachments->records[index];
        if (!record->view.id)
        {
            continue;
        }
        if (record->view.state < DESKTOP_ATTACHMENT_READY && record->view.state > DESKTOP_ATTACHMENT_EMPTY && (now > record->deadline || (record->view.outgoing && !_client_attachments_internal__valid(session, record))))
        {
            record->decision = 3;
        }
        if (record->busy)
        {
            if (record->decision == 3)
            {
                record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
            }
            continue;
        }
        if (attachments->uploading == record->view.id && record->view.state == DESKTOP_ATTACHMENT_CANCELLED)
        {
            if (record->acknowledged)
            {
                attachments->uploading = 0;
            }
            else if (now > record->deadline)
            {
                desktop_session__detail(session, "Upload acknowledgement timed out. Reconnect before uploading again.", TRUE);
            }
        }
        if (record->decision)
        {
            if (record->view.offer[0] && (record->offered || !record->view.outgoing))
            {
                message = client_msg__delivery__new_message("direct_attachment_decision");
                complete = message && cJSON_AddNumberToObject(message, "sender_id", (double)(record->view.outgoing ? session->model.local_id : record->view.peer)) && cJSON_AddStringToObject(message, "offer_id", record->view.offer) && cJSON_AddStringToObject(message, "action", record->view.outgoing ? "cancel" : record->decision == 1 ? "accept" : "reject");
                operation = 2;
            }
            else
            {
                record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
                record->job = ATTACHMENT_JOB_NONE;
                record->decision = 0;
                if (attachments->uploading == record->view.id && (record->offset < record->wire_size || record->acknowledged))
                {
                    attachments->uploading = 0;
                }
                desktop_attachment_record__release(record);
                clib__string_copy(record->view.detail, sizeof(record->view.detail), "Cancelled");
                attachments->revision++;
            }
        }
        else if (record->view.state == DESKTOP_ATTACHMENT_WAITING && record->view.outgoing && !record->offered)
        {
            message = client_msg__delivery__new_message("direct_attachment_offer");
            complete = message && cJSON_AddNumberToObject(message, "receiver_id", (double)record->view.peer) && cJSON_AddStringToObject(message, "offer_id", record->view.offer) && cJSON_AddBoolToObject(message, "is_picture", record->view.picture) && cJSON_AddStringToObject(message, "preview", record->preview);
            operation = 3;
        }
        else if (record->view.state == DESKTOP_ATTACHMENT_UPLOADING && !record->job && (!attachments->uploading || attachments->uploading == record->view.id))
        {
            if (record->acknowledged)
            {
                message = client_msg__delivery__new_message("file_send_completed");
                complete = message && _client_attachments_internal__extra(message, record);
                operation = 5;
            }
            else if (record->wire && record->offset < record->wire_size)
            {
                count = record->wire_size - record->offset;
                count = count > 16384 ? 16384 : count;
                clib__string_copy_n(chunk, sizeof(chunk), record->wire + record->offset, count);
                message = client_msg__delivery__new_message("file_send");
                complete = message && cJSON_AddNumberToObject(message, "total_bytes_length", (double)record->wire_size) && cJSON_AddStringToObject(message, "data_part_base64", chunk) && cJSON_AddBoolToObject(message, "is_new_file", record->offset == 0) && (record->offset || _client_attachments_internal__extra(message, record));
                operation = 4;
            }
        }
    }
    if (message && complete)
    {
        // The mutex protects copied chunk data and state; this send only queues a bounded message.
        sent = client_msg__delivery__try_send(session, message);
        message = NULL;
    }
    crypto_session__free_json(message);
    if (sent)
    {
        if (operation == 1)
        {
            attachments->preferences_sent = attachments->preferences_revision;
        }
        else if (operation == 2)
        {
            if (record->decision == 1)
            {
                record->accepted = TRUE;
                record->view.state = DESKTOP_ATTACHMENT_WAITING;
                clib__string_copy(record->view.detail, sizeof(record->view.detail), "Accepted; waiting for the sender");
            }
            else
            {
                if (attachments->uploading == record->view.id && (record->offset < record->wire_size || record->acknowledged))
                {
                    attachments->uploading = 0;
                }
                desktop_attachment_record__release(record);
                record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
                clib__string_copy(record->view.detail, sizeof(record->view.detail), "Cancelled or declined");
            }
            record->decision = 0;
        }
        else if (operation == 3)
        {
            record->offered = TRUE;
            crypto_session__free_text(record->preview);
            record->preview = NULL;
        }
        else if (operation == 4)
        {
            attachments->uploading = record->view.id;
            record->offset += count;
            record->view.progress = (int64)(record->offset * 100 / record->wire_size);
        }
        else if (operation == 5)
        {
            crypto_session__free_text(record->wire);
            crypto_session__free_text(record->header);
            record->wire = NULL;
            record->header = NULL;
            record->wire_capacity = 0;
            record->wire_size = 0;
            record->completion_sent = TRUE;
            record->view.state = DESKTOP_ATTACHMENT_READY;
            record->view.progress = 100;
            attachments->uploading = 0;
            clib__string_copy(record->view.detail, sizeof(record->view.detail), "Uploaded");
        }
        attachments->revision++;
    }
    desktop_mutex__unlock(attachments->mutex);
    mbedtls_platform_zeroize(chunk, sizeof(chunk));
}
