
#include "definitions.h"

#ifdef WIN32
#include <Windows.h>
#endif

#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "server_message.h"
#include "server_logs.h"
#include "memory_manager.h"
#include "../third-party/eteran-cvector/cvector.h"
#include "../third-party/rxi-log/log.h"
#include "clib/clib_string.h"  // clib__is_string_equal (identity online check)
#include "clib/clib_memory.h"  // clib__null_memory (clearing a delivered offline message slot)
#include "../third-party/zhicheng/base64.h"  // BASE64_ENCODE_OUT_SIZE (identity hash buffer)

#include "util.h"

/**
 * @brief gets called by invididuals websocket thread
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of client to send this message to
 * @param char* random_value_challenge_string -> randomly generated string to be sent as challenge
 * @param char* dh_public_mix_for_client -> dh public mix that client needs to set on his side, result of diffie hellman key exchange
 *
 * @return void
 */
/**
 * @brief tells a client its rsa key is too weak, naming the required size, so it can offer to
 *        regenerate. only sent when the admin enabled announce_minimum_rsa_key_bits; otherwise
 *        the client is dropped without a word and never learns the requirement.
 *
 * @attention sent in PLAINTEXT on purpose: the key is rejected before the diffie-hellman
 *            exchange completes, so there is no shared secret to encrypt with yet. the message
 *            carries nothing secret - only the already-public policy the admin chose to publish
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of the rejected client
 *
 * @return void
 */
void server_msg__send_rsa_key_too_weak_to_single_client(ws_cli_conn_t* websocket)
{
    char* json_root_object1_string = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "rsa_key_too_weak");
    cJSON_AddNumberToObject(json_message_object1, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    if (json_root_object1_string != NULL_POINTER)
    {
        ws_sendframe_txt(websocket, json_root_object1_string);
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

void server_msg__send_public_key_challenge_to_single_client(ws_cli_conn_t* websocket, char* random_value_challenge_string, char* dh_public_mix_for_client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_AUTHENTICATION log_info("%s ", "server_msg__send_public_key_challenge_to_single_client \n");

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_public_key_challenge_to_single_client");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "public_key_challenge");
    cJSON_AddStringToObject(json_message_object1, "value", random_value_challenge_string);
    cJSON_AddStringToObject(json_message_object1, "dh_public_mix", dh_public_mix_for_client);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, NULL_POINTER);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(websocket, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief part of authentication process. Sends authentication status to client along with information whether server is using voice chan and stun port , for UDP voice chat to set on clients side
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of client to send this message to
 * @param char* ws_connection_dh_shared_secret -> self exlanatory
 *
 * @return void
 *
 */
void server_msg__send_authentication_status_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_authentication_status_to_single_client");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "authentication_status");
    cJSON_AddStringToObject(json_message_object1, "value", "success");
    cJSON_AddBoolToObject(json_message_object1, "is_voice_chat_active", g_server_settings.is_voice_chat_active);
    cJSON_AddBoolToObject(json_message_object1, "is_music_bot_audio_active", g_server_settings.is_music_bot_audio_active);
    cJSON_AddBoolToObject(json_message_object1, "is_idle_mode_allowed", g_server_settings.is_idle_mode_allowed);
    // the client attempts a fast reconnect only on a server that allows it
    cJSON_AddBoolToObject(json_message_object1, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
    // a row rendered for an admin skips the flag, also when he becomes admin after joining
    cJSON_AddBoolToObject(json_message_object1, "hide_admin_country_flag", g_server_settings.hide_admin_country_flag);
    cJSON_AddBoolToObject(json_message_object1, "is_alias_registration_allowed", g_server_settings.allow_alias_registrations);
    // avatars policy travels in-protocol so clients that were NOT served by this server's http
    // server (the android app loads the page from its assets) still learn it - otherwise they
    // fall back to "avatars off" and hide the set/delete avatar actions
    cJSON_AddBoolToObject(json_message_object1, "allow_avatars", g_server_settings.allow_avatars);
    cJSON_AddBoolToObject(json_message_object1, "allow_typing_indicator", g_server_settings.allow_typing_indicator);
    // rename policy travels in-protocol so the client can grey its rename input out when users may not rename
    cJSON_AddBoolToObject(json_message_object1, "allow_client_renames", g_server_settings.allow_client_renames);
    cJSON_AddNumberToObject(json_message_object1, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
    // chat file policy: the client refuses oversize/forbidden files itself, with a reason, before uploading
    cJSON_AddBoolToObject(json_message_object1, "allow_file_uploads", g_server_settings.allow_file_uploads);
    cJSON_AddNumberToObject(json_message_object1, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(json_message_object1, "chat_picture_max_size", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_AddBoolToObject(json_message_object1, "allow_chat_pictures", g_server_settings.allow_chat_pictures);
    cJSON_AddNumberToObject(json_message_object1, "stun_port", 3478);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    // DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(websocket, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends one client's info (connected duration, ip, country, identity, last action) to a single
 *        admin client, in reply to a get_client_info request
 *
 * @param client_t* receiving_client -> the admin the reply is sent to (provides the websocket + shared secret)
 * @param client_t* target_client -> the client the info is about
 *
 * @attention caller must hold at least the clients read lock
 *
 * @return void
 */
void server_msg__send_client_info_to_single_client(client_t* receiving_client, client_t* target_client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 now_ms = 0;
    uint64 connected_seconds = 0;
    uint64 last_action_seconds_ago = 0;

    now_ms = base__get_timestamp_ms();
    if (now_ms >= target_client->timestamp_connected)
    {
        connected_seconds = (now_ms - target_client->timestamp_connected) / 1000;
    }
    if (now_ms >= target_client->timestamp_last_action)
    {
        last_action_seconds_ago = (now_ms - target_client->timestamp_last_action) / 1000;
    }

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_info");
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)target_client->client_id);
    cJSON_AddNumberToObject(json_message_object1, "connected_seconds", (double)connected_seconds);
    cJSON_AddNumberToObject(json_message_object1, "last_action_seconds_ago", (double)last_action_seconds_ago);
    cJSON_AddStringToObject(json_message_object1, "ip_address", &target_client->ip_address[0]);
    cJSON_AddStringToObject(json_message_object1, "country_iso_code", &target_client->country_iso_code[0]);
    cJSON_AddStringToObject(json_message_object1, "identity", &target_client->public_key[0]);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, receiving_client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(receiving_client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief tells a single client that the channel they tried to join is full (used by the capacity gate)
 *
 * @param client_t* client -> the client to notify
 * @param uint64 channel_id -> the full channel
 *
 * @return void
 */
void server_msg__send_channel_full_to_single_client(client_t* client, uint64 channel_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_full");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)channel_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief This function sends channel list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection
 * @param char* ws_connection_dh_shared_secret -> DH key exchange generated shared secret, that must be used later within this function.
 *
 * @return void
 */
void server_msg__send_channel_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_channel_array = 0;
    char* json_root_object1_string = 0;
    char* msg_text = 0;
    int64 size_of_allocated_message_buffer = 0;
    uint64 i = 0;
    cJSON* single_channel = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_list_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_channel_array = cJSON_CreateArray();
    json_root_object1_string = 0;
    msg_text = 0;
    size_of_allocated_message_buffer = 0;

    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        if (g_channel_array[i].is_existing == FALSE)
        {
            continue;
        }

        single_channel = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_channel, "channel_id", g_channel_array[i].channel_id);
        cJSON_AddNumberToObject(single_channel, "parent_channel_id", g_channel_array[i].parent_channel_id);
        cJSON_AddStringToObject(single_channel, "name", g_channel_array[i].name);
        cJSON_AddStringToObject(single_channel, "description", g_channel_array[i].description);
        cJSON_AddBoolToObject(single_channel, "is_using_password", g_channel_array[i].is_using_password);
        cJSON_AddBoolToObject(single_channel, "is_audio_enabled", g_channel_array[i].is_audio_enabled);
        cJSON_AddBoolToObject(single_channel, "is_root_channel", g_channel_array[i].is_root_channel);
        cJSON_AddBoolToObject(single_channel, "has_maintainer", g_channel_array[i].is_channel_maintainer_present);
        cJSON_AddBoolToObject(single_channel, "is_temp_channel", g_channel_array[i].is_temp_channel);
        cJSON_AddBoolToObject(single_channel, "is_client_limit_active", g_channel_array[i].is_client_limit_active);
        cJSON_AddNumberToObject(single_channel, "max_client_count", (double)g_channel_array[i].max_client_count);
        cJSON_AddBoolToObject(single_channel, "has_channel_icon", g_channel_array[i].has_channel_icon);
        cJSON_AddNumberToObject(single_channel, "channel_icon_id", (double)g_channel_array[i].icon_id);
        cJSON_AddItemToArray(json_channel_array, single_channel);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "channel_list");
    cJSON_AddItemToObject(json_message_object1, "channels", json_channel_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_list_to_single_client: json_root_object1_string ", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(websocket, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief This function sends client list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection
 * @param char* ws_connection_dh_shared_secret -> DH key exchange generated shared secret, that must be used later within this function.
 * @param char* local_clients_username -> username of local client
 * @param uint64 client_receiver_id -> id of the client that receives the list, used to compare his channel
 *                                     against every listed client when hiding clients that sit in
 *                                     password protected channels
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_client_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret, char* local_clients_username, uint64 client_receiver_id)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_client_array = 0;
    cJSON* json_tag_ids_array = 0;
    client_t* client_in_loop = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 tag_id_index = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_list_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_client_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        cJSON* single_client = NULL_POINTER;
        boole is_hide_client_active = FALSE;
        int64 audio_state_to_send = 0;

        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        single_client = cJSON_CreateObject();
        cJSON_AddStringToObject(single_client, "username", client_in_loop->username);
        cJSON_AddStringToObject(single_client, "public_key", client_in_loop->public_key);

        // check if client in loop is in different channel that receiving client
        // check if client is in password protected channel
        is_hide_client_active = FALSE;

        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)
        {
            if (g_clients_array[client_receiver_id].channel_id != client_in_loop->channel_id)
            {
                // bounds check, channel_id is not validated anywhere
                if (client_in_loop->channel_id < g_server_settings.max_channel_count
                    && g_channel_array[client_in_loop->channel_id].is_existing == TRUE
                    && g_channel_array[client_in_loop->channel_id].is_using_password == TRUE)
                {
                    is_hide_client_active = TRUE;
                }
            }
        }
        if (is_hide_client_active == TRUE)
        {
            cJSON_AddNumberToObject(single_client, "channel_id", (double)-1);
        }
        else
        {
            cJSON_AddNumberToObject(single_client, "channel_id", (double)client_in_loop->channel_id);
        }

        cJSON_AddNumberToObject(single_client, "client_id", (double)client_in_loop->client_id);

        cJSON_AddBoolToObject(single_client, "is_music_bot", client_in_loop->is_music_bot);

        if (client_in_loop->is_music_bot == TRUE)
        {
            DBG_SERVER_MESSAGE log_info("%s %lld %s", "server_msg client id is -> ", client_in_loop->client_id, "\n");
        }

        audio_state_to_send = client_in_loop->audio_state;

        // only send "PUSH_TO_TALK_ACTIVE" state to connected client for other clients in same channel as he is (root channel in this case)
        // privacy reasons
        if (client_in_loop->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE && client_in_loop->channel_id != ROOT_CHANNEL_ID)
        {
            audio_state_to_send = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }

        cJSON_AddNumberToObject(single_client, "audio_state", (double)audio_state_to_send);

        if (client_in_loop->tag_ids != NULL_POINTER)
        {
            json_tag_ids_array = cJSON_CreateIntArray(client_in_loop->tag_ids, cvector_size(client_in_loop->tag_ids));
            cJSON_AddItemToObject(single_client, "tag_ids", json_tag_ids_array);
        }
        else
        {
            json_tag_ids_array = cJSON_CreateArray();
            cJSON_AddItemToObject(single_client, "tag_ids", json_tag_ids_array);
        }

        cJSON_AddBoolToObject(single_client, "is_idle", client_in_loop->is_idle);

        // admin-registered alias (display name); empty string when none
        cJSON_AddStringToObject(single_client, "alias", &client_in_loop->alias[0]);

        // property country_iso_code will always be part of response
        // the value will be empty if ip address is from unknown country or if server doesn't display flags,
        // and for an admin when the server keeps admin flags private
        cJSON_AddStringToObject(single_client, "country_iso_code",
            (g_server_settings.hide_admin_country_flag == TRUE && client_in_loop->is_admin == TRUE) ? "" : client_in_loop->country_iso_code);

        cJSON_AddItemToArray(json_client_array, single_client);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "client_list");
    cJSON_AddItemToObject(json_message_object1, "clients", json_client_array);
    cJSON_AddStringToObject(json_message_object1, "local_username", local_clients_username);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: json_root_object1_string ", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");
        ws_sendframe_txt(websocket, msg_text);
        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief This function sends icon list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection
 * @param char* ws_connection_dh_shared_secret -> DH key exchange generated shared secret, that must be used later within this function.
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_icon_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_icons_array = 0;
    client_t* client_in_loop = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_icon_list_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_icons_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_ICONS; x++)
    {
        cJSON* single_client = NULL_POINTER;

        if (g_icons_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_client = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_client, "icon_id", (double)g_icons_array[x].id);
        cJSON_AddStringToObject(single_client, "base64_icon", g_icons_array[x].base64);
        cJSON_AddItemToArray(json_icons_array, single_client);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "icon_list");
    cJSON_AddItemToObject(json_message_object1, "icons", json_icons_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_icon_list_to_single_client: json_root_object1_string ", json_root_object1_string, "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_icon_list_to_single_client 1 \n");

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_icon_list_to_single_client 2 \n");

    ws_sendframe_txt(websocket, msg_text);

    DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_icon_list_to_single_client 3 \n");

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief This function sends tag list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection
 * @param char* ws_connection_dh_shared_secret -> DH key exchange generated shared secret, that must be used later within this function.
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_tag_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_tags_array = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_tag_list_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_tags_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < MAX_TAGS; x++)
    {
        cJSON* single_tag_id_object = NULL_POINTER;

        if (g_tags_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_tag_id_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_tag_id_object, "tag_id", (double)g_tags_array[x].id);
        cJSON_AddStringToObject(single_tag_id_object, "tag_name", g_tags_array[x].name);
        cJSON_AddNumberToObject(single_tag_id_object, "tag_linked_icon_id", (double)g_tags_array[x].icon_id);
        cJSON_AddItemToObject(single_tag_id_object, "has_icon", cJSON_CreateBool(g_tags_array[x].has_icon == TRUE));
        cJSON_AddItemToArray(json_tags_array, single_tag_id_object);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "tag_list");
    cJSON_AddItemToObject(json_message_object1, "tags", json_tags_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_tag_list_to_single_client: json_root_object1_string ", json_root_object1_string, "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

    ws_sendframe_txt(websocket, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends the admin identity-management list: every stored identity as { public_key_hash,
 *        tag_ids[], is_online }. is_online is TRUE when a currently-connected authenticated client
 *        hashes to the same identity. admin-only; the caller checks that and holds the clients lock.
 *
 * @param ws_cli_conn_t* websocket -> the requesting admin's connection
 * @param char* ws_connection_dh_shared_secret -> that connection's DH secret for encryption
 *
 * @note the caller must hold the clients read lock (g_clients_array is read for online status). the
 *       identity store is guarded by g_client_stored_data_mutex, taken here.
 *
 * @return void
 */
void server_msg__send_identity_list_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_identities_array = 0;
    char client_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 c = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_identity_list_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_identities_array = cJSON_CreateArray();

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        cJSON* single_identity_object = NULL_POINTER;
        cJSON* identity_tag_ids_array = NULL_POINTER;
        boole is_online = FALSE;

        // an identity is worth listing when it carries ANYTHING: tags, an alias or an avatar. it used
        // to require tags, which hid every registered user from the admin - registration grants an
        // alias, not a tag, so the very people this list is for were the ones missing from it
        if (g_client_stored_data[i].public_key[0] == 0
            || (g_client_stored_data[i].tag_id_count == 0 && g_client_stored_data[i].alias[0] == 0 && g_client_stored_data[i].base64_avatar[0] == 0))
        {
            continue;
        }

        single_identity_object = cJSON_CreateObject();
        cJSON_AddStringToObject(single_identity_object, "public_key_hash", &g_client_stored_data[i].public_key[0]);
        cJSON_AddStringToObject(single_identity_object, "username", &g_client_stored_data[i].username[0]);
        // the admin-granted display name; empty means this identity is not registered
        cJSON_AddStringToObject(single_identity_object, "alias", &g_client_stored_data[i].alias[0]);

        identity_tag_ids_array = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(identity_tag_ids_array, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(single_identity_object, "tag_ids", identity_tag_ids_array);

        // mark online if any connected authenticated non-bot client hashes to this identity
        for (c = 0; c < g_server_settings.max_client_count; c++)
        {
            client_t* connected_client = &g_clients_array[c];

            if (connected_client->is_existing == FALSE || connected_client->is_authenticated == FALSE || connected_client->is_music_bot == TRUE)
            {
                continue;
            }
            if (connected_client->public_key[0] == 0)
            {
                continue;
            }

            base__hash_password_to_base64(connected_client->public_key, client_hash, sizeof(client_hash));
            if (clib__is_string_equal(client_hash, &g_client_stored_data[i].public_key[0]) == TRUE)
            {
                is_online = TRUE;
                break;
            }
        }
        cJSON_AddItemToObject(single_identity_object, "is_online", cJSON_CreateBool(is_online == TRUE));

        cJSON_AddItemToArray(json_identities_array, single_identity_object);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    cJSON_AddStringToObject(json_message_object1, "type", "identity_list");
    cJSON_AddItemToObject(json_message_object1, "identities", json_identities_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(websocket, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief This function microphone usage of all clients for current clients channel.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection
 * @param char* ws_connection_dh_shared_secret -> DH key exchange generated shared secret, that must be used later within this function.
 * @param uint64 current_channel_id -> current channel id
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_active_microphone_usage_for_current_channel_to_single_client(ws_cli_conn_t* websocket, char* ws_connection_dh_shared_secret, uint64 current_channel_id)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_clients_array = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_active_microphone_usage_for_current_channel_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_clients_array = cJSON_CreateArray();

    // create array of clients
    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        cJSON* single_object = NULL_POINTER;

        client_in_loop = &g_clients_array[x];
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->channel_id != current_channel_id)
        {
            continue;
        }

        // have to send even to ourselves
        // if client.client_id == current_client_id {
        // continue;
        // }

        // only active mics are relevant since this is "active microphone usage"
        // though
        if (client_in_loop->audio_state != AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            continue;
        }

        single_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_object, "client_id", client_in_loop->client_id);
        cJSON_AddNumberToObject(single_object, "audio_state", client_in_loop->audio_state);
        cJSON_AddBoolToObject(single_object, "is_streaming_song", client_in_loop->is_streaming_song);
        cJSON_AddStringToObject(single_object, "song_name", client_in_loop->song_name);

        cJSON_AddItemToArray(json_clients_array, single_object);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "current_channel_active_microphone_usage");
    cJSON_AddItemToObject(json_message_object1, "clients", json_clients_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_send_active_microphone_usage_for_current_channel_to_single_client: json_root_object1_string ", json_root_object1_string, "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, ws_connection_dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");
        ws_sendframe_txt(websocket, msg_text);
        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief This function sends tag list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param uint64 client_id_of_connected_client -> self explanatory
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @note, this function is somewhat expensive, the json is constructed in loop individually for each client...
 * same as send_client_list_to_single_client,
 *
 * @return void
 */
void server_msg__send_client_connect_message_to_all_clients(uint64 client_id_of_connected_client)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_tag_ids_array = 0;
    client_t* new_client = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;

    new_client = &g_clients_array[client_id_of_connected_client];

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_connect_message_to_all_clients \n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        int64 audio_state_to_send = 0;

        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->client_id == client_id_of_connected_client)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        json_root_object1 = cJSON_CreateObject();
        json_message_object1 = cJSON_CreateObject();
        // carry the connecting client's tags so identity-restored tags show up on join without a page reload
        json_tag_ids_array = cJSON_CreateIntArray(new_client->tag_ids, (int)cvector_size(new_client->tag_ids));

        cJSON_AddStringToObject(json_message_object1, "type", "client_connect");
        cJSON_AddStringToObject(json_message_object1, "username", new_client->username);
        cJSON_AddStringToObject(json_message_object1, "alias", new_client->alias);
        cJSON_AddStringToObject(json_message_object1, "public_key", new_client->public_key);
        cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)new_client->channel_id);
        cJSON_AddNumberToObject(json_message_object1, "client_id", (double)new_client->client_id);
        cJSON_AddBoolToObject(json_message_object1, "is_music_bot", new_client->is_music_bot);

        audio_state_to_send = new_client->audio_state;

        // only send "PUSH_TO_TALK_ACTIVE" state to connected client for other clients in same channel as he is (root channel in this case)
        // privacy reasons
        if (new_client->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE && client_in_loop->channel_id != new_client->channel_id)
        {
            audio_state_to_send = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }

        cJSON_AddNumberToObject(json_message_object1, "audio_state", audio_state_to_send);
        cJSON_AddStringToObject(json_message_object1, "country_iso_code",
            (g_server_settings.hide_admin_country_flag == TRUE && new_client->is_admin == TRUE) ? "" : new_client->country_iso_code);
        cJSON_AddItemToObject(json_message_object1, "tag_ids", json_tag_ids_array);

        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

        json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        base__free_json_message(json_root_object1, json_root_object1_string);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");
            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }
}

/**
 * @brief This function sends maintainer of current channel where client just joined. Used when client joins the server or when he swiches channel
 *
 * @param client_t* client -> the client to send the maintainer id to
 * @param uint64 channel_id -> id of the channel
 * @param uint64 id_of_client_that_is_maintainer_of_channel -> id of the channel's current maintainer
 *
 * @attention sometimes parameter id_of_client_that_joined_the_channel can have the value as id_of_client_that_is_maintainer_of_channel
 *
 * @return void
 */
void server_msg__send_maintainer_id_to_single_client(client_t* client, uint64 channel_id, uint64 id_of_client_that_is_maintainer_of_channel)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_maintainer_id_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_maintainer_id");
    cJSON_AddNumberToObject(json_message_object1, "maintainer_id", (double)id_of_client_that_is_maintainer_of_channel);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)channel_id);
    cJSON_AddBoolToObject(json_message_object1, "has_maintainer", g_channel_array[channel_id].is_channel_maintainer_present);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    // DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        ws_sendframe_txt(client->p_ws_connection, msg_text);
        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief This function sends connection check response to client
 *
 * @param client_t* client -> self explanatory
 *
 * @attention thanks for your attention
 *
 * @return void
 */
void server_msg__send_connection_check_response_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_connection_check_response_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "connection_check_response");
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    // DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        ws_sendframe_txt(client->p_ws_connection, msg_text);

        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief tells a returning client that its session was adopted (fast reconnect), so it keeps its
 *        state and treats the lists that follow as a refresh. sent before authentication_status
 *
 * @param client_t* client -> the resumed session, already carrying the new socket
 *
 * @return void
 */
void server_msg__send_fast_reconnect_ok_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_fast_reconnect_ok_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "fast_reconnect_ok");
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)client->client_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)client->channel_id);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        ws_sendframe_txt(client->p_ws_connection, msg_text);

        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief This function sends client_rename_message to all authenticated clients
 *
 * @param uint64 id_of_client_that_changed_his_username -> self explanatory
 * @param char* new_username -> self explanatory
 *
 * @attention this function is used within acquired readlock within client_message.c client_msg__process_change_client_username function
 *
 * @return void
 */
void server_msg__send_client_rename_message_to_all_clients(uint64 id_of_client_that_changed_his_username, char* new_username)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_rename_message_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_rename");
    cJSON_AddStringToObject(json_message_object1, "new_username", new_username);
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)id_of_client_that_changed_his_username);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_client_rename_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief tells a single client it should change the admin password (sent once after the first admin login,
 *        because the initial password was typed in cleartext at setup)
 *
 * @param client_t* client -> the client to notify
 *
 * @return void
 */
void server_msg__send_force_admin_password_change_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "force_admin_password_change");
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends an access-denied message to a single client
 *
 * @param client_t* client -> self explanatory
 * @param char* reason -> optional text the client shows instead of its generic denial line; NULL omits it
 *
 * @attention this function is used within acquired readlock within client_message.c , multiple functions
 *
 * @return void
 */
void server_msg__send_access_denied_to_single_client(client_t* client, char* reason)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_access_denied_to_single_client \n");

    cJSON_AddStringToObject(json_message_object1, "type", "access_denied");

    if (reason != NULL_POINTER && reason[0] != 0)
    {
        cJSON_AddStringToObject(json_message_object1, "reason", reason);
    }

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief replies to a single client with the current general-settings toggle values, so the client can
 *        reflect the live server state (used when an admin opens the general-settings tab)
 *
 * @param client_t* client -> the client to reply to
 *
 * @return void
 */
void server_msg__send_server_settings_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_bans = 0;
    cJSON* json_ban = 0;
    cJSON* json_blocked_countries = 0;
    ban_entry_t* ban_in_loop = 0;
    uint64 i = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "server_settings_values");
    cJSON_AddItemToObject(json_message_object1, "display_country_flags", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "hide_admin_country_flag", cJSON_CreateBool(g_server_settings.hide_admin_country_flag == TRUE));
    cJSON_AddItemToObject(json_message_object1, "enable_audio", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "enable_music_bot_audio", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "hide_clients_in_password_channels", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "allow_temp_channels", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_AddItemToObject(json_message_object1, "allow_typing_indicator", cJSON_CreateBool(g_server_settings.allow_typing_indicator == TRUE));
    cJSON_AddItemToObject(json_message_object1, "allow_client_renames", cJSON_CreateBool(g_server_settings.allow_client_renames == TRUE));
    cJSON_AddItemToObject(json_message_object1, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_AddItemToObject(json_message_object1, "allow_private_messages", cJSON_CreateBool(g_server_settings.allow_private_messages == TRUE));
    cJSON_AddItemToObject(json_message_object1, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_AddItemToObject(json_message_object1, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_AddItemToObject(json_message_object1, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_AddItemToObject(json_message_object1, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_AddNumberToObject(json_message_object1, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_AddItemToObject(json_message_object1, "announce_minimum_rsa_key_bits", cJSON_CreateBool(g_server_settings.announce_minimum_rsa_key_bits == TRUE));
    cJSON_AddItemToObject(json_message_object1, "allow_file_uploads", cJSON_CreateBool(g_server_settings.allow_file_uploads == TRUE));
    cJSON_AddNumberToObject(json_message_object1, "file_upload_max_size_mb", (double)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)));
    cJSON_AddNumberToObject(json_message_object1, "chat_picture_max_size_mb", (double)(g_server_settings.chat_picture_max_size_bytes / (1024 * 1024)));
    cJSON_AddItemToObject(json_message_object1, "allow_chat_pictures", cJSON_CreateBool(g_server_settings.allow_chat_pictures == TRUE));
    cJSON_AddItemToObject(json_message_object1, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    json_blocked_countries = cJSON_CreateArray();
    cJSON_AddItemToObject(json_message_object1, "blocked_countries", json_blocked_countries);
    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        cJSON_AddItemToArray(json_blocked_countries, cJSON_CreateString(&g_server_settings.blocked_countries[i][0]));
    }
    cJSON_AddItemToObject(json_message_object1, "log_client_joins", cJSON_CreateBool(g_server_settings.log_client_joins == TRUE));
    cJSON_AddItemToObject(json_message_object1, "log_username_changes", cJSON_CreateBool(g_server_settings.log_username_changes == TRUE));
    cJSON_AddItemToObject(json_message_object1, "log_tag_changes", cJSON_CreateBool(g_server_settings.log_tag_changes == TRUE));
    cJSON_AddItemToObject(json_message_object1, "log_server_settings_updates", cJSON_CreateBool(g_server_settings.log_server_settings_updates == TRUE));
    cJSON_AddItemToObject(json_message_object1, "log_kicks_and_bans", cJSON_CreateBool(g_server_settings.log_kicks_and_bans == TRUE));
    cJSON_AddItemToObject(json_message_object1, "log_client_disconnects", cJSON_CreateBool(g_server_settings.log_client_disconnects == TRUE));
    cJSON_AddItemToObject(json_message_object1, "log_failed_attempts", cJSON_CreateBool(g_server_settings.log_failed_attempts == TRUE));
    cJSON_AddNumberToObject(json_message_object1, "admin_log_max_size_mb", (double)(g_server_settings.admin_log_max_size_bytes / (1024 * 1024)));
    cJSON_AddNumberToObject(json_message_object1, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);

    // include the current ban list so the admin's bans section can render it
    json_bans = cJSON_CreateArray();
    cJSON_AddItemToObject(json_message_object1, "bans", json_bans);
    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        ban_in_loop = &g_ban_array[i];
        if (ban_in_loop->is_existing == FALSE)
        {
            continue;
        }
        json_ban = cJSON_CreateObject();
        cJSON_AddStringToObject(json_ban, "ip_address", &ban_in_loop->ip_address[0]);
        cJSON_AddStringToObject(json_ban, "country_iso_code", &ban_in_loop->country_iso_code[0]);
        cJSON_AddStringToObject(json_ban, "identity", &ban_in_loop->identity[0]);
        cJSON_AddStringToObject(json_ban, "extra_data", &ban_in_loop->extra_data[0]);
        cJSON_AddNumberToObject(json_ban, "timestamp_banned", (double)ban_in_loop->timestamp_banned);
        cJSON_AddItemToArray(json_bans, json_ban);
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief replies to a single client with every admin log line, oldest first (used when an admin
 *        opens or refreshes the log tab). the caller must have checked the client is an admin
 *
 * @param client_t* client -> the client to reply to
 *
 * @return void
 */
void server_msg__send_admin_log_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_lines = 0;

    json_lines = server_logs__build_json_array();
    if (json_lines == NULL_POINTER)
    {
        return;
    }

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "admin_log");
    cJSON_AddItemToObject(json_message_object1, "lines", json_lines);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief broadcasts the client-relevant policy values (file/picture limits, avatars, typing,
 *        aliases) to every connected client, so a settings change applies without a reconnect
 *
 * @note the caller must hold the clients read lock
 *
 * @return void
 */
void server_msg__send_policy_update_to_all_clients(void)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE || client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        json_root_object1 = cJSON_CreateObject();
        json_message_object1 = cJSON_CreateObject();

        cJSON_AddStringToObject(json_message_object1, "type", "server_policy");
        cJSON_AddBoolToObject(json_message_object1, "allow_file_uploads", g_server_settings.allow_file_uploads);
        cJSON_AddNumberToObject(json_message_object1, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);
        cJSON_AddNumberToObject(json_message_object1, "chat_picture_max_size", (double)g_server_settings.chat_picture_max_size_bytes);
        cJSON_AddBoolToObject(json_message_object1, "allow_chat_pictures", g_server_settings.allow_chat_pictures);
        cJSON_AddBoolToObject(json_message_object1, "allow_typing_indicator", g_server_settings.allow_typing_indicator);
        cJSON_AddBoolToObject(json_message_object1, "allow_client_renames", g_server_settings.allow_client_renames);
        cJSON_AddBoolToObject(json_message_object1, "allow_avatars", g_server_settings.allow_avatars);
        cJSON_AddNumberToObject(json_message_object1, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
        cJSON_AddBoolToObject(json_message_object1, "is_alias_registration_allowed", g_server_settings.allow_alias_registrations);
        cJSON_AddBoolToObject(json_message_object1, "is_fast_reconnect_allowed", g_server_settings.is_fast_reconnect_allowed);
        cJSON_AddBoolToObject(json_message_object1, "hide_admin_country_flag", g_server_settings.hide_admin_country_flag);

        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

        json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        base__free_json_message(json_root_object1, json_root_object1_string);

        if (msg_text == NULL_POINTER)
        {
            continue;
        }

        ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);

        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief broadcasts a channel-created event to all clients
 *
 * @param uint64 created_channel_index -> self explanatory
 * @param uint64 channel_creator_client_index -> self explanatory
 *
 * @attention this function is called within two acquired read locks, clients_global_rwlock_guard and channels_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_channel_create_message_to_all_clients(uint64 created_channel_index, uint64 channel_creator_client_index)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;
    channel_t* channel = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_create_message_to_all_clients \n");

    channel = &g_channel_array[created_channel_index];

    cJSON_AddStringToObject(json_message_object1, "type", "channel_create");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "parent_channel_id", channel->parent_channel_id);
    cJSON_AddStringToObject(json_message_object1, "name", channel->name);
    cJSON_AddStringToObject(json_message_object1, "description", channel->description);
    cJSON_AddNumberToObject(json_message_object1, "maintainer_id", channel->maintainer_id);
    cJSON_AddBoolToObject(json_message_object1, "is_using_password", channel->is_using_password);
    cJSON_AddBoolToObject(json_message_object1, "is_audio_enabled", channel->is_audio_enabled);
    cJSON_AddBoolToObject(json_message_object1, "is_root_channel", channel->is_root_channel);
    cJSON_AddBoolToObject(json_message_object1, "has_maintainer", channel->is_channel_maintainer_present);
    cJSON_AddBoolToObject(json_message_object1, "is_temp_channel", channel->is_temp_channel);
    cJSON_AddBoolToObject(json_message_object1, "is_client_limit_active", channel->is_client_limit_active);
    cJSON_AddNumberToObject(json_message_object1, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(json_message_object1, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(json_message_object1, "channel_icon_id", (double)channel->icon_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_creator_id", channel_creator_client_index);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_create_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a channel-edited event to all clients
 *
 * @param uint64 edited_channel_index -> self explanatory
 * @param uint64 channel_editor_id -> id of the client that performed the edit, sent along in the event
 *                                    so receivers know who changed the channel
 *
 * @attention this function is called within two acquired read locks, clients_global_rwlock_guard and channels_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_channel_edit_message_to_all_clients(uint64 edited_channel_index, uint64 channel_editor_id)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;
    channel_t* channel = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_edit_message_to_all_clients \n");

    channel = &g_channel_array[edited_channel_index];

    cJSON_AddStringToObject(json_message_object1, "type", "channel_edit");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
    cJSON_AddStringToObject(json_message_object1, "channel_name", channel->name);
    cJSON_AddStringToObject(json_message_object1, "channel_description", channel->description);
    cJSON_AddBoolToObject(json_message_object1, "is_using_password", (cJSON_bool)channel->is_using_password);
    cJSON_AddBoolToObject(json_message_object1, "is_audio_enabled", (cJSON_bool)channel->is_audio_enabled);
    cJSON_AddBoolToObject(json_message_object1, "is_client_limit_active", (cJSON_bool)channel->is_client_limit_active);
    cJSON_AddNumberToObject(json_message_object1, "max_client_count", (double)channel->max_client_count);
    cJSON_AddBoolToObject(json_message_object1, "has_channel_icon", channel->has_channel_icon);
    cJSON_AddNumberToObject(json_message_object1, "channel_icon_id", (double)channel->icon_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_editor_id", channel_editor_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_edit_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief metadata are sent, so that client knows image is being sent to him.
 *
 * @param uint64 client_index -> self explanatory
 * @param uint64 chat_message_id -> id of server message
 * @param uint64 local_message_id -> id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(uint64 client_index, uint64 chat_message_id, uint64 local_message_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client \n");

    client = &g_clients_array[client_index];

    cJSON_AddStringToObject(json_message_object1, "type", "server_chat_message_id_for_local_message_id");
    cJSON_AddNumberToObject(json_message_object1, "server_chat_message_id", chat_message_id);
    cJSON_AddNumberToObject(json_message_object1, "local_message_id", local_message_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief send chat message itself to client
 *
 * @param uint64 client_sender_id -> id of client that sent the chat message
 * @param uint64 client_receiver_id -> id of client to send the chat message to
 * @param uint64 server_chat_message_id -> server assigned id of the chat message
 * @param char* chat_message_value -> text content of the chat message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_message_to_single_client(uint64 client_sender_id, uint64 client_receiver_id, uint64 server_chat_message_id, char* chat_message_value)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_receiver = 0;
    client_t* client_sender = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_chat_message_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_sender = &g_clients_array[client_sender_id];
    client_receiver = &g_clients_array[client_receiver_id];

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_message");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "sender_username", client_sender->username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender->client_id);
    cJSON_AddNumberToObject(json_message_object1, "server_chat_message_id", server_chat_message_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_message_to_single_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_receiver->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client_receiver->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param uint64 client_sender_id -> self explanatory
 * @param uint64 receiving_channel_id -> self explanatory
 * @param uint64 server_chat_message_id -> server assigned id of the chat message
 * @param char* chat_message_value -> self explanatory
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_message_to_clients_in_same_channel(uint64 client_sender_id, uint64 receiving_channel_id, uint64 server_chat_message_id, char* chat_message_value)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_chat_message_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_sender = &g_clients_array[client_sender_id];

    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_message");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "sender_username", client_sender->username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", receiving_channel_id);
    cJSON_AddNumberToObject(json_message_object1, "server_chat_message_id", server_chat_message_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_message_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->channel_id != receiving_channel_id)
        {
            continue;
        }

        if (client_in_loop->client_id == client_sender_id)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends a delete/edit action for a chat message to a SINGLE client (private/direct case). the action
 *        carries the target server chat id and the requester's public key + admin flag, so the receiver can
 *        decide for itself whether to honour it (author or admin). for an edit it also carries the new value.
 *
 * @param uint64 client_receiver_id -> id of the client to send the action to
 * @param char* action_type -> "chat_message_delete" or "chat_message_edit"
 * @param uint64 target_chat_message_id -> server chat id of the message to act on
 * @param char* requester_public_key -> public key of the client that requested the action
 * @param boole requester_is_admin -> whether the requester is an admin
 * @param char* new_message_value -> new text for an edit, or NULL_POINTER for a delete
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_chat_message_action_to_single_client(uint64 client_receiver_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_receiver = 0;

    client_receiver = &g_clients_array[client_receiver_id];

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", action_type);
    cJSON_AddNumberToObject(json_message_object1, "chat_message_id", target_chat_message_id);
    cJSON_AddStringToObject(json_message_object1, "requester_public_key", requester_public_key);
    cJSON_AddBoolToObject(json_message_object1, "requester_is_admin", requester_is_admin == TRUE);
    if (new_message_value != NULL_POINTER)
    {
        cJSON_AddStringToObject(json_message_object1, "new_message_value", new_message_value);
    }

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_receiver->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client_receiver->p_ws_connection, msg_text);
    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends a delete/edit action for a chat message to every client in a channel (channel case). unlike a
 *        normal chat broadcast this does NOT skip the requester, so the message updates in their own view too.
 *
 * @param uint64 receiving_channel_id -> the channel whose clients receive the action
 * @param char* action_type -> "chat_message_delete" or "chat_message_edit"
 * @param uint64 target_chat_message_id -> server chat id of the message to act on
 * @param char* requester_public_key -> public key of the client that requested the action
 * @param boole requester_is_admin -> whether the requester is an admin
 * @param char* new_message_value -> new text for an edit, or NULL_POINTER for a delete
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_chat_message_action_to_clients_in_same_channel(uint64 receiving_channel_id, char* action_type, uint64 target_chat_message_id, char* requester_public_key, boole requester_is_admin, char* new_message_value)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_in_loop = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", action_type);
    cJSON_AddNumberToObject(json_message_object1, "chat_message_id", target_chat_message_id);
    cJSON_AddStringToObject(json_message_object1, "requester_public_key", requester_public_key);
    cJSON_AddBoolToObject(json_message_object1, "requester_is_admin", requester_is_admin == TRUE);
    if (new_message_value != NULL_POINTER)
    {
        cJSON_AddStringToObject(json_message_object1, "new_message_value", new_message_value);
    }

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->channel_id != receiving_channel_id)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends chat-picture metadata to all clients in the sender's channel
 *
 * @param uint64 client_sender_id -> self explanatory
 * @param uint64 receiving_channel_id -> self explanatory
 * @param uint64 server_chat_message_id -> server assigned id of the picture
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(uint64 client_sender_id, uint64 receiving_channel_id, uint64 server_chat_message_id, uint64 encrypted_size)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_picture_metadata");
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", receiving_channel_id);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", server_chat_message_id);
    // the encrypted length the receiver counts its chunks against, for the progress ring
    cJSON_AddNumberToObject(json_message_object1, "size", (double)encrypted_size);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop->channel_id != receiving_channel_id)
        {
            DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel :client->channel_id != receiving_channel_id \n");
            DBG_SERVER_MESSAGE log_info("%s %lld %s", "client->channel_id", client_in_loop->channel_id, "\n");
            DBG_SERVER_MESSAGE log_info("%s %llu %s", "receiving_channel_id", receiving_channel_id, "\n");

            continue;
        }

        if (client_in_loop->client_id == client_sender_id)
        {
            continue;
        }

        DBG_SERVER_MESSAGE log_info("%s %lld %s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel: sending to client ", client_in_loop->client_id, "\n");

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param uint64 client_sender_id -> self explanatory
 * @param uint64 receiving_channel_id -> self explanatory
 * @param uint64 server_chat_message_id -> server assigned id of the picture message
 * @param char* chat_message_value -> self explanatory
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_channel_chat_picture_to_clients_in_same_channel(uint64 client_sender_id, uint64 receiving_channel_id, uint64 server_chat_message_id, char* chat_message_value)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_chat_picture_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_sender = &g_clients_array[client_sender_id];

    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_picture");
    cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
    cJSON_AddStringToObject(json_message_object1, "username", client_sender->username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", receiving_channel_id);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", server_chat_message_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_message_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->channel_id != receiving_channel_id)
        {
            continue;
        }

        if (client_in_loop->client_id == client_sender_id)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief This function sends image status to the client that SENT the image so client knows that server received and sent his message to other clients / client
 *
 * @param client_t* client -> self explanatory
 * @param char* status -> status string that is put into the "value" field of the image_sent_status message
 *
 * @attention thanks for your attention
 *
 * @return void
 */
void server_msg__send_image_status_to_single_client(client_t* client, char* status)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_image_status_to_single_client \n");

    cJSON_AddStringToObject(json_message_object1, "type", "image_sent_status");
    cJSON_AddStringToObject(json_message_object1, "value", status);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    // DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");
    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief send chat picture metadata to a single client
 *
 * @param uint64 client_sender_id -> id of client that sent the picture
 * @param uint64 client_receiver_id -> id of client to send the picture metadata to
 * @param uint64 server_chat_message_id -> server assigned id of the picture
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_picture_metadata_to_single_client(uint64 client_sender_id, uint64 client_receiver_id, uint64 server_chat_message_id, uint64 encrypted_size)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_receiver = 0;
    client_t* client_sender = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_chat_picture_metadata_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_sender = &g_clients_array[client_sender_id];
    client_receiver = &g_clients_array[client_receiver_id];

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_picture_metadata");
    cJSON_AddStringToObject(json_message_object1, "sender_username", client_sender->username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender->client_id);
    cJSON_AddNumberToObject(json_message_object1, "picture_id", server_chat_message_id);
    // the encrypted length the receiver counts its chunks against, for the progress ring
    cJSON_AddNumberToObject(json_message_object1, "size", (double)encrypted_size);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_picture_to_single_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_receiver->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client_receiver->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends chat-file metadata to all clients in the sender's channel: the sender's encrypted
 *        name/size/mime header (opaque here) and the encrypted body length, so receivers can draw
 *        the file card and a progress ring before the chunks arrive
 *
 * @param uint64 client_sender_id -> self explanatory
 * @param uint64 receiving_channel_id -> self explanatory
 * @param uint64 server_chat_message_id -> server assigned id of the file
 * @param char* file_header -> the sender's encrypted header, forwarded as is
 * @param uint64 encrypted_size -> length of the encrypted body that follows in chunks
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_channel_chat_file_metadata_to_clients_in_same_channel(uint64 client_sender_id, uint64 receiving_channel_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_in_loop = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_chat_file_metadata_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_file_metadata");
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", receiving_channel_id);
    cJSON_AddNumberToObject(json_message_object1, "file_id", server_chat_message_id);
    cJSON_AddStringToObject(json_message_object1, "file_header", file_header);
    cJSON_AddNumberToObject(json_message_object1, "encrypted_size", (double)encrypted_size);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop->channel_id != receiving_channel_id)
        {
            continue;
        }

        if (client_in_loop->client_id == client_sender_id)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends chat-file metadata to a single client (direct chat): the sender's encrypted
 *        name/size/mime header (opaque here) and the encrypted body length
 *
 * @param uint64 client_sender_id -> id of client that sent the file
 * @param uint64 client_receiver_id -> id of client to send the file metadata to
 * @param uint64 server_chat_message_id -> server assigned id of the file
 * @param char* file_header -> the sender's encrypted header, forwarded as is
 * @param uint64 encrypted_size -> length of the encrypted body that follows in chunks
 *
 * @attention this function is used within read lock on clients array
 *
 * @return void
 */
void server_msg__send_chat_file_metadata_to_single_client(uint64 client_sender_id, uint64 client_receiver_id, uint64 server_chat_message_id, char* file_header, uint64 encrypted_size)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_receiver = 0;
    client_t* client_sender = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_chat_file_metadata_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_sender = &g_clients_array[client_sender_id];
    client_receiver = &g_clients_array[client_receiver_id];

    cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_file_metadata");
    cJSON_AddStringToObject(json_message_object1, "sender_username", client_sender->username);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender->client_id);
    cJSON_AddNumberToObject(json_message_object1, "file_id", server_chat_message_id);
    cJSON_AddStringToObject(json_message_object1, "file_header", file_header);
    cJSON_AddNumberToObject(json_message_object1, "encrypted_size", (double)encrypted_size);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_receiver->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client_receiver->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief tells the client that SENT a chat file why the server refused it, so the failure is never
 *        silent on its side. reasons: file_uploads_disabled, file_too_large, receiver_unavailable,
 *        private_messages_disabled
 *
 * @param client_t* client -> the sender
 * @param char* reason -> one of the reason strings above
 * @param uint64 local_message_id -> the sender's own id of the refused message, so it can mark that card
 *
 * @attention this function is used within acquired lock on clients array
 *
 * @return void
 */
void server_msg__send_file_send_error_to_single_client(client_t* client, char* reason, uint64 local_message_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s %s %s", "server_msg__send_file_send_error_to_single_client", reason, "\n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "file_send_error");
    cJSON_AddStringToObject(json_message_object1, "reason", reason);
    cJSON_AddNumberToObject(json_message_object1, "local_message_id", (double)local_message_id);
    cJSON_AddNumberToObject(json_message_object1, "file_upload_max_size", (double)g_server_settings.file_upload_max_size_bytes);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends channel join message to all clients
 *
 * @param client_t* client_that_switched_channel -> self explanatory
 * @param channel_t* new_channel -> id of server message
 *
 * @attention
 * this function gets called AFTER new channel id is assigned to client struct, not before
 * so this function must assume client is already in his new channel, if it is going to do any logical operations based on that
 *
 * @return void
 */
void server_msg__send_channel_join_message_to_all_clients(client_t* client_that_switched_channel, channel_t* new_channel)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_root_object1_client_hidden_type = 0;
    cJSON* json_message_object1_client_hidden_type = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    char* json_root_object1_string_client_hidden_type = 0;

    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client_in_loop = 0;
    char* song_name = 0;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_join_message_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    json_root_object1_client_hidden_type = cJSON_CreateObject();
    json_message_object1_client_hidden_type = cJSON_CreateObject();

    // clients not in same channel will not receive real time microphone usage information from the client that switched the channel
    is_streaming_song = client_that_switched_channel->is_streaming_song;
    audio_state = client_that_switched_channel->audio_state;
    song_name = client_that_switched_channel->song_name;

    if (client_that_switched_channel->channel_id != new_channel->channel_id)
    {
        if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        is_streaming_song = FALSE;
        song_name = "";
    }

    cJSON_AddStringToObject(json_message_object1, "type", "channel_join");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", new_channel->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_that_switched_channel->client_id);
    cJSON_AddNumberToObject(json_message_object1, "audio_state", audio_state);
    cJSON_AddBoolToObject(json_message_object1, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(json_message_object1, "song_name", song_name);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    cJSON_AddStringToObject(json_message_object1_client_hidden_type, "type", "channel_join");
    cJSON_AddNumberToObject(json_message_object1_client_hidden_type, "channel_id", (double)-1);
    cJSON_AddNumberToObject(json_message_object1_client_hidden_type, "client_id", client_that_switched_channel->client_id);
    cJSON_AddNumberToObject(json_message_object1_client_hidden_type, "audio_state", audio_state);
    cJSON_AddBoolToObject(json_message_object1_client_hidden_type, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(json_message_object1_client_hidden_type, "song_name", song_name);
    cJSON_AddItemToObject(json_root_object1_client_hidden_type, "message", json_message_object1_client_hidden_type);
    json_root_object1_string_client_hidden_type = cJSON_PrintUnformatted(json_root_object1_client_hidden_type);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_join_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        boole is_hide_client_active = FALSE;

        client_in_loop = &g_clients_array[x];

        if (client_in_loop->is_existing == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        is_hide_client_active = FALSE;

        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)
        {
            if (client_in_loop->channel_id != new_channel->channel_id)
            {
                if (new_channel->is_using_password == TRUE)
                {
                    is_hide_client_active = TRUE;
                }
            }
        }
        if (is_hide_client_active == TRUE)
        {
            size_of_allocated_message_buffer = 0;
            msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string_client_hidden_type, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

            if (msg_text == NULL_POINTER)
            {
                goto label_server_msg__send_channel_join_message_to_all_clients_end;
            }
        }
        else
        {
            size_of_allocated_message_buffer = 0;
            msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

            if (msg_text == NULL_POINTER)
            {
                goto label_server_msg__send_channel_join_message_to_all_clients_end;
            }
        }

        DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

        ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
        memorymanager__free((nuint)msg_text);
    }

label_server_msg__send_channel_join_message_to_all_clients_end:

    base__free_json_message(json_root_object1, json_root_object1_string);
    base__free_json_message(json_root_object1_client_hidden_type, json_root_object1_string_client_hidden_type);
}

/**
 * @brief sends a channel_join message about one client to one single receiving client
 *
 *        when the receiving client is not in the same channel as the client that switched, real time
 *        microphone information is stripped before sending, push to talk active is downgraded to push
 *        to talk enabled and the streamed song name is cleared.
 *
 * @param client_t* client_that_switched_channel -> the client whose channel change is being announced
 * @param channel_t* new_channel -> the channel that client_that_switched_channel joined
 * @param client_t* receiving_client -> the single client this message is encrypted for and sent to
 *
 * @return void
 */
void server_msg__send_channel_join_message_to_single_client(client_t* client_that_switched_channel, channel_t* new_channel, client_t* receiving_client)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;

    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    char* song_name = 0;
    boole is_streaming_song = FALSE;
    int64 audio_state = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_join_message_to_single_client \n");

    // clients not in same channel will not receive real time microphone usage information from the client that switched the channel
    is_streaming_song = client_that_switched_channel->is_streaming_song;
    audio_state = client_that_switched_channel->audio_state;
    song_name = client_that_switched_channel->song_name;

    if (client_that_switched_channel->channel_id != new_channel->channel_id)
    {
        if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
        {
            audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }
        is_streaming_song = FALSE;
        song_name = "";
    }

    cJSON_AddStringToObject(json_message_object1, "type", "channel_join");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", new_channel->channel_id);
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_that_switched_channel->client_id);
    cJSON_AddNumberToObject(json_message_object1, "audio_state", audio_state);
    cJSON_AddBoolToObject(json_message_object1, "is_streaming_song", is_streaming_song);
    cJSON_AddStringToObject(json_message_object1, "song_name", song_name);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_join_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, receiving_client->dh_shared_secret);

    if (msg_text == NULL_POINTER)
    {
        goto label_server_msg__send_channel_join_message_to_all_clients_end;
    }

    DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

    ws_sendframe_txt(receiving_client->p_ws_connection, msg_text);
    memorymanager__free((nuint)msg_text);

label_server_msg__send_channel_join_message_to_all_clients_end:

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts the channel's maintainer id to all clients in that channel
 *
 * @param uint64 channel_id -> self explanatory
 * @param uint64 maintainer_id -> id of server message
 *
 * @attention used within write lock for clients and channels
 *
 * @return void
 */
void server_msg__send_maintainer_id_to_clients_in_same_channel(uint64 channel_id, uint64 maintainer_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_maintainer_id_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_maintainer_id");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", channel_id);
    cJSON_AddNumberToObject(json_message_object1, "maintainer_id", maintainer_id);
    cJSON_AddBoolToObject(json_message_object1, "has_maintainer", g_channel_array[channel_id].is_channel_maintainer_present);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_maintainer_id_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->channel_id != channel_id)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a channel-deleted event to all clients
 *
 * @param uint64 deleted_channel_id -> id of the deleted channel
 * @param uint64 channel_deletor_id -> id of the client that deleted it
 *
 * @attention used within write lock for clients and channels
 *
 * @return void
 */
void server_msg__send_channel_delete_message_to_all_clients(uint64 deleted_channel_id, uint64 channel_deletor_id)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client = 0;
    channel_t* channel = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_delete_message_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_delete");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", deleted_channel_id);
    cJSON_AddNumberToObject(json_message_object1, "channel_deletor_id", channel_deletor_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_delete_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client = &g_clients_array[x];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a client-disconnected event to all clients
 *
 * @param uint64 client_index -> id of the disconnected client
 *
 * @attention used within acquired write lock for clients_array
 *
 * @return void
 */
void server_msg__send_client_disconnect_message_to_all_clients(uint64 client_index)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_disconnect_message_to_all_clients \n");

    cJSON_AddStringToObject(json_message_object1, "type", "client_disconnect");
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_index);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_client_disconnect_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client = &g_clients_array[x];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->client_id == client_index)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends a poke message to a single client
 *
 * @param client_t* client -> the client to poke
 * @param uint64 sender_index -> id of the client sending the poke
 * @param char* poke_message -> the poke text
 *
 * @return void
 */
void server_msg__send_poke_to_single_client(client_t* client, uint64 sender_index, char* poke_message)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_poke_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "poke");
    cJSON_AddNumberToObject(json_message_object1, "client_id", sender_index);
    cJSON_AddStringToObject(json_message_object1, "poke_message", poke_message);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_picture_to_single_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief tells the people a client is writing to that he is typing. carries no message content at
 *        all - only who is typing and which conversation it belongs to, so the receiver can show it
 *        against the right chat and drop it again when it stops being refreshed.
 *
 * @param uint64 sender_client_id -> the client that is typing
 * @param char* receiver_type -> "channel" or "user", the kind of conversation being written to
 * @param uint64 receiver_id -> channel id for "channel", target client id for "user"
 *
 * @attention the caller must already hold a read lock on clients_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_typing_indicator(uint64 sender_client_id, char* receiver_type, uint64 receiver_id)
{
    char* json_root_object_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    boole is_direct_message = FALSE;
    client_t* client_in_loop = 0;
    client_t* client_sender = 0;
    cJSON* json_root_object = 0;
    cJSON* json_message_object = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_typing_indicator \n");

    client_sender = &g_clients_array[sender_client_id];
    is_direct_message = clib__is_string_equal(receiver_type, "user");

    json_root_object = cJSON_CreateObject();
    json_message_object = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object, "type", "typing_indicator");
    cJSON_AddNumberToObject(json_message_object, "client_id", sender_client_id);
    cJSON_AddStringToObject(json_message_object, "receiver_type", receiver_type);
    cJSON_AddNumberToObject(json_message_object, "receiver_id", (double)receiver_id);

    cJSON_AddItemToObject(json_root_object, "message", json_message_object);

    json_root_object_string = cJSON_PrintUnformatted(json_root_object);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        if (client_in_loop->is_existing == FALSE || client_in_loop->is_authenticated == FALSE)
        {
            continue;
        }

        if (client_in_loop->is_music_bot == TRUE)
        {
            continue;
        }

        if (client_in_loop->client_id == sender_client_id)
        {
            continue;  // he knows he is typing
        }

        // a direct message goes to that one person, a channel one to whoever stands in the channel
        if (is_direct_message == TRUE)
        {
            if (client_in_loop->client_id != receiver_id)
            {
                continue;
            }
        }
        else if (client_in_loop->channel_id != receiver_id)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object_string, &size_of_allocated_message_buffer, client_in_loop->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client_in_loop->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object, json_root_object_string);
}

/**
 * @brief sends a WebRTC SDP offer to a single client
 *
 * @param const char* candidate -> the ICE candidate string
 * @param const char* mid -> the sdpMid the candidate belongs to
 * @param client_t* client -> the client to send the candidate to
 *
 * @return void
 */
void server_msg__send_webrtc_sdp_offer_to_single_client(const char* candidate, const char* mid, client_t* client)
{
    cJSON* json_root_object = NULL_POINTER;
    cJSON* json_message_object = NULL_POINTER;
    cJSON* json_message_value = NULL_POINTER;
    cJSON* jmid = NULL_POINTER;
    cJSON* jsoncandidate = NULL_POINTER;
    cJSON* jsonlineindex = NULL_POINTER;
    char* json_root_object_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    json_root_object = cJSON_CreateObject();
    json_message_object = cJSON_CreateObject();
    json_message_value = cJSON_CreateObject();

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_webrtc_sdp_offer_to_single_client \n");

    jsoncandidate = cJSON_CreateString(candidate);
    cJSON_AddItemToObject(json_message_value, "candidate", jsoncandidate);

    jmid = cJSON_CreateString(mid);
    cJSON_AddItemToObject(json_message_value, "sdpMid", jmid);

    jsonlineindex = cJSON_CreateNumber(0.0);
    cJSON_AddItemToObject(json_message_value, "sdpMLineIndex", jsonlineindex);

    cJSON_AddItemToObject(json_message_object, "value", json_message_value);

    cJSON_AddStringToObject(json_message_object, "type", "ice_candidate");

    cJSON_AddItemToObject(json_root_object, "message", json_message_object);

    json_root_object_string = cJSON_PrintUnformatted(json_root_object);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    DBG_AUDIOCHANNEL_WEBRTC log_info("%s", "peerconnection_oncandidate_callback CANDIDATE SEND \n");
    base__free_json_message(json_root_object, json_root_object_string);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief broadcasts a client's audio state to all clients
 *
 * @param uint64 client_whose_audio_to_send -> id of the client whose audio state is broadcast
 * @param uint64 state -> the client's new audio state
 *
 * @return void
 */
void server_msg__send_audio_state_of_client_to_all_clients(uint64 client_whose_audio_to_send, uint64 state)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 i = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client = 0;
    int64 audio_state_to_send = 0;
    boole is_hide_client_active = FALSE;
    boole status = FALSE;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_audio_state_of_client_to_all_clients \n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        status = util__is_client_valid_and_not_music_bot(i);

        if (status == FALSE)
        {
            continue;
        }

        client = &g_clients_array[i];

        is_hide_client_active = FALSE;

        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)
        {
            if (client->channel_id != g_clients_array[client_whose_audio_to_send].channel_id)
            {
                // if channel of receiving client and client that is sending audio state, isn't same
                // and client that is sending audio state is located in password protected channel
                // skip client
                if (g_clients_array[client_whose_audio_to_send].channel_id < g_server_settings.max_channel_count
                    && g_channel_array[g_clients_array[client_whose_audio_to_send].channel_id].is_existing == TRUE
                    && g_channel_array[g_clients_array[client_whose_audio_to_send].channel_id].is_using_password == TRUE)
                {
                    is_hide_client_active = TRUE;
                }
            }
        }

        if (is_hide_client_active == TRUE)
        {
            continue;
        }

        audio_state_to_send = state;

        // only send microphone active state to clients in same channel as sender
        if (client->channel_id != g_clients_array[client_whose_audio_to_send].channel_id)
        {
            if (state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
            {
                audio_state_to_send = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
            }
        }

        json_root_object1 = cJSON_CreateObject();
        json_message_object1 = cJSON_CreateObject();

        cJSON_AddStringToObject(json_message_object1, "type", "audio_state_of_single_client");
        cJSON_AddNumberToObject(json_message_object1, "client_id", client_whose_audio_to_send);
        cJSON_AddNumberToObject(json_message_object1, "value", audio_state_to_send);
        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

        json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
            base__free_json_message(json_root_object1, json_root_object1_string);
        }
    }
}

/**
 * @brief tells clients in the channel that a song stream has started
 *
 * @param client_t* client_that_streams -> the streaming client
 *
 * @return void
 */
void server_msg__send_start_song_stream_message_to_clients_in_same_channel(client_t* client_that_streams)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_start_song_stream_message_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "start_song_stream");
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_that_streams->client_id);
    cJSON_AddStringToObject(json_message_object1, "song_name", client_that_streams->song_name);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_start_song_stream_message_to_clients_in_same_channel ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->channel_id != client_that_streams->channel_id)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief tells clients in the channel that a song stream has stopped
 *
 * @param client_t* client_that_streams -> the streaming client
 *
 * @return void
 */
void server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client_t* client_that_streams)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_stop_song_stream_message_to_clients_in_same_channel \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "stop_song_stream");
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_that_streams->client_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server__msg__send_stop_song_stream_message_to_clients_in_same_channel ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        if (client->channel_id != client_that_streams->channel_id)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "server__msg__send_stop_song_stream_message_to_clients_in_same_channel: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a tag-added-to-client event to all clients
 *
 * @param uint64 client_id_of_client_that_got_the_new_tag -> id of the client that received the tag
 * @param uint64 tag_id -> id of the tag added
 *
 * @return void
 */
void server_msg__send_add_tag_to_client_event_to_all_clients(uint64 client_id_of_client_that_got_the_new_tag, uint64 tag_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_add_tag_to_client_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "tag_add_to_client");
    cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_id_of_client_that_got_the_new_tag);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_add_tag_to_client_event_to_all_clients ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_add_tag_to_client_event_to_all_clients: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts "this client's alias changed" (with the alias text; empty = cleared) to all
 *        authenticated non-bot clients. must be called with the clients lock already held
 *        (like the tag-event broadcasts).
 *
 * @param uint64 client_id -> the client whose identity just got the alias
 * @param char* alias -> the new alias text, empty or NULL when cleared
 *
 * @return void
 */
void server_msg__send_client_alias_changed_to_all_clients(uint64 client_id, char* alias)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_alias_changed_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_alias_changed");
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_id);
    cJSON_AddStringToObject(json_message_object1, "alias", (alias == NULL_POINTER) ? "" : alias);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a lightweight "this client's avatar changed" event (no image payload) to all
 *        authenticated non-bot clients, so they can re-request that client's avatar if they show it.
 *
 * @param uint64 client_id_whose_avatar_changed -> id of the client whose avatar was set/deleted
 *
 * @note must be called with the clients lock already held (like the tag-event broadcasts)
 *
 * @return void
 */
void server_msg__send_avatar_changed_event_to_all_clients(uint64 client_id_whose_avatar_changed)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "avatar_changed");
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)client_id_whose_avatar_changed);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE)
        {
            continue;
        }
        if (client->is_authenticated == FALSE)
        {
            continue;
        }
        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends one client's avatar (full base64 data-url, or empty string if none) to a single
 *        requesting client. used for both the profile-pane request and the chunked tree lazy-load,
 *        so avatars are only ever sent to clients that asked for them.
 *
 * @param ws_cli_conn_t* websocket        -> the requester's connection
 * @param char*          dh_shared_secret -> the requester's shared secret (to encrypt with)
 * @param uint64         client_id        -> whose avatar this is
 * @param char*          base64_avatar    -> the avatar data-url, or "" for none
 *
 * @return void
 */
void server_msg__send_client_avatar_to_single_client(ws_cli_conn_t* websocket, char* dh_shared_secret, uint64 client_id, char* base64_avatar)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    if (websocket == NULL_POINTER || dh_shared_secret == NULL_POINTER)
    {
        return;
    }

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_avatar");
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)client_id);
    cJSON_AddStringToObject(json_message_object1, "base64_avatar", (base64_avatar != NULL_POINTER) ? base64_avatar : "");

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    if (json_root_object1_string != NULL_POINTER)
    {
        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(websocket, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends the stored-identity list to one client, so it can show the people registered on this
 *        server even while they are offline.
 *
 *        every entry carries the alias, the avatar and the tag ids. no identity hash and no username
 *        is sent: the alias is the only handle, and it is what the client pairs against a live client
 *        of the same alias. the raw public key is added on top only while allow_offline_messages is
 *        enabled, since that is what lets a peer encrypt to somebody who is not connected - it is
 *        public by definition, but it still only goes to registered requesters (the caller checked
 *        that). last_seen (unix seconds) is added only while allow_last_seen is enabled, so nothing
 *        leaks while the setting is off. slots with no public key or no alias are skipped - they are
 *        free, or they have no name to show and nothing to pair on.
 *
 * @param ws_cli_conn_t* websocket -> the requesting client's connection
 * @param char* dh_shared_secret -> that client's shared secret, used for the encryption
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock), exactly like
 *       the identity serializer in base.c.
 *
 * @return void
 */
void server_msg__send_stored_clients_to_single_client(ws_cli_conn_t* websocket, char* dh_shared_secret)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    uint64 t = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_stored_clients = 0;
    cJSON* json_stored_client = 0;
    cJSON* json_tag_ids = 0;

    if (websocket == NULL_POINTER || dh_shared_secret == NULL_POINTER)
    {
        return;
    }

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_stored_clients = cJSON_CreateArray();

    cJSON_AddStringToObject(json_message_object1, "type", "stored_clients_list");

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue; // free slot, or nothing to name and pair it by
        }

        json_stored_client = cJSON_CreateObject();
        cJSON_AddStringToObject(json_stored_client, "alias", &g_client_stored_data[i].alias[0]);
        cJSON_AddStringToObject(json_stored_client, "base64_avatar", &g_client_stored_data[i].base64_avatar[0]);

        // with offline messages on, registered peers also get this identity's public key: it is what
        // lets them encrypt a message to somebody who is not connected. a public key is public by
        // definition, but it is still only handed to REGISTERED requesters (the caller checked) and
        // only while the feature is enabled
        if (g_server_settings.allow_offline_messages == TRUE && g_client_stored_data[i].raw_public_key[0] != 0)
        {
            cJSON_AddStringToObject(json_stored_client, "public_key", &g_client_stored_data[i].raw_public_key[0]);
        }

        // when the admin enabled last-seen, tell the client when this identity was last connected
        // (unix seconds). left out entirely while the setting is off, so nothing leaks
        if (g_server_settings.allow_last_seen == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            cJSON_AddNumberToObject(json_stored_client, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds);
        }

        json_tag_ids = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(json_tag_ids, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(json_stored_client, "tag_ids", json_tag_ids);

        cJSON_AddItemToArray(json_stored_clients, json_stored_client);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    cJSON_AddItemToObject(json_message_object1, "stored_clients", json_stored_clients);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    if (json_root_object1_string != NULL_POINTER)
    {
        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(websocket, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief delivers everything that was said to this client's identity while it was offline, oldest
 *        first, then drops the queue for that identity. each message goes out as its own
 *        offline_chat_message so the client can render them like any other direct message; the
 *        payload is the sender's ciphertext, untouched (the server never could read it).
 *
 * @param client_t* client -> the freshly authenticated client
 *
 * @return void
 *
 * @note takes g_offline_messages_mutex (leaf lock). caller holds the clients lock.
 */
void server_msg__send_queued_offline_messages_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    char recipient_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 delivered_count = 0;
    uint64 lowest_sequence_number = 0;
    int64 next_slot = 0;

    if (g_offline_messages == NULL_POINTER || client == NULL_POINTER || client->public_key[0] == 0)
    {
        return;
    }

    base__hash_password_to_base64(client->public_key, recipient_identity_hash, sizeof(recipient_identity_hash));

    // deliver in the order they were sent: repeatedly pick the lowest sequence number still queued
    // for this identity. the queue is small and this runs once per connect
    while (TRUE)
    {
        next_slot = -1;
        lowest_sequence_number = 0;

        pthread_mutex_lock(&g_offline_messages_mutex);

        for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
        {
            if (g_offline_messages[i].is_used == FALSE)
            {
                continue;
            }

            if (clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], recipient_identity_hash) == FALSE)
            {
                continue;
            }

            if (next_slot == -1 || g_offline_messages[i].sequence_number < lowest_sequence_number)
            {
                next_slot = (int64)i;
                lowest_sequence_number = g_offline_messages[i].sequence_number;
            }
        }

        if (next_slot == -1)
        {
            pthread_mutex_unlock(&g_offline_messages_mutex);
            break;
        }

        json_root_object1 = cJSON_CreateObject();
        json_message_object1 = cJSON_CreateObject();

        cJSON_AddStringToObject(json_message_object1, "type", "offline_chat_message");
        cJSON_AddStringToObject(json_message_object1, "value", g_offline_messages[next_slot].base64_encrypted_message);
        cJSON_AddStringToObject(json_message_object1, "sender_alias", &g_offline_messages[next_slot].sender_alias[0]);
        cJSON_AddNumberToObject(json_message_object1, "queued_unix_seconds", (double)g_offline_messages[next_slot].queued_unix_seconds);
        cJSON_AddNumberToObject(json_message_object1, "sequence_number", (double)g_offline_messages[next_slot].sequence_number);

        // free this one before sending, so a failing send cannot leave it queued forever
        if (g_offline_messages[next_slot].base64_encrypted_message != NULL_POINTER)
        {
            memorymanager__free((nuint)g_offline_messages[next_slot].base64_encrypted_message);
        }
        clib__null_memory(&g_offline_messages[next_slot], sizeof(offline_chat_message_t));

        pthread_mutex_unlock(&g_offline_messages_mutex);

        cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);
        json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

        if (json_root_object1_string != NULL_POINTER)
        {
            size_of_allocated_message_buffer = 0;
            msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

            if (msg_text != NULL_POINTER)
            {
                ws_sendframe_txt(client->p_ws_connection, msg_text);
                memorymanager__free((nuint)msg_text);
            }
        }

        base__free_json_message(json_root_object1, json_root_object1_string);

        delivered_count++;
    }

    if (delivered_count > 0)
    {
        DBG_SERVER_MESSAGE log_info("%s %llu %s", "delivered", delivered_count, "offline message(s) on connect \n");
    }
}

/**
 * @brief broadcasts a tag-removed-from-client event to all clients
 *
 * @param uint64 client_id_of_client_that_got_tag_removed -> id of the client that lost the tag
 * @param uint64 tag_id -> id of the tag removed
 *
 * @return void
 */
void server_msg__send_remove_tag_from_client_event_to_all_clients(uint64 client_id_of_client_that_got_tag_removed, uint64 tag_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_sender = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_remove_tag_from_client_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "remove_tag_from_client");
    cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
    cJSON_AddNumberToObject(json_message_object1, "client_id", client_id_of_client_that_got_tag_removed);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_remove_tag_from_client_event_to_all_clients ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_remove_tag_from_client_event_to_all_clients: msg_text ", msg_text, "\n");
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a new-icon-added event to all clients
 *
 * @param uint64 new_icon_id -> id of the new icon
 * @param char* icon_base64_value -> base64-encoded icon image
 *
 * @return void
 */
void server_msg__send_add_new_icon_event_to_all_clients(uint64 new_icon_id, char* icon_base64_value)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_add_new_icon_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "icon_add");
    cJSON_AddNumberToObject(json_message_object1, "icon_id", new_icon_id);
    cJSON_AddStringToObject(json_message_object1, "base64_icon", icon_base64_value);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_add_new_icon_event_to_all_clients ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_remove_tag_from_client_event_to_all_clients: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a tag-deleted-from-pool event to all clients
 *
 * @param uint64 tag_id -> id of the tag that was deleted
 *
 * @return void
 */
void server_msg__send_remove_tag_event_to_all_clients(uint64 tag_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_remove_tag_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "tag_delete");
    cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts an icon-deleted-from-pool event to all clients
 *
 * @param uint64 icon_id -> id of the icon that was deleted
 *
 * @return void
 */
void server_msg__send_remove_icon_event_to_all_clients(uint64 icon_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_remove_icon_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "icon_delete");
    cJSON_AddNumberToObject(json_message_object1, "icon_id", icon_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a tag-icon-changed event to all clients
 *
 * @param uint64 tag_id -> id of the tag whose icon changed
 * @param boole has_icon -> whether the tag now has an icon
 * @param uint64 icon_id -> the tag's icon id (meaningful only when has_icon is TRUE)
 *
 * @return void
 */
void server_msg__send_tag_icon_changed_event_to_all_clients(uint64 tag_id, boole has_icon, uint64 icon_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_tag_icon_changed_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "tag_icon_changed");
    cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
    cJSON_AddItemToObject(json_message_object1, "has_icon", cJSON_CreateBool(has_icon == TRUE));
    cJSON_AddNumberToObject(json_message_object1, "tag_linked_icon_id", icon_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a channel's icon change to every authenticated client so their channel row updates live
 *
 * @param uint64 channel_id -> id of the channel whose icon changed
 * @param boole has_channel_icon -> whether the channel now has an icon
 * @param uint64 icon_id -> the channel's icon id (meaningful only when has_channel_icon is TRUE)
 *
 * @return void
 */
void server_msg__send_channel_icon_changed_event_to_all_clients(uint64 channel_id, boole has_channel_icon, uint64 icon_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_channel_icon_changed_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "channel_icon_changed");
    cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)channel_id);
    cJSON_AddItemToObject(json_message_object1, "has_channel_icon", cJSON_CreateBool(has_channel_icon == TRUE));
    cJSON_AddNumberToObject(json_message_object1, "channel_icon_id", (double)icon_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief broadcasts a new-tag-created event to all clients
 *
 * @param uint64 tag_id -> id of the new tag
 * @param char* tag_name -> name of the new tag
 * @param uint64 tag_linked_icon_id -> id of the icon linked to the tag
 * @param boole has_icon -> TRUE when the tag really has an icon, sent as a bool so the client knows
 *                          whether tag_linked_icon_id is meaningful
 *
 * @return void
 */
void server_msg__send_create_new_tag_event_to_all_clients(uint64 tag_id, char* tag_name, uint64 tag_linked_icon_id, boole has_icon)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_create_new_tag_event_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "tag_add");
    cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
    cJSON_AddStringToObject(json_message_object1, "tag_name", tag_name);
    cJSON_AddNumberToObject(json_message_object1, "tag_linked_icon_id", tag_linked_icon_id);
    cJSON_AddItemToObject(json_message_object1, "has_icon", cJSON_CreateBool(has_icon == TRUE));

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_create_new_tag_event_to_all_clients ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        // if statements that are most probable to run should be first in loop
        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_create_new_tag_event_to_all_clients: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief notifies an idle client that another client is calling them
 *
 * @param client_t* caller -> the client placing the call
 * @param client_t* callee -> the idle client being called
 *
 * @return void
 */
void server_msg__send_call_event_to_idle_client(client_t* caller, client_t* callee)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_call_event_to_idle_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "call");
    cJSON_AddNumberToObject(json_message_object1, "caller_client_id", caller->client_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_call_event_to_idle_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, callee->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(callee->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief This function sends client_going_to_idle_mode to all authenticated clients
 *
 * @param uint64 client_that_goes_idle -> id of the client that is going to idle mode
 *
 * @attention
 *
 * @return void
 */
void server_msg__send_client_going_to_idle_mode_info_to_all_clients(uint64 client_that_goes_idle)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_going_to_idle_mode_info_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_going_to_idle_mode");
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)client_that_goes_idle);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_client_going_to_idle_mode_info_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client = &g_clients_array[x];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief This function sends client_coming_back_from_idle_mode to all authenticated clients
 *
 * @param uint64 client_that_comes_from_idle -> id of the client coming back from idle mode
 * @param uint64 channel_the_client_joins -> id of the channel the client joins on return
 *
 * @attention
 *
 * @return void
 */
void server_msg__send_client_coming_back_from_idle_mode_info_to_all_clients(uint64 client_that_comes_from_idle, uint64 channel_the_client_joins)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    uint64 x = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_client_coming_back_from_idle_mode_info_to_all_clients \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "client_coming_back_from_idle_mode");
    cJSON_AddNumberToObject(json_message_object1, "client_id", (double)client_that_comes_from_idle);
    cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)channel_the_client_joins);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_client_coming_back_from_idle_mode_info_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

    for (x = 0; x < g_server_settings.max_client_count; x++)
    {
        client = &g_clients_array[x];

        if (client->is_existing == FALSE)
        {
            continue;
        }

        if (client->is_authenticated == FALSE)
        {
            continue;
        }

        if (client->is_music_bot == TRUE)
        {
            continue;
        }

        size_of_allocated_message_buffer = 0;
        msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

        if (msg_text != NULL_POINTER)
        {
            DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

            ws_sendframe_txt(client->p_ws_connection, msg_text);
            memorymanager__free((nuint)msg_text);
        }
    }

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief sends song list of music bot to single_client
 *
 * @param uint64 sender_client_index -> id of the client to send the song list to
 * @param uint64 music_bot_index -> id of the music bot whose song list is sent
 *
 * @attention this function must be used within rdlock
 *
 * @return void
 */
void server_msg__send_music_bot_song_list_to_single_client(uint64 sender_client_index, uint64 music_bot_index)
{
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    cJSON* json_songs_array = 0;
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    client_t* client = 0;
    music_bot_single_song_data_t* single_song_in_loop = 0;
    client_t* music_bot = NULL_POINTER;
    uint64 loop_index = 0;
    boole status1 = FALSE;
    boole status2 = FALSE;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_music_bot_song_list_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();
    json_songs_array = cJSON_CreateArray();

    // create array of music bot data
    client = &g_clients_array[sender_client_index];

    status1 = util__is_client_valid_admin(sender_client_index);
    status2 = util__is_client_valid_musicbot(music_bot_index);

    if (status1 == FALSE || status2 == FALSE)
    {
        return;
    }

    music_bot = &g_clients_array[music_bot_index];

    // create array of clients
    for (loop_index = 0; loop_index < MUSIC_BOT_MAX_FILE_COUNT; loop_index++)
    {
        cJSON* song = NULL_POINTER;

        single_song_in_loop = &music_bot->music_bot_client_extension.songs[loop_index];

        if (single_song_in_loop->is_existing == FALSE)
        {
            continue;
        }

        song = cJSON_CreateObject();
        cJSON_AddStringToObject(song, "name", single_song_in_loop->song_name);
        cJSON_AddNumberToObject(song, "id", loop_index);
        cJSON_AddNumberToObject(song, "duration_seconds", single_song_in_loop->length_seconds);
        cJSON_AddItemToArray(json_songs_array, song);
    }

    cJSON_AddStringToObject(json_message_object1, "type", "music_bot_song_list");
    cJSON_AddItemToObject(json_message_object1, "songs", json_songs_array);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);
    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "json_root_object1_string: msg_text ", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

    ws_sendframe_txt(client->p_ws_connection, msg_text);
    memorymanager__free((nuint)msg_text);

    base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief tells the client its file upload completed successfully, so it can send instructions on what to do with the file
 *
 * @param client_t* client -> the client whose file upload completed
 * @todo make image file upload use this function
 *
 * @attention this function is used within acquired readlock within client_message.c , multiple functions
 *
 * @return void
 */
void server_msg__send_file_send_completed_status_to_single_client(client_t* client)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_file_send_completed_status_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "file_send_success");
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text == NULL_POINTER)
    {
        return;
    }

    ws_sendframe_txt(client->p_ws_connection, msg_text);

    memorymanager__free((nuint)msg_text);
}

/**
 * @brief sends one chunk of a file transfer as a file_receive_chunk message to the single receiving
 *        client, encrypted with that receiver's shared secret.
 *
 *        the receiver is looked up by index in g_clients_array. the chunk is passed through as the
 *        "value" field exactly as given, together with the sender id and the server chat message id so
 *        the client can append the chunk to the right transfer.
 *
 * @param char* chunk -> the already prepared chunk payload, sent as the "value" field
 * @param uint64 current_size -> size counter of the transfer so far, not written into the message
 * @param uint64 sender_id -> id of the client that sends the file
 * @param uint64 receiver_id -> index into g_clients_array of the client this chunk goes to
 * @param uint64 server_chat_message_id -> id of the chat message this file belongs to
 *
 * @return void
 */
void server_msg__send_file_by_chunk_to_single_client(char* chunk, uint64 current_size, uint64 sender_id, uint64 receiver_id, uint64 server_chat_message_id)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_receiver = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_file_by_chunk_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_receiver = &g_clients_array[receiver_id];

    cJSON_AddStringToObject(json_message_object1, "type", "file_receive_chunk");
    cJSON_AddStringToObject(json_message_object1, "value", chunk);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", sender_id);
    cJSON_AddNumberToObject(json_message_object1, "server_chat_message_id", server_chat_message_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_file_by_chunk_to_single_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_receiver->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        ws_sendframe_txt(client_receiver->p_ws_connection, msg_text);
        memorymanager__free((nuint)msg_text);
    }
}

/**
 * @brief tells one receiving client that a file transfer is finished, as a file_receive_completed
 *        message encrypted with that receiver's shared secret.
 *
 *        sender and receiver are both looked up by index in g_clients_array. the message carries the
 *        receive type, the sender's client id and the server chat message id, so the client can close
 *        the right transfer and render it.
 *
 * @param data_for_file_send_thread_t* info -> the transfer descriptor, read for client_sender_id and
 *                                             server_chat_message_id
 * @param uint64 receiver_id -> index into g_clients_array of the client that gets the notification
 * @param char* receive_type -> kind of transfer, sent as the "receive_type" field
 *
 * @return void
 */
void server_msg__send_file_receive_completed_to_single_client(data_for_file_send_thread_t* info, uint64 receiver_id, char* receive_type)
{
    char* json_root_object1_string = 0;
    int64 size_of_allocated_message_buffer = 0;
    char* msg_text = 0;
    uint64 i = 0;
    cJSON* json_root_object1 = 0;
    cJSON* json_message_object1 = 0;
    client_t* client_receiver = 0;
    client_t* client_sender = 0;

    DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE log_info("%s", "server_msg__send_file_receive_completed_to_single_client \n");

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    client_sender = &g_clients_array[info->client_sender_id];
    client_receiver = &g_clients_array[receiver_id];

    cJSON_AddStringToObject(json_message_object1, "type", "file_receive_completed");
    cJSON_AddStringToObject(json_message_object1, "receive_type", receive_type);
    cJSON_AddNumberToObject(json_message_object1, "sender_id", client_sender->client_id);
    cJSON_AddNumberToObject(json_message_object1, "server_chat_message_id", info->server_chat_message_id);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_file_receive_completed_to_single_client", json_root_object1_string, "\n");

    size_of_allocated_message_buffer = 0;
    msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client_receiver->dh_shared_secret);

    base__free_json_message(json_root_object1, json_root_object1_string);

    if (msg_text != NULL_POINTER)
    {
        ws_sendframe_txt(client_receiver->p_ws_connection, msg_text);
        memorymanager__free((nuint)msg_text);
    }
}