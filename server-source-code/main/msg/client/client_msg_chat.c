// This file checks incoming chat messages, edits, typing notifications, pokes and URL preview requests.
#include "client_msg.h"
#include <string.h>
#include "../../core/identity_store.h"
#include "../server/server_msg.h"
#include "../../httpclient/url_resolver.h"
#include "../../httpclient/httpclient.h"
#include "../../base.h"
#include "../../util.h"
#include "../../clib/clib_string.h"
#include "../../clib/clib_memory.h"
#include "../../../third-party/rxi-log/log.h"
#include "../../../third-party/zhicheng/base64.h"

// declarations
static boole _client_msg_chat_internal__is_json_chat_message_format_valid(cJSON* json_root, uint64 client_id);
static void _client_msg_chat_internal__process_chat_message_action(cJSON* json_root, uint64 sender_client_id, char* outbound_action_type, boole is_edit);
static boole _client_msg_chat_internal__is_json_poke_client_request_format_valid(cJSON* json_root, uint64 client_id);
static boole _client_msg_chat_internal__is_offline_chat_message_valid(cJSON* json_root);
static boole _client_msg_chat_internal__is_call_idle_client_message_valid(cJSON* json_root);

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
static boole _client_msg_chat_internal__is_json_chat_message_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_chat_message_value = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;
    cJSON* json_message_object = 0;

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
    if (clib__utf8_string_length_check_max_length(json_chat_message_value->valuestring, (int)((g_server_settings.websocket_message_max_length * 3) / 4 - 2048)) == -1)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " chat message value too large \n");
        return FALSE;
    }

    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
    if (cJSON_IsNumber(json_receiver_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_receiver_id) \n");
        return FALSE;
    }

    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");
    if (cJSON_IsNumber(json_local_message_id) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client : ", client_id, " cJSON_IsNumber(json_local_message_id) \n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief This function processes direct chat message
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_direct_chat_message(cJSON* json_root, uint64 sender_client_id)
{
    boole is_message_valid = FALSE;
    boole is_receiver_existing = FALSE;
    boole is_receiver_idle = FALSE;
    boole is_receiver_music_bot = FALSE;
    cJSON* json_receiver_id = 0;
    cJSON* json_local_message_id = 0;
    cJSON* json_chat_message_value = 0;
    cJSON* json_message_object = 0;

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

    if (json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__chat__process_direct_chat_message client : ", sender_client_id, " json_receiver_id->valueint < 0 || json_receiver_id->valueint >= g_server_settings.max_client_count \n");
        return;
    }

    // private messaging can be switched off server-wide, making this a channels-only server.
    // music bots are addressed the same way, so they keep working regardless
    if (g_server_settings.is_private_messaging_allowed == FALSE && g_clients_array[json_receiver_id->valueint].is_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__chat__process_direct_chat_message client : ", sender_client_id, " private messages are disabled on this server \n");
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // if sender has idle mode active, stop it
    is_receiver_existing = g_clients_array[json_receiver_id->valueint].is_authenticated;
    is_receiver_idle = g_clients_array[json_receiver_id->valueint].is_idle;
    is_receiver_music_bot = g_clients_array[json_receiver_id->valueint].is_music_bot;

    // an idle client keeps its websocket open, so text can still reach it; the admin may allow that by config
    if (is_receiver_existing == TRUE && (is_receiver_idle == FALSE || g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE) && is_receiver_music_bot == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__chat__process_direct_chat_message receiver is_existing: ", json_receiver_id->valueint, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(&g_clients_array[sender_client_id], server_chat_message_id, json_local_message_id->valueint);
        server_msg__chat__send_message_to_single_client(&g_clients_array[json_receiver_id->valueint], &g_clients_array[sender_client_id], server_chat_message_id, json_chat_message_value->valuestring);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %d %s", "client_msg__chat__process_direct_chat_message receiver is_existing not is_existing or is in idle mode", json_receiver_id->valueint, "\n");
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief This function processes a channel chat message, broadcasting it to clients in the sender's channel
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_channel_chat_message(cJSON* json_root, uint64 sender_client_id)
{
    boole is_channel_existing = FALSE;
    // cJSON* json_receiver_id = 0; json contains this ID, but it is not needed. Id of channel is taken from sender's client struct
    cJSON* json_local_message_id = 0;
    cJSON* json_chat_message_value = 0;
    cJSON* json_message_object = 0;
    uint64 channel_id = 0;
    uint64 server_chat_message_id = 0;

    boole status = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_channel_chat_message base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_chat_internal__is_json_chat_message_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_channel_chat_message client_msg__is_json_chat_message_format_valid == FALSE \n");
        return;
    }

    DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_channel_chat_message got here \n");
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_chat_message_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");
    json_local_message_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "local_message_id");

    clib__read_lock(&g_clients_global_rwlock_guard);
    clib__read_lock(&g_channels_global_rwlock_guard);

    channel_id = g_clients_array[sender_client_id].channel_id;
    is_channel_existing = g_channel_array[channel_id].is_existing;

    if (is_channel_existing == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiving channel exists: ", channel_id, "\n");

        server_chat_message_id = base__get_chat_message_id();
        base__increment_chat_message_id();
        server_msg__chat__send_server_chat_message_id_for_local_chat_message_id_to_single_client(&g_clients_array[sender_client_id], server_chat_message_id, json_local_message_id->valueint);
        server_msg__chat__send_message_to_clients_in_same_channel(&g_clients_array[sender_client_id], channel_id, server_chat_message_id, json_chat_message_value->valuestring);
    }
    else
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "receiving channel does not exist: ", channel_id, "\n");
    }

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
static void _client_msg_chat_internal__process_chat_message_action(cJSON* json_root, uint64 sender_client_id, char* outbound_action_type, boole is_edit)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_message_id = 0;
    cJSON* json_receiver_type = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_new_message_value = 0;
    char* receiver_type = 0;
    char* new_message_value = NULL_POINTER;
    char* requester_public_key = 0;
    uint64 target_chat_message_id = 0;
    uint64 channel_id = 0;
    int64 receiver_id = 0;
    boole requester_is_admin = FALSE;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
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

    if (cJSON_IsNumber(json_message_id) == FALSE || cJSON_IsString(json_receiver_type) == FALSE)
    {
        return;
    }

    target_chat_message_id = (uint64)json_message_id->valuedouble;
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
        if (clib__utf8_string_length_check_max_length(json_new_message_value->valuestring, (int)((g_server_settings.websocket_message_max_length * 3) / 4 - 2048)) == -1)
        {
            return;
        }

        new_message_value = json_new_message_value->valuestring;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    requester_public_key = g_clients_array[sender_client_id].public_key;
    requester_is_admin = g_clients_array[sender_client_id].is_admin;

    if (clib__is_string_equal(receiver_type, "user") == TRUE)
    {
        json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "receiver_id");
        if (cJSON_IsNumber(json_receiver_id) == TRUE)
        {
            receiver_id = json_receiver_id->valueint;
            if (receiver_id >= 0 && receiver_id < g_server_settings.max_client_count && g_clients_array[receiver_id].is_authenticated == TRUE)
            {
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
        server_msg__chat__send_message_action_to_clients_in_same_channel(channel_id, outbound_action_type, target_chat_message_id, requester_public_key, requester_is_admin, new_message_value);
        clib__unlock(&g_channels_global_rwlock_guard);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief processes a request to delete a chat message (rebroadcasts a delete action to the right audience)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_delete_chat_message_request(cJSON* json_root, uint64 sender_client_id)
{
    _client_msg_chat_internal__process_chat_message_action(json_root, sender_client_id, "chat_message_delete", FALSE);
}

/**
 * @brief processes a request to edit a chat message (rebroadcasts an edit action to the right audience)
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_edit_chat_message_request(cJSON* json_root, uint64 sender_client_id)
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
static boole _client_msg_chat_internal__is_json_poke_client_request_format_valid(cJSON* json_root, uint64 client_id)
{
    cJSON* json_poke_message = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_message_object = 0;

    // existence of "message" has already been checked at this point
    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    // json_message_object exists, continue validating received json
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_receiver_id) == FALSE)
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
 * @brief This function processes a poke-client request, forwarding the poke message to the target client
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_poke_client_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_receiver_id = 0;
    cJSON* json_poke_message = 0;

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_poke_client_request base__is_request_allowed_based_on_spam_protection == FALSE \n");
        return;
    }

    status = _client_msg_chat_internal__is_json_poke_client_request_format_valid(json_root, sender_client_id);
    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_poke_client_request client_msg__is_json_poke_client_request_format_valid == FALSE \n");
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_poke_message = cJSON_GetObjectItemCaseSensitive(json_message_object, "poke_message");
    json_receiver_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");

    clib__read_lock(&g_clients_global_rwlock_guard);

    if (g_clients_array[json_receiver_id->valueint].is_authenticated == TRUE)
    {
        server_msg__chat__send_poke_to_single_client(&g_clients_array[json_receiver_id->valueint], &g_clients_array[sender_client_id], json_poke_message->valuestring);
    }

    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief passes on "this client is typing" to the conversation the client is writing to. the request carries
 *        no message content and nothing is stored - it is forwarded to whoever is on the other side
 *        and forgotten. the whole feature is off unless the server allows it.
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that is typing
 *
 * @attention this function uses read lock on clients_global_rwlock_guard
 *
 * @return void
 */
void client_msg__chat__process_typing_indicator_request(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole is_direct_message = FALSE;
    cJSON* json_message_object = 0;
    cJSON* json_receiver_type = 0;
    cJSON* json_receiver_id = 0;

    if (g_server_settings.is_typing_indicator_allowed == FALSE)
    {
        return;
    }

    status = base__is_request_allowed_based_on_spam_protection(sender_client_id);
    if (status == FALSE)
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

    if (cJSON_IsNumber(json_receiver_id) == FALSE || json_receiver_id->valueint < 0)
    {
        return;
    }

    is_direct_message = clib__is_string_equal(json_receiver_type->valuestring, "user");

    if (is_direct_message == FALSE && clib__is_string_equal(json_receiver_type->valuestring, "channel") == FALSE)
    {
        return; // only those two kinds of conversation exist
    }

    if (is_direct_message == TRUE && (uint64)json_receiver_id->valueint >= g_server_settings.max_client_count)
    {
        return;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    // a channel indicator may only be sent into the channel the sender actually stands in, so nobody
    // can announce itself into a room it is not in
    if (is_direct_message == FALSE && g_clients_array[sender_client_id].channel_id != (uint64)json_receiver_id->valueint)
    {
        goto label_client_msg__process_typing_indicator_request_end;
    }

    server_msg__chat__send_typing_indicator(&g_clients_array[sender_client_id], json_receiver_type->valuestring, (uint64)json_receiver_id->valueint);

label_client_msg__process_typing_indicator_request_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief validates an offline chat message request: a registered recipient alias and a non-empty
 *        ciphertext within the queue's per-message cap.
 *
 * @param cJSON* json_root -> the parsed client request
 *
 * @return boole
 */
static boole _client_msg_chat_internal__is_offline_chat_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_recipient_alias = 0;
    cJSON* json_value = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    if (json_message_object == NULL_POINTER)
    {
        return FALSE;
    }

    json_recipient_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "recipient_alias");
    if (cJSON_IsString(json_recipient_alias) == FALSE || json_recipient_alias->valuestring == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_offline_chat_message_valid recipient_alias \n");
        return FALSE;
    }

    if (clib__utf8_string_length(json_recipient_alias->valuestring) == 0 || clib__utf8_string_length(json_recipient_alias->valuestring) >= USERNAME_MAX_LENGTH)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "_client_msg_chat_internal__is_offline_chat_message_valid recipient_alias length \n");
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
 * @brief stores a text message addressed to a registered identity that is not connected, and delivers
 *        it when that identity connects again
 *
 *        the payload arrives already encrypted, so the server never sees plaintext: the sender
 *        encrypted it with the recipient's public key, which it took from the stored-clients list.
 *        the message is stored in ram, keyed by identity hash rather than by alias.
 *
 *        the request is refused unless the feature is on, identities and the stored-clients list
 *        are on, the SENDER is registered, the recipient alias belongs to a registered identity,
 *        and that identity is not connected right now (if it is, the client should send a normal
 *        direct message instead).
 *
 * @param cJSON* json_root -> the parsed client request
 * @param uint64 sender_client_id -> id of the client that sent the request
 *
 * @return void
 */
void client_msg__chat__process_offline_chat_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    boole queued = FALSE;
    boole is_recipient_connected = FALSE;
    client_t* sender = 0;
    cJSON* json_message_object = 0;
    cJSON* json_recipient_alias = 0;
    cJSON* json_value = 0;
    char sender_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    char recipient_identity_hash[IDENTITY_HASH_MAX_LENGTH];
    char sender_alias[USERNAME_MAX_LENGTH];
    char* message_value = 0;
    uint64 i = 0;

    if (g_server_settings.is_identity_storage_enabled == FALSE || g_server_settings.is_stored_client_list_allowed == FALSE || g_server_settings.is_offline_messaging_allowed == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_offline_chat_message offline messages are not enabled \n");
        return;
    }

    status = _client_msg_chat_internal__is_offline_chat_message_valid(json_root);
    if (status == FALSE)
    {
        return;
    }

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    json_recipient_alias = cJSON_GetObjectItemCaseSensitive(json_message_object, "recipient_alias");
    json_value = cJSON_GetObjectItemCaseSensitive(json_message_object, "value");

    clib__null_memory(&sender_identity_hash[0], sizeof(sender_identity_hash));
    clib__null_memory(&recipient_identity_hash[0], sizeof(recipient_identity_hash));
    clib__null_memory(&sender_alias[0], sizeof(sender_alias));

    // read the sender out of the clients array, then release: queueing takes its own leaf lock
    clib__read_lock(&g_clients_global_rwlock_guard);

    sender = &g_clients_array[sender_client_id];

    if (sender->is_existing == FALSE || sender->is_authenticated == FALSE || sender->is_music_bot == TRUE || sender->public_key[0] == 0)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // only a registered user may leave messages, so an anonymous guest cannot spam the queue
    if (sender->is_registered == FALSE || sender->alias[0] == 0)
    {
        DBG_CLIENT_MESSAGE log_info("%s %llu %s", "client_msg__chat__process_offline_chat_message sender", sender_client_id, "is not registered \n");
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    util__hash_password_to_base64(sender->public_key, sender_identity_hash, sizeof(sender_identity_hash));
    clib__copy_memory(&sender->alias[0], &sender_alias[0], clib__utf8_string_length(&sender->alias[0]), USERNAME_MAX_LENGTH - 1);

    // if the recipient is connected right now there is nothing to queue - the sender should have
    // used a normal direct message. checked while the clients lock is held
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (g_clients_array[i].alias[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(&g_clients_array[i].alias[0], json_recipient_alias->valuestring) == TRUE)
        {
            is_recipient_connected = TRUE;
            break;
        }
    }

    clib__unlock(&g_clients_global_rwlock_guard);

    if (is_recipient_connected == TRUE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_offline_chat_message recipient is online, not queueing \n");
        return;
    }

    // resolve the alias to the identity that owns it: the queue is keyed by identity, so moving an
    // alias to somebody else later cannot hand them messages meant for the original owner
    if (identity_store__get_identity_hash_by_alias(json_recipient_alias->valuestring, &recipient_identity_hash[0], sizeof(recipient_identity_hash)) == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_offline_chat_message no identity carries that alias \n");
        return;
    }

    message_value = json_value->valuestring;

    queued = identity_store__queue_offline_message(&recipient_identity_hash[0], &sender_identity_hash[0], &sender_alias[0], message_value);

    if (queued == FALSE)
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__chat__process_offline_chat_message could not queue the message \n");
    }
}

/**
 * @brief validates a call-idle-client message: the target client id is present and within range
 *
 * @param cJSON* json_root -> the parsed client request
 * @note this is just first glance check
 *
 * @return boole
 */
static boole _client_msg_chat_internal__is_call_idle_client_message_valid(cJSON* json_root)
{
    cJSON* json_message_object = 0;
    cJSON* json_client_id = 0;

    json_message_object = cJSON_GetObjectItemCaseSensitive(json_root, "message");

    json_client_id = cJSON_GetObjectItemCaseSensitive(json_message_object, "client_id");
    if (cJSON_IsNumber(json_client_id) == FALSE)
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
void client_msg__chat__process_call_idle_client_message(cJSON* json_root, uint64 sender_client_id)
{
    boole status = FALSE;
    client_t* caller = 0;
    client_t* callee = 0;

    cJSON* json_message_object = 0;
    cJSON* json_callee_client_id = 0;

    status = _client_msg_chat_internal__is_call_idle_client_message_valid(json_root);
    if (status == FALSE)
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

    if (caller->is_authenticated == FALSE || caller->is_existing == FALSE)
    {
        goto label_client_msg__process_call_idle_client_message_end;
    }

    // check if the client is idle too later
    callee = &g_clients_array[json_callee_client_id->valueint];

    if (callee->is_authenticated == FALSE || callee->is_existing == FALSE)
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
void client_msg__chat__process_resolve_url_request(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket)
{
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* url = cJSON_GetObjectItemCaseSensitive(message, "url");
    cJSON* request_id = cJSON_GetObjectItemCaseSensitive(message, "request_id");
    httpclient_url_t parsed;
    if (!cJSON_IsString(url) || !cJSON_IsString(request_id)
        || strlen(url->valuestring) > 2048 || !request_id->valuestring[0] || strlen(request_id->valuestring) > 64
        || httpclient__parse_url(url->valuestring, &parsed) != HTTPCLIENT_OK)
    {
        return;
    }
    url_resolver__queue_lookup(url->valuestring, request_id->valuestring, client_id, socket);
}
