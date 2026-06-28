
#include "definitions.h"

#ifdef WIN32
#include <Windows.h>
#endif

#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "server_message.h"
#include "memory_manager.h"
#include "../third-party/eteran-cvector/cvector.h"
#include "../third-party/rxi-log/log.h"

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
    cJSON_AddNumberToObject(json_message_object1, "stun_port", 3478);
    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    /* DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n"); */
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

    /* create array of clients */
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

        /* check if client in loop is in different channel that receiving client
           check if client is in password protected channel */
        is_hide_client_active = FALSE;

        if (g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)
        {
            if (g_clients_array[client_receiver_id].channel_id != client_in_loop->channel_id)
            {
                if (g_channel_array[client_in_loop->channel_id].is_using_password == TRUE)
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

        /* only send "PUSH_TO_TALK_ACTIVE" state to connected client for other clients in same channel as he is (root channel in this case)
           privacy reasons */
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

        /* property country_iso_code will always be part of response
           the value will be empty if ip address is from unknown country or if server doesn't display flags */
        cJSON_AddStringToObject(single_client, "country_iso_code", client_in_loop->country_iso_code);

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

    /* create array of clients */
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

    /* create array of clients */
    for (x = 0; x < MAX_TAGS; x++)
    {
        cJSON* single_tag_id_object = NULL_POINTER;

        if (g_tags_array[x].is_existing == FALSE)
        {
            continue;
        }

        single_tag_id_object = cJSON_CreateObject();
        cJSON_AddNumberToObject(single_tag_id_object, "tag_id", (double)g_tags_array->id);
        cJSON_AddStringToObject(single_tag_id_object, "tag_name", g_tags_array[x].name);
        cJSON_AddNumberToObject(single_tag_id_object, "tag_linked_icon_id", (double)g_tags_array[x].icon_id);
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

    /* create array of clients */
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

        /* have to send even to ourselves
           if client.client_id == current_client_id {
               continue;
           } */

        /* only active mics are relevant since this is "active microphone usage" */
        /* though */
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
        json_tag_ids_array = cJSON_CreateArray();

        cJSON_AddStringToObject(json_message_object1, "type", "client_connect");
        cJSON_AddStringToObject(json_message_object1, "username", new_client->username);
        cJSON_AddStringToObject(json_message_object1, "public_key", new_client->public_key);
        cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)new_client->channel_id);
        cJSON_AddNumberToObject(json_message_object1, "client_id", (double)new_client->client_id);
        cJSON_AddBoolToObject(json_message_object1, "is_music_bot", new_client->is_music_bot);

        audio_state_to_send = new_client->audio_state;

        /* only send "PUSH_TO_TALK_ACTIVE" state to connected client for other clients in same channel as he is (root channel in this case)
           privacy reasons */
        if (new_client->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE && client_in_loop->channel_id != new_client->channel_id)
        {
            audio_state_to_send = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
        }

        cJSON_AddNumberToObject(json_message_object1, "audio_state", audio_state_to_send);
        cJSON_AddStringToObject(json_message_object1, "country_iso_code", new_client->country_iso_code);
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

    /* DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n"); */
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

    /* DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n"); */
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
 * @brief sends an access-denied message to a single client
 *
 * @param client_t* client -> self explanatory
 *
 * @attention this function is used within acquired readlock within client_message.c , multiple functions
 *
 * @return void
 */
void server_msg__send_access_denied_to_single_client(client_t* client)
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
    ban_entry_t* ban_in_loop = 0;
    uint64 i = 0;

    json_root_object1 = cJSON_CreateObject();
    json_message_object1 = cJSON_CreateObject();

    cJSON_AddStringToObject(json_message_object1, "type", "server_settings_values");
    cJSON_AddItemToObject(json_message_object1, "display_country_flags", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "enable_audio", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "enable_music_bot_audio", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "hide_clients_in_password_channels", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(json_message_object1, "allow_temp_channels", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));

    /* include the current ban list so the admin's bans section can render it */
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

        /* if statements that are most probable to run should be first in loop */
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
void server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(uint64 client_sender_id, uint64 receiving_channel_id, uint64 server_chat_message_id)
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
    cJSON_AddNumberToObject(json_message_object1, "size", 10000000);

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_in_loop = &g_clients_array[i];

        /* if statements that are most probable to run should be first in loop */
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

        /* if statements that are most probable to run should be first in loop */
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

    /* DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n"); */
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
void server_msg__send_chat_picture_metadata_to_single_client(uint64 client_sender_id, uint64 client_receiver_id, uint64 server_chat_message_id)
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

    /* clients not in same channel will not receive real time microphone usage information from the client that switched the channel */
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
 * @brief sends channel join message to single_client
 *
 * @param client_t* client_that_switched_channel -> self explanatory
 * @param channel_t* new_channel -> id of server message
 *
 * @attention
 * t
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

    /* clients not in same channel will not receive real time microphone usage information from the client that switched the channel */
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

        /* if statements that are most probable to run should be first in loop */
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
                /* if channel of receiving client and client that is sending audio state, isn't same
                   and client that is sending audio state is located in password protected channel
                   skip client */
                if (g_channel_array[g_clients_array[client_whose_audio_to_send].channel_id].is_using_password == TRUE)
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

        /* only send microphone active state to clients in same channel as sender */
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

        /* if statements that are most probable to run should be first in loop */
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

        /* if statements that are most probable to run should be first in loop */
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

        /* if statements that are most probable to run should be first in loop */
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

        /* if statements that are most probable to run should be first in loop */
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

        /* if statements that are most probable to run should be first in loop */
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
 * @brief broadcasts a new-tag-created event to all clients
 *
 * @param uint64 tag_id -> id of the new tag
 * @param char* tag_name -> name of the new tag
 * @param uint64 tag_linked_icon_id -> id of the icon linked to the tag
 *
 * @return void
 */
void server_msg__send_create_new_tag_event_to_all_clients(uint64 tag_id, char* tag_name, uint64 tag_linked_icon_id)
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

    cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

    json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

    DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_create_new_tag_event_to_all_clients ", json_root_object1_string, "\n");

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client = &g_clients_array[i];

        /* if statements that are most probable to run should be first in loop */
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

    /* create array of music bot data */
    client = &g_clients_array[sender_client_index];

    status1 = util__is_client_valid_admin(sender_client_index);
    status2 = util__is_client_valid_musicbot(music_bot_index);

    if (status1 == FALSE || status2 == FALSE)
    {
        return;
    }

    music_bot = &g_clients_array[music_bot_index];

    /* create array of clients */
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