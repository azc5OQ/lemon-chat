#include "attachments/attachments_internal.h"
#include "session/session_internal.h"
#include "msg/server/server_msg.h"
#include "platform/system.h"
#include <stdlib.h>

// declarations
static void _server_attachments_internal__offer(desktop_session_t *session, const cJSON *message);
static void _server_attachments_internal__metadata(desktop_session_t *session, const cJSON *message, boole direct, boole picture);
static void _server_attachments_internal__append(desktop_session_t *session, desktop_attachment_record_t *record, int64 slot);
static boole _server_attachments_internal__blocked(const desktop_chat_preferences_t *preferences, boole direct, boole picture);

/**
 * @brief applies local receiving choices even when the server cannot avoid sending the payload.
 * @param const desktop_chat_preferences_t* preferences -> locked local choices
 * @param boole direct -> private transfer
 * @param boole picture -> image rather than arbitrary file
 * @return boole -> TRUE when locally blocked
 */
static boole _server_attachments_internal__blocked(const desktop_chat_preferences_t *preferences, boole direct, boole picture)
{
    return direct ? (picture ? preferences->block_direct_pictures : preferences->block_direct_files) : (picture ? preferences->block_channel_pictures : preferences->block_channel_files);
}

/**
 * @brief creates one chat row referencing a stable record ID rather than a payload pointer.
 * @param desktop_session_t* session -> protocol worker
 * @param desktop_attachment_record_t* record -> locked record
 * @param int64 slot -> validated sender slot
 * @return void
 */
static void _server_attachments_internal__append(desktop_session_t *session, desktop_attachment_record_t *record, int64 slot)
{
    if (record->view.peer >= 0)
    {
        server_msg__chat__append_direct(session, slot, "Attachment", 0);
    }
    else
    {
        server_msg__chat__append(session, record->view.channel, session->model.clients[slot].name, "Attachment", 0);
    }
    session->model.chats[session->model.chat_count - 1].attachment_id = record->view.id;
}

/**
 * @brief validates direct-offer routing and queues private decryption before enabling Accept.
 * @param desktop_session_t* session -> sole protocol worker
 * @param const cJSON* message -> borrowed server message
 * @return void
 */
static void _server_attachments_internal__offer(desktop_session_t *session, const cJSON *message)
{
    desktop_attachments_t *attachments = session->attachments;
    desktop_attachment_record_t *record = NULL_POINTER;
    const char *id = server_msg__read_string(message, "offer_id");
    const char *status = server_msg__read_string(message, "status");
    const char *preview = server_msg__read_string(message, "preview");
    int64 sender = server_msg__read_number(message, "sender_id");
    int64 slot = desktop_session__client(session, sender);
    int64 index = 0;
    uint64 length = clib__strlen(preview);
    boole picture = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "is_picture"));
    if (clib__strlen(id) != 32 || (slot >= 0 && session->model.clients[slot].ignored))
    {
        return;
    }
    for (index = 0; index < 32; index++)
    {
        if (!clib__strchr("0123456789abcdef", id[index]))
        {
            return;
        }
    }
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        if (clib__strcmp(attachments->records[index].view.offer, id) == 0)
        {
            record = &attachments->records[index];
            break;
        }
    }
    if (clib__strcmp(status, "offered") == 0 && !record && slot >= 0 && sender != session->model.local_id && length > 0 && length <= 49152)
    {
        record = desktop_attachments__reserve(attachments);
        if (!record)
        {
            return;
        }
        record->view.peer = sender;
        record->view.peer_serial = session->model.clients[slot].serial;
        record->view.picture = picture;
        record->sender = sender;
        record->sender_serial = record->view.peer_serial;
        clib__string_copy(record->view.offer, sizeof(record->view.offer), id);
        record->view.state = DESKTOP_ATTACHMENT_PREPARING;
        _server_attachments_internal__append(session, record, slot);
        if (_server_attachments_internal__blocked(&attachments->preferences, TRUE, picture))
        {
            desktop_attachment_record__fail(record, "Receiving this attachment type is disabled in local settings.");
            record->decision = 2;
            return;
        }
        record->preview = calloc(1, length + 1);
        if (!record->preview)
        {
            desktop_attachment_record__fail(record, "Could not allocate the attachment offer.");
            record->decision = 2;
            return;
        }
        clib__string_copy(record->preview, length + 1, preview);
        record->job = ATTACHMENT_JOB_OFFER;
        clib__string_copy(record->view.detail, sizeof(record->view.detail), "Reading attachment offer");
    }
    else if (record && sender == (record->view.outgoing ? session->model.local_id : record->view.peer))
    {
        if (!record->busy && clib__strcmp(status, "accepted") == 0 && record->view.outgoing && record->view.state == DESKTOP_ATTACHMENT_WAITING)
        {
            record->accepted = TRUE;
            record->view.state = DESKTOP_ATTACHMENT_UPLOADING;
            clib__string_copy(record->view.detail, sizeof(record->view.detail), "Uploading");
        }
        else if (record->view.state < DESKTOP_ATTACHMENT_READY && (clib__strcmp(status, "rejected") == 0 || clib__strcmp(status, "cancelled") == 0 || clib__strcmp(status, "expired") == 0 || clib__strcmp(status, "unavailable") == 0 || clib__strcmp(status, "busy") == 0))
        {
            record->decision = 3;
            if (record->busy)
            {
                record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
            }
            clib__string_copy(record->view.detail, sizeof(record->view.detail), status);
        }
    }
}

/**
 * @brief bounds total encrypted storage and captures keys before accepting incoming chunks.
 * @param desktop_session_t* session -> protocol worker
 * @param const cJSON* message -> borrowed metadata
 * @param boole direct -> private destination
 * @param boole picture -> image payload
 * @return void
 */
static void _server_attachments_internal__metadata(desktop_session_t *session, const cJSON *message, boole direct, boole picture)
{
    desktop_attachments_t *attachments = session->attachments;
    desktop_attachment_record_t *record = NULL_POINTER;
    desktop_attachment_record_t *candidate = NULL_POINTER;
    int64 sender = server_msg__read_number(message, "sender_id");
    int64 slot = desktop_session__client(session, sender);
    int64 channel = direct ? -1 : server_msg__read_number(message, "channel_id");
    int64 server_id = server_msg__read_number(message, picture ? "picture_id" : "file_id");
    int64 length = server_msg__read_number(message, picture ? "size" : "encrypted_size");
    const char *header = server_msg__read_string(message, "file_header");
    uint64 header_length = clib__strlen(header);
    uint64 total = 0;
    int64 active = 0;
    int64 index = 0;
    boole reuse = FALSE;
    if (slot < 0 || session->model.clients[slot].ignored || sender == session->model.local_id || server_id < 0 || length <= 0 || (uint64)length > DESKTOP_ATTACHMENT_WIRE_LIMIT || (!direct && (channel != session->model.channel || !session->model.keys_ready)) || (!picture && (!header_length || header_length > 16384)) || _server_attachments_internal__blocked(&attachments->preferences, direct, picture))
    {
        return;
    }
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        candidate = &attachments->records[index];
        if (!candidate->view.outgoing && candidate->view.state == DESKTOP_ATTACHMENT_RECEIVING)
        {
            active++;
            total += candidate->wire_size;
            if (candidate->sender == sender && candidate->view.server_id == server_id)
            {
                return;
            }
        }
        if (direct && !candidate->view.outgoing && candidate->view.peer == sender && candidate->view.peer_serial == session->model.clients[slot].serial && candidate->accepted && candidate->view.state == DESKTOP_ATTACHMENT_WAITING && candidate->view.picture == picture && !candidate->busy)
        {
            record = candidate;
        }
    }
    if (active >= 2 || (uint64)length > DESKTOP_ATTACHMENT_CACHE_LIMIT - total)
    {
        desktop_session__detail(session, "Incoming attachment skipped: transfer memory limit reached.", FALSE);
        return;
    }
    reuse = record != NULL;
    record = record ? record : desktop_attachments__reserve(attachments);
    if (!record)
    {
        return;
    }
    record->view.picture = picture;
    record->view.peer = direct ? sender : -1;
    record->view.peer_serial = direct ? session->model.clients[slot].serial : 0;
    record->view.channel = channel;
    record->view.server_id = server_id;
    record->sender = sender;
    record->sender_serial = session->model.clients[slot].serial;
    record->wire_size = (uint64)length;
    record->wire_capacity = (uint64)length + 1;
    record->wire = calloc(1, record->wire_capacity);
    if (!picture)
    {
        record->header = calloc(1, header_length + 1);
        if (record->header)
        {
            clib__string_copy(record->header, header_length + 1, header);
        }
    }
    if (!reuse)
    {
        _server_attachments_internal__append(session, record, slot);
    }
    if (!record->wire || (!picture && !record->header))
    {
        desktop_attachment_record__fail(record, "Could not allocate this transfer.");
        return;
    }
    record->keys = session->keys;
    record->previous = session->previous_keys;
    record->have_previous = session->have_previous;
    record->view.state = DESKTOP_ATTACHMENT_RECEIVING;
    record->deadline = desktop_system__milliseconds() + 600000;
    clib__string_copy(record->view.detail, sizeof(record->view.detail), "Receiving");
}

/**
 * @brief consumes only announced, length-checked transfers and matching upload acknowledgements.
 * @param desktop_session_t* session -> authenticated protocol worker
 * @param const cJSON* message -> borrowed server message
 * @return void
 */
void server_msg__attachments__receive(desktop_session_t *session, const cJSON *message)
{
    desktop_attachments_t *attachments = session->attachments;
    desktop_attachment_record_t *record = NULL_POINTER;
    const char *type = server_msg__read_string(message, "type");
    const char *value = server_msg__read_string(message, "value");
    const char *expected = NULL_POINTER;
    int64 sender = server_msg__read_number(message, "sender_id");
    int64 server_id = server_msg__read_number(message, "server_chat_message_id");
    int64 slot = desktop_session__client(session, sender);
    int64 local_id = server_msg__read_number(message, "local_message_id");
    int64 index = 0;
    uint64 length = 0;
    desktop_mutex__lock(attachments->mutex);
    if (clib__strcmp(type, "direct_attachment_offer") == 0)
    {
        _server_attachments_internal__offer(session, message);
    }
    else if (clib__strcmp(type, "file_send_success") == 0)
    {
        record = desktop_attachments__find(attachments, attachments->uploading);
        if (record && (record->view.state == DESKTOP_ATTACHMENT_CANCELLED || (record->offset == record->wire_size && record->offset)))
        {
            record->acknowledged = TRUE;
        }
    }
    else if (clib__strcmp(type, "file_send_error") == 0)
    {
        for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
        {
            record = &attachments->records[index];
            if (record->view.outgoing && !record->busy && record->view.local_id == (uint64)local_id && local_id > 0)
            {
                if (attachments->uploading == record->view.id)
                {
                    attachments->uploading = 0;
                }
                desktop_attachment_record__fail(record, server_msg__read_string(message, "reason"));
                break;
            }
        }
    }
    else if (clib__strcmp(type, "channel_chat_picture_metadata") == 0 || clib__strcmp(type, "direct_chat_picture_metadata") == 0)
    {
        _server_attachments_internal__metadata(session, message, type[0] == 'd', TRUE);
    }
    else if (clib__strcmp(type, "channel_chat_file_metadata") == 0 || clib__strcmp(type, "direct_chat_file_metadata") == 0)
    {
        _server_attachments_internal__metadata(session, message, type[0] == 'd', FALSE);
    }
    else if (clib__strcmp(type, "file_receive_chunk") == 0 || clib__strcmp(type, "file_receive_completed") == 0)
    {
        for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
        {
            record = &attachments->records[index];
            if (!record->view.outgoing && !record->busy && !record->job && record->view.state == DESKTOP_ATTACHMENT_RECEIVING && record->sender == sender && record->view.server_id == server_id && slot >= 0 && record->sender_serial == session->model.clients[slot].serial)
            {
                break;
            }
        }
        if (index == DESKTOP_ATTACHMENT_COUNT)
        {
            desktop_mutex__unlock(attachments->mutex);
            return;
        }
        if (clib__strcmp(type, "file_receive_chunk") == 0)
        {
            length = clib__strlen(value);
            if (!length || length > 65536 || record->received > record->wire_size || length > record->wire_size - record->received)
            {
                desktop_attachment_record__fail(record, "Invalid attachment chunk length.");
            }
            else
            {
                clib__memcpy(record->wire + record->received, value, length);
                record->received += length;
                record->view.progress = (int64)(record->received * 100 / record->wire_size);
            }
        }
        else
        {
            expected = record->view.peer >= 0 ? (record->view.picture ? "direct_chat_picture" : "direct_chat_file") : (record->view.picture ? "channel_chat_picture" : "channel_chat_file");
            if (record->received != record->wire_size || clib__strcmp(expected, server_msg__read_string(message, "receive_type")) != 0)
            {
                desktop_attachment_record__fail(record, "Incomplete attachment or mismatched transfer type.");
            }
            else
            {
                record->job = ATTACHMENT_JOB_DECODE;
                clib__string_copy(record->view.detail, sizeof(record->view.detail), "Decrypting attachment");
            }
        }
    }
    else
    {
        desktop_mutex__unlock(attachments->mutex);
        return;
    }
    attachments->revision++;
    desktop_mutex__unlock(attachments->mutex);
}
