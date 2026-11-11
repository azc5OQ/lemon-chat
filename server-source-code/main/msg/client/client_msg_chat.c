// This file checks incoming chat messages, edits, typing notifications, pokes and URL preview requests.
#include "client_msg.h"
#include "../../core/permissions.h"
#include <string.h>
#include "../../core/identity_store.h"
#include "../server/server_msg.h"
#include "../../httpclient/url_resolver.h"
#include "../../httpclient/httpclient.h"
#include "../../base.h"
#include "../../util.h"
#include "../../clib/clib.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"

// The purposes a direct message can name in its "purpose" field.
#define DIRECT_MESSAGE_PURPOSE_CONVERSATION "conversation"
#define DIRECT_MESSAGE_PURPOSE_CHANNEL_KEYS "channel_keys"
#define DIRECT_MESSAGE_PURPOSE_SEEN_RECEIPT "seen_receipt"

// declarations
static cstring _client_msg_chat_internal__read_direct_message_purpose(cJSON *json_message_object);
static cstring _client_msg_chat_internal__channel_message_refusal(client_t *sender, channel_t *channel);
static boole _client_msg_chat_internal__is_json_chat_message_format_valid(cJSON *json_root, uint64 client_id);
static void _client_msg_chat_internal__process_chat_message_action(cJSON *json_root, uint64 sender_client_id, char *outbound_action_type, boole is_edit);
static boole _client_msg_chat_internal__is_json_poke_client_request_format_valid(cJSON *json_root, uint64 client_id);
static boole _client_msg_chat_internal__is_offline_chat_message_valid(cJSON *json_root);
static boole _client_msg_chat_internal__is_call_idle_client_message_valid(cJSON *json_root);

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @note this function is used for processing four different kinds of client messages, direct chat message, channel chat message, direct chat picture, channel chat picture
 *
 * @return boole
 */
static boole _client_msg_chat_internal__is_json_chat_message_format_valid(cJSON *json_root, uint64 client_id)
{
    cJSON *json_chat_message_value = NULL_POINTER;
    cJSON *json_receiver_id = NULL_POINTER;
    cJSON *json_local_message_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    uint64 validated_id = 0;
    uint64 receiver_count = g_server_settings.max_client_count > g_server_settings.max_channel_count ? g_server_settings.max_client_count : g_server_settings.max_channel_count;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsString(json_chat_message_value) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_chat_message_value) \n");
        return FALSE;
    }

    if (json_chat_message_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_chat_message_value->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_chat_message_value->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_chat_message_value->valuestring) \n");
        return FALSE;
    }

    // reject an oversized value loudly. the server re-encrypts the forwarded message, and if its base64 output
    // would exceed the websocket frame limit, base__encrypt returns NULL and the send loop silently skips every
    // recipient while the sender still gets a delivery ack. base64 grows ~4/3; leave 2KB for the added sender
    // fields + envelope overhead. derived from the frame limit so it tracks the websocket_message_max_length setting.
    if (clib__utf8_string_length_check_max_length(json_chat_message_value->valuestring, (int64)((g_server_settings.websocket_message_max_length * 3) / 4 - 2048)) == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " chat message value too large \n");
        return FALSE;
    }

    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
    if (receiver_count == 0 || util__json_read_uint64(json_receiver_id, receiver_count - 1, &validated_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_receiver_id) \n");
        return FALSE;
    }

    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");
    if (util__json_read_uint64(json_local_message_id, 9007199254740991ULL, &validated_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_local_message_id) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Reads what a direct message is for, as named by its sender.
 *
 *        The content of a direct message is encrypted for the receiver, so the server cannot
 *        tell what two clients say to each other from the channel keys that a channel's
 *        maintainer hands out, or from a "message seen" receipt. The sender names the purpose
 *        in the request. The server applies the rules of that purpose and passes the purpose on
 *        with the message. The receiving client reads the content only as what the purpose
 *        says, so a text message sent as "channel_keys" is not shown as text.
 *
 *        "conversation" covers everything that belongs to a conversation between two clients:
 *        text, edits, reactions and the notice that one side closed the conversation's window.
 *        The server cannot tell these apart and does not need to.
 *
 *        The purpose tells the server which kind of direct message it relays. That is the price
 *        of being able to refuse direct texts by permission and power; the content stays unreadable.
 *
 * @param cJSON* json_message_object -> the "message" object of the request
 *
 * @return cstring -> DIRECT_MESSAGE_PURPOSE_CHANNEL_KEYS, DIRECT_MESSAGE_PURPOSE_SEEN_RECEIPT or DIRECT_MESSAGE_PURPOSE_CONVERSATION; a request that names no purpose or an unknown one is handled as conversation content
 */
static cstring _client_msg_chat_internal__read_direct_message_purpose(cJSON *json_message_object)
{
    cJSON *json_purpose = cJSON_GetObjectItemCaseSensitive(json_message_object, "purpose");

    // Default is conversation.
    if (cJSON_IsString(json_purpose) == FALSE || json_purpose->valuestring == NULL_POINTER)
    {
        return DIRECT_MESSAGE_PURPOSE_CONVERSATION;
    }

    // Known purposes.
    if (clib__strcmp(json_purpose->valuestring, DIRECT_MESSAGE_PURPOSE_CHANNEL_KEYS) == 0)
    {
        return DIRECT_MESSAGE_PURPOSE_CHANNEL_KEYS;
    }
    if (clib__strcmp(json_purpose->valuestring, DIRECT_MESSAGE_PURPOSE_SEEN_RECEIPT) == 0)
    {
        return DIRECT_MESSAGE_PURPOSE_SEEN_RECEIPT;
    }
    return DIRECT_MESSAGE_PURPOSE_CONVERSATION;
}

/**
 * @brief This function processes direct chat message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_direct_chat_message(cJSON *json_root, uint64 sender_client_id)
{
    boole is_message_valid = FALSE;
    boole is_receiver_existing = FALSE;
    boole is_receiver_idle = FALSE;
    boole is_receiver_music_bot = FALSE;
    boole is_receipt = FALSE;
    cJSON *json_receiver_id = NULL_POINTER;
    cJSON *json_local_message_id = NULL_POINTER;
    cJSON *json_chat_message_value = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    client_t *sender = NULL_POINTER;
    client_t *receiver = NULL_POINTER;
    cstring purpose = NULL_POINTER;
    cstring refusal = NULL_POINTER;
    uint64 server_chat_message_id = 0;

    // because maintainer of channel sends out channel keys to each client individually,
    // and a maintainer needs to do that as quickly as possible, repeatedly, possibly for a hundred clients when everyone is in the maintainer's channel
    // spam prevention cannot be placed here like in other places, needs to be improved
    is_message_valid = _client_msg_chat_internal__is_json_chat_message_format_valid(json_root, sender_client_id);

    if (is_message_valid == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_direct_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
        return;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_direct_chat_message got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

    // Check the recipient index before reading the clients array.
    if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__chat__process_direct_chat_message client : ", sender_client_id, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count \n");
        return;
    }

    // Lock the clients.
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // Sender and receiver.
    sender = &g_clients_array[sender_client_id];
    receiver = &g_clients_array[json_receiver_id->valueint];

    // Read the purpose.
    purpose = _client_msg_chat_internal__read_direct_message_purpose(json_message_object);

    // Read the recipient state while holding the clients lock.
    is_receiver_existing = receiver->is_authenticated;
    is_receiver_idle = receiver->is_idle;
    is_receiver_music_bot = receiver->is_music_bot;

    // Channel keys.
    if (clib__strcmp(purpose, DIRECT_MESSAGE_PURPOSE_CHANNEL_KEYS) == 0)
    {
        // Channel keys always reach the members of the sender's channel; no other check applies.
        if (is_receiver_existing == FALSE || sender->is_idle == TRUE || is_receiver_idle == TRUE || sender->channel_id != receiver->channel_id)
        {
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }
    }
    else
    {
        // Private messaging can be switched off for the whole server; music bots still receive messages.
        if (g_server_settings.is_private_messaging_allowed == FALSE && is_receiver_music_bot == FALSE)
        {
            DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__chat__process_direct_chat_message client : ", sender_client_id, " private messages are disabled on this server \n");
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }

        // A read receipt needs no permission; it is dropped without a notice when it cannot be delivered.
        is_receipt = (boole)(clib__strcmp(purpose, DIRECT_MESSAGE_PURPOSE_SEEN_RECEIPT) == 0);

        // Require both the server setting and permission to message an idle recipient.
        if (is_receiver_idle == TRUE &&
            (g_server_settings.is_sending_text_to_idle_clients_allowed == FALSE || permissions__is_allowed(sender, PERMISSION_MESSAGE_IDLE_CLIENTS) == FALSE))
        {
            if (is_receipt == FALSE)
            {
                server_msg__administration__send_permission_denied_to_single_client(sender, permissions__name(PERMISSION_MESSAGE_IDLE_CLIENTS));
            }
            clib__unlock(&g_clients_global_rwlock_guard);
            return;
        }

        // The sender needs the permission and enough power against the receiver's protection.
        if (is_receipt == FALSE && is_receiver_existing == TRUE && is_receiver_music_bot == FALSE)
        {
            refusal = permissions__contact_refusal(sender, receiver, PERMISSION_SEND_DIRECT_MESSAGES);
            if (refusal != NULL_POINTER)
            {
                server_msg__administration__send_permission_denied_to_single_client(sender, refusal);
                clib__unlock(&g_clients_global_rwlock_guard);
                return;
            }
        }
    }

    // Forward the message.
    if (is_receiver_existing == TRUE &&
        (is_receiver_idle == FALSE || g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE) &&
        is_receiver_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__chat__process_direct_chat_message receiver is_existing: ", json_receiver_id->valueint, "\n");

        // Assign a server message ID, acknowledge it and forward the encrypted content.
        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(sender, server_chat_message_id, (uint64)json_local_message_id->valuedouble);
        server_msg__chat__send_message_to_single_client(receiver, sender, server_chat_message_id, json_chat_message_value->valuestring, purpose);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__chat__process_direct_chat_message receiver is_existing not is_existing or is in idle mode", json_receiver_id->valueint, "\n");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks whether a client may send or edit text in the channel it is in.
 *
 * @param client_t* sender -> client making the request, with the clients lock held
 * @param channel_t* channel -> the sender's channel, with the channels lock held
 *
 * @return cstring -> NULL_POINTER when allowed; otherwise the name of the permission or power that refused it
 */
static cstring _client_msg_chat_internal__channel_message_refusal(client_t *sender, channel_t *channel)
{
    if (permissions__is_allowed(sender, PERMISSION_SEND_CHANNEL_MESSAGES) == FALSE)
    {
        return permissions__name(PERMISSION_SEND_CHANNEL_MESSAGES);
    }
    if (permissions__can_use_channel(sender, channel, PERMISSION_CHANNEL_MESSAGE_POWER) == FALSE)
    {
        return permissions__power_name(PERMISSION_CHANNEL_MESSAGE_POWER);
    }
    return NULL_POINTER;
}

/**
 * @brief This function processes a channel chat message, broadcasting it to clients in the sender's
 *        channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_channel_chat_message(cJSON *json_root, uint64 sender_client_id)
{
    boole is_channel_existing = FALSE;

    // cJSON* json_receiver_id = 0; json contains this ID, but it is not needed. Id of channel is taken from sender's client struct
    cJSON *json_local_message_id = NULL_POINTER;
    cJSON *json_chat_message_value = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cstring refusal = NULL_POINTER;
    uint64 channel_id = 0;
    uint64 server_chat_message_id = 0;

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_channel_chat_message base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_chat_internal__is_json_chat_message_format_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_channel_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
        return;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_channel_chat_message got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    clib__read_lock(&g_channels_global_rwlock_guard);

    channel_id = g_clients_array[sender_client_id].channel_id;
    is_channel_existing = channel_id < g_server_settings.max_channel_count && g_channel_array[channel_id].is_existing;

    if (is_channel_existing == TRUE)
    {
        refusal = _client_msg_chat_internal__channel_message_refusal(&g_clients_array[sender_client_id], &g_channel_array[channel_id]);
        if (refusal != NULL_POINTER)
        {
            server_msg__administration__send_permission_denied_to_single_client(&g_clients_array[sender_client_id], refusal);
            goto channel_message_done;
        }
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiving channel exists: ", channel_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(&g_clients_array[sender_client_id], server_chat_message_id, (uint64)json_local_message_id->valuedouble);
        server_msg__chat__send_message_to_clients_in_same_channel(&g_clients_array[sender_client_id], channel_id, server_chat_message_id, json_chat_message_value->valuestring);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiving channel does not exist: ", channel_id, "\n");
    }

channel_message_done:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief shared worker for delete/edit chat message requests, it rate-limits, stamps the
 *        requester's identity and rebroadcasts the action to the right audience
 *
 *        the client sends only the target message id (plus the new value for an edit) and the
 *        receiver context. the server keeps no message state, so the action goes back out either
 *        to the sender's channel, or to the private counterpart plus the requester. each receiving
 *        client then decides for itself whether to honour it, based on the requester's public key
 *        and admin flag.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 * @param char* outbound_action_type -> "chat_message_delete" or "chat_message_edit"
 * @param boole is_edit -> TRUE for edit (reads and validates new_message_value), FALSE for delete
 *
 * @return void
 */
static void _client_msg_chat_internal__process_chat_message_action(cJSON *json_root, uint64 sender_client_id, char *outbound_action_type, boole is_edit)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_message_id = NULL_POINTER;
    cJSON *json_receiver_type = NULL_POINTER;
    cJSON *json_receiver_id = NULL_POINTER;
    cJSON *json_new_message_value = NULL_POINTER;
    char *receiver_type = NULL_POINTER;
    char *new_message_value = NULL_POINTER;
    char *requester_public_key = NULL_POINTER;
    cstring refusal = NULL_POINTER;
    uint64 target_chat_message_id = 0;
    uint64 channel_id = 0;
    uint64 receiver_id = 0;
    boole requester_is_admin = FALSE;

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "message_id");
    json_receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");

    if (util__json_read_uint64(json_message_id, 9007199254740991ULL, &target_chat_message_id) == FALSE || cJSON_IsString(json_receiver_type) == FALSE)
    {
        return;
    }

    receiver_type = json_receiver_type->valuestring;

    if (is_edit == TRUE)
    {
        json_new_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "new_message_value");
        if (cJSON_IsString(json_new_message_value) == FALSE || json_new_message_value->valuestring == NULL_POINTER)
        {
            return;
        }

        if (clib__utf8_string_length(json_new_message_value->valuestring) == 0)
        {
            return;
        }

        // same oversize guard a normal chat message gets (see _client_msg_chat_internal__is_json_chat_message_format_valid)
        if (clib__utf8_string_length_check_max_length(json_new_message_value->valuestring, (int64)((g_server_settings.websocket_message_max_length * 3) / 4 - 2048)) == -1)
        {
            return;
        }

        new_message_value = json_new_message_value->valuestring;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    requester_public_key = g_clients_array[sender_client_id].public_key;
    requester_is_admin = g_clients_array[sender_client_id].is_admin;

    if (clib__is_string_equal(receiver_type, "user") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
        if (g_server_settings.max_client_count > 0 && util__json_read_uint64(json_receiver_id, g_server_settings.max_client_count - 1, &receiver_id) == TRUE)
        {
            if (g_clients_array[receiver_id].is_existing == TRUE && g_clients_array[receiver_id].is_authenticated == TRUE && g_clients_array[receiver_id].is_music_bot == FALSE)
            {
                // An edit carries new content and follows the same delivery policy as a new message.
                // Deletion remains available when sending new private content has been disabled.
                if (is_edit == TRUE && (g_server_settings.is_private_messaging_allowed == FALSE || (g_clients_array[receiver_id].is_idle == TRUE && (g_server_settings.is_sending_text_to_idle_clients_allowed == FALSE || permissions__is_allowed(&g_clients_array[sender_client_id], PERMISSION_MESSAGE_IDLE_CLIENTS) == FALSE))))
                {
                    server_msg__administration__send_permission_denied_to_single_client(&g_clients_array[sender_client_id], permissions__name(PERMISSION_MESSAGE_IDLE_CLIENTS));
                    clib__unlock(&g_clients_global_rwlock_guard);
                    return;
                }

                // an edit carries new text, so it needs the same rights as a new direct message
                refusal = is_edit == TRUE ? permissions__contact_refusal(&g_clients_array[sender_client_id], &g_clients_array[receiver_id], PERMISSION_SEND_DIRECT_MESSAGES) : NULL_POINTER;
                if (refusal != NULL_POINTER)
                {
                    server_msg__administration__send_permission_denied_to_single_client(&g_clients_array[sender_client_id], refusal);
                    clib__unlock(&g_clients_global_rwlock_guard);
                    return;
                }

                // deliver to the private counterpart and back to the requester so both views update
                server_msg__chat__send_message_action_to_single_client(&g_clients_array[receiver_id], outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
                server_msg__chat__send_message_action_to_single_client(&g_clients_array[sender_client_id], outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
            }
        }
    }
    else
    {
        clib__read_lock(&g_channels_global_rwlock_guard);
        channel_id = g_clients_array[sender_client_id].channel_id;
        if (channel_id < g_server_settings.max_channel_count && g_channel_array[channel_id].is_existing == TRUE)
        {
            refusal = is_edit == TRUE ? _client_msg_chat_internal__channel_message_refusal(&g_clients_array[sender_client_id], &g_channel_array[channel_id]) : NULL_POINTER;
            if (refusal != NULL_POINTER)
            {
                server_msg__administration__send_permission_denied_to_single_client(&g_clients_array[sender_client_id], refusal);
            }
            else
            {
                server_msg__chat__send_message_action_to_clients_in_same_channel(channel_id, outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
            }
        }
        clib__unlock(&g_channels_global_rwlock_guard);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a request to delete a chat message (rebroadcasts a delete action to the right
 *        audience)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_delete_chat_message_request(cJSON *json_root, uint64 sender_client_id)
{
    _client_msg_chat_internal__process_chat_message_action(json_root, sender_client_id, "chat_message_delete", FALSE);
}

/**
 * @brief processes a request to edit a chat message (rebroadcasts an edit action to the right
 *        audience)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_edit_chat_message_request(cJSON *json_root, uint64 sender_client_id)
{
    _client_msg_chat_internal__process_chat_message_action(json_root, sender_client_id, "chat_message_edit", TRUE);
}

/**
 * @brief verifies if received json has needed keys and values
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 client_id -> id of the client
 *
 * @return boole
 */
static boole _client_msg_chat_internal__is_json_poke_client_request_format_valid(cJSON *json_root, uint64 client_id)
{
    uint64 validated_id = 0;
    cJSON *json_poke_message = NULL_POINTER;
    cJSON *json_receiver_id = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (g_server_settings.max_client_count == 0 || util__json_read_uint64(json_receiver_id, g_server_settings.max_client_count - 1, &validated_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_receiver_id) \n");
        return FALSE;
    }

    if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count \n");
        return FALSE;
    }

    json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
    if (cJSON_IsString(json_poke_message) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsString(json_poke_message) \n");
        return FALSE;
    }

    if (json_poke_message->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " json_poke_message->valuestring == NULL \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_poke_message->valuestring) == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " clib__utf8_string_length(json_poke_message->valuestring) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function processes a poke-client request, forwarding the poke message to the target
 *        client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_poke_client_request(cJSON *json_root, uint64 sender_client_id)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_receiver_id = NULL_POINTER;
    cJSON *json_poke_message = NULL_POINTER;
    cstring refusal = NULL_POINTER;

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_poke_client_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    if (_client_msg_chat_internal__is_json_poke_client_request_format_valid(json_root, sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_poke_client_request client_msg__is_json_poke_client_request_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    if (g_clients_array[json_receiver_id->valueint].is_authenticated == TRUE)
    {
        // The sender needs the poke permission and enough poke power against the receiver's protection.
        refusal = permissions__contact_refusal(&g_clients_array[sender_client_id], &g_clients_array[json_receiver_id->valueint], PERMISSION_SEND_POKES);
        if (refusal != NULL_POINTER)
        {
            server_msg__administration__send_permission_denied_to_single_client(&g_clients_array[sender_client_id], refusal);
        }
        else
        {
            server_msg__chat__send_poke_to_single_client(&g_clients_array[json_receiver_id->valueint], &g_clients_array[sender_client_id], json_poke_message->valuestring);
        }
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief passes on "this client is typing" to the conversation the client is writing to. the
 *        request carries no message content and nothing is stored - it is forwarded to whoever is
 *        on the other side and forgotten. the whole feature is off unless the server allows it.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that is typing
 *
 * @attention this function uses read lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__chat__process_typing_indicator_request(cJSON *json_root, uint64 sender_client_id)
{
    boole is_direct_message = FALSE;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_receiver_type = NULL_POINTER;
    cJSON *json_receiver_id = NULL_POINTER;
    uint64 receiver_id = 0;
    uint64 receiver_count = 0;

    if (g_server_settings.is_typing_indicator_allowed == FALSE)
    {
        return;
    }

    if (base__is_request_allowed_based_on_spam_protection(sender_client_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_typing_indicator_request spam protection == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return;
    }

    json_receiver_type = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_type");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");

    if (cJSON_IsString(json_receiver_type) == FALSE || json_receiver_type->valuestring == NULL_POINTER)
    {
        return;
    }

    is_direct_message = clib__is_string_equal(json_receiver_type->valuestring, "user");

    if (is_direct_message == FALSE && clib__is_string_equal(json_receiver_type->valuestring, "channel") == FALSE)
    {
        return; // only those two kinds of conversation exist
    }

    receiver_count = is_direct_message == TRUE ? g_server_settings.max_client_count : g_server_settings.max_channel_count;
    if (receiver_count == 0 || util__json_read_uint64(json_receiver_id, receiver_count - 1, &receiver_id) == FALSE)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto label_client_msg__process_typing_indicator_request_end;
    }

    // a channel indicator may only be sent into the channel the sender actually stands in, so nobody
    // can announce itself into a room it is not in
    if (is_direct_message == FALSE && g_clients_array[sender_client_id].channel_id != receiver_id)
    {
        goto label_client_msg__process_typing_indicator_request_end;
    }

    // a client that may not send a direct message to somebody may not announce typing to them either
    if (is_direct_message == TRUE && (g_server_settings.is_private_messaging_allowed == FALSE || permissions__contact_refusal(&g_clients_array[sender_client_id], &g_clients_array[receiver_id], PERMISSION_SEND_DIRECT_MESSAGES) != NULL_POINTER))
    {
        goto label_client_msg__process_typing_indicator_request_end;
    }

    server_msg__chat__send_typing_indicator(&g_clients_array[sender_client_id], json_receiver_type->valuestring, receiver_id);

label_client_msg__process_typing_indicator_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks the saved recipient identity and encrypted offline message.
 * @param json_root Parsed request.
 */
static boole _client_msg_chat_internal__is_offline_chat_message_valid(cJSON *json_root)
{
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_recipient_identity_hash = NULL_POINTER;
    cJSON *json_value = NULL_POINTER;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return FALSE;
    }

    json_recipient_identity_hash = cJSON_GetObjectItemCaseSensitive(json_message_object, "recipient_identity_hash");
    if (cJSON_IsString(json_recipient_identity_hash) == FALSE || json_recipient_identity_hash->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_offline_chat_message_valid recipient_identity_hash \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_recipient_identity_hash->valuestring) == 0 || clib__utf8_string_length(json_recipient_identity_hash->valuestring) >= IDENTITY_HASH_MAX_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_offline_chat_message_valid recipient_identity_hash length \n");
        return FALSE;
    }

    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    if (cJSON_IsString(json_value) == FALSE || json_value->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_offline_chat_message_valid value \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_value->valuestring) == 0 || clib__utf8_string_length(json_value->valuestring) >= MAX_OFFLINE_MESSAGE_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_offline_chat_message_valid value length \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Stores an encrypted message when the sender and saved recipient have the required
 *        permissions.
 * @param json_root Request containing the recipient identity and encrypted message.
 * @param sender_client_id Client sending the message.
 */
void client_msg__chat__process_offline_chat_message(cJSON *json_root, uint64 sender_client_id)
{
    client_t *sender = NULL_POINTER;
    cJSON *message = NULL_POINTER;
    const char *recipient = NULL_POINTER;
    char sender_hash[IDENTITY_HASH_MAX_LENGTH] = { 0 };
    char connected_hash[IDENTITY_HASH_MAX_LENGTH] = { 0 };
    uint64 i = 0;

    if (_client_msg_chat_internal__is_offline_chat_message_valid(json_root) == FALSE)
    {
        return;
    }
    message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    recipient = cJSON_GetObjectItemCaseSensitive(message, "recipient_identity_hash")->valuestring;
    clib__read_lock(&g_clients_global_rwlock_guard);
    if (base__is_current_client_request(sender_client_id) == FALSE)
    {
        goto label_offline_message_end;
    }
    sender = &g_clients_array[sender_client_id];
    if (g_server_settings.is_identity_storage_enabled == FALSE ||
        g_server_settings.is_offline_messaging_allowed == FALSE ||
        permissions__is_allowed(sender, PERMISSION_SEND_OFFLINE_MESSAGES) == FALSE ||
        permissions__is_identity_allowed(recipient, PERMISSION_RECEIVE_OFFLINE_MESSAGES) == FALSE)
    {
        server_msg__administration__send_access_denied_to_single_client(sender, NULL_POINTER);
        goto label_offline_message_end;
    }
    for (i = 0; i < (uint64)g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE || g_clients_array[i].public_key[0] == 0)
        {
            continue;
        }
        util__hash_password_to_base64(g_clients_array[i].public_key, connected_hash, sizeof(connected_hash));
        if (clib__is_string_equal(connected_hash, recipient) == TRUE)
        {
            goto label_offline_message_end;
        }
    }
    util__hash_password_to_base64(sender->public_key, sender_hash, sizeof(sender_hash));
    identity_store__queue_offline_message((char *)recipient, sender_hash, sender->username, cJSON_GetObjectItemCaseSensitive(message, "value")->valuestring);

label_offline_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates a call-idle-client message: the target client id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_chat_internal__is_call_idle_client_message_valid(cJSON *json_root)
{
    uint64 validated_id = 0;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_client_id = NULL_POINTER;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (g_server_settings.max_client_count == 0 || util__json_read_uint64(json_client_id, g_server_settings.max_client_count - 1, &validated_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_call_idle_client_message_valid cJSON_IsNumber(json_client_id)");
        return FALSE;
    }

    if (json_client_id->valueint < 0 || json_client_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "json_client_id is invalid");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief processes a request to call an idle client (notify them so they can come back)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_call_idle_client_message(cJSON *json_root, uint64 sender_client_id)
{
    boole status0 = FALSE;
    client_t *caller = NULL_POINTER;
    client_t *callee = NULL_POINTER;
    cJSON *json_message_object = NULL_POINTER;
    cJSON *json_callee_client_id = NULL_POINTER;

    if (_client_msg_chat_internal__is_call_idle_client_message_valid(json_root) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_call_idle_client_message is not valid");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_callee_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__write_lock(&g_clients_global_rwlock_guard);

    // check if the client that sent the message is valid: connected and existing.
    // this was checked before but not within write lock like here
    caller = &g_clients_array[sender_client_id];

    status0 = (base__is_current_client_request(sender_client_id) == FALSE) || (caller->is_authenticated == FALSE || caller->is_existing == FALSE);
    if (status0 == TRUE)
    {
        goto label_client_msg__process_call_idle_client_message_end;
    }

    if (g_server_settings.is_idle_mode_allowed == FALSE || permissions__is_allowed(caller, PERMISSION_CALL_IDLE_CLIENTS) == FALSE)
    {
        server_msg__administration__send_permission_denied_to_single_client(caller, g_server_settings.is_idle_mode_allowed == TRUE ? permissions__name(PERMISSION_CALL_IDLE_CLIENTS) : NULL_POINTER);
        goto label_client_msg__process_call_idle_client_message_end;
    }
    callee = &g_clients_array[json_callee_client_id->valueint];

    status0 = callee->is_authenticated == FALSE || callee->is_existing == FALSE || callee->is_idle == FALSE || callee->is_music_bot == TRUE;
    if (status0 == TRUE)
    {
        goto label_client_msg__process_call_idle_client_message_end;
    }

    server_msg__channel__send_call_event_to_idle_client(callee, caller);

label_client_msg__process_call_idle_client_message_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief Checks a URL preview request and asks the HTTP client to look up the page.
 * @param cJSON* json_root -> message containing the URL and request ID
 * @param uint64 client_id -> client requesting the preview
 * @param ws_cli_conn_t* socket -> connection that sent the request
 */
void client_msg__chat__process_resolve_url_request(cJSON *json_root, uint64 client_id, ws_cli_conn_t *socket)
{
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(message, "url");
    cJSON *request_id = cJSON_GetObjectItemCaseSensitive(message, "request_id");
    httpclient_url_t parsed;

    if (!cJSON_IsString(url) ||
        !cJSON_IsString(request_id) ||
        clib__strlen(url->valuestring) > 2048 ||
        !request_id->valuestring[0] ||
        clib__strlen(request_id->valuestring) > 64 ||
        httpclient__parse_url(url->valuestring, &parsed) != HTTPCLIENT_OK)
    {
        return;
    }
    url_resolver__queue_lookup(url->valuestring, request_id->valuestring, client_id, socket);
}
