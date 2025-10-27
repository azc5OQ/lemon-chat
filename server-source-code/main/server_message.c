#include <stdio.h>
#include <stdlib.h>

#include "mytypedef.h"
#include "dave-g-json/cJSON.h"
#include "../theldus-websocket/include/ws.h"
#include "base.h"
#include "server_message.h"
#include "log/log.h"
#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "dave-g-json/cJSON.h"
#include "memory_manager.h"
#include "eteran-cvector/cvector.h"

#ifdef WIN32
#include <Windows.h>
#endif

/**
 * @brief gets called by invididuals websocket thread
 *
 * @param client_id_to_send_to index / id of client to send this message to
 * @param random_value_challenge_string randomly generated string to be sent as challenge
 * @param dh_public_mix_for_client dh public mix that client needs to set on his side, result of diffie hellman key exchange
 *
 * @return void
 */
void server_msg__send_public_key_challenge_to_single_client(ws_cli_conn_t *websocket, char *random_value_challenge_string, char *dh_public_mix_for_client)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	DBG_AUTHENTICATION log_info("%s ", "server_msg__send_public_key_challenge_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "public_key_challenge");
	cJSON_AddStringToObject(json_message_object1, "value", random_value_challenge_string);
	cJSON_AddStringToObject(json_message_object1, "dh_public_mix", dh_public_mix_for_client);
	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string, "\n");

	size_of_allocated_message_buffer = 0;
	msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, NULL);

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
 * @param websocket websocket connection of client to send this message to
 * @param ws_connection_dh_shared_secret self exlanatory
 *
 * @return void
 *
 */
void server_msg__send_authentication_status_to_single_client(ws_cli_conn_t *websocket, char *ws_connection_dh_shared_secret)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "authentication_status");
	cJSON_AddStringToObject(json_message_object1, "value", "success");
	cJSON_AddBoolToObject(json_message_object1, "is_voice_chat_active", true);
	cJSON_AddNumberToObject(json_message_object1, "stun_port", 3478);
	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	//DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");

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
 * @brief This function sends channel list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param websocket websocket connection
 * @param ws_connection_dh_shared_secret DH key exchange generated shared secret, that must be used later within this function.
 *
 * @return void
 */
void server_msg__send_channel_list_to_single_client(ws_cli_conn_t *websocket, char *ws_connection_dh_shared_secret)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_channel_array = 0;
	char *json_root_object1_string = 0;
	char *msg_text = 0;
	int size_of_allocated_message_buffer = 0;
	int i;
	cJSON *single_channel = 0;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_channel_list_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();
	json_channel_array = cJSON_CreateArray();
	json_root_object1_string = 0;
	msg_text = 0;
	size_of_allocated_message_buffer = 0;

	for (i = 0; i < MAX_CHANNELS; i++)
	{
		if (channel_array[i].is_existing == FALSE)
		{
			continue;
		}

		single_channel = cJSON_CreateObject();
		cJSON_AddNumberToObject(single_channel, "channel_id", channel_array[i].channel_id);
		cJSON_AddNumberToObject(single_channel, "parent_channel_id", channel_array[i].parent_channel_id);
		cJSON_AddStringToObject(single_channel, "name", channel_array[i].name);
		cJSON_AddStringToObject(single_channel, "description", channel_array[i].description);
		cJSON_AddBoolToObject(single_channel, "is_using_password", channel_array[i].is_using_password);
		cJSON_AddBoolToObject(single_channel, "is_audio_enabled", channel_array[i].is_audio_enabled);
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
 * @param websocket websocket connection
 * @param ws_connection_dh_shared_secret DH key exchange generated shared secret, that must be used later within this function.
 * @param local_clients_username username of local client
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_client_list_to_single_client(ws_cli_conn_t *websocket, char *ws_connection_dh_shared_secret, char *local_clients_username, int client_receiver_id)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_client_array = 0;
	cJSON *json_tag_ids_array = 0;
	client_t *client_in_loop = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	uint64 tag_id_index = 0;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_client_list_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();
	json_client_array = cJSON_CreateArray();

	// create array of clients
	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client_in_loop = &clients_array[x];

		if (client_in_loop->is_existing == FALSE)
		{
			continue;
		}

		if (client_in_loop->is_authenticated == FALSE)
		{
			continue;
		}

		cJSON *single_client = cJSON_CreateObject();
		cJSON_AddStringToObject(single_client, "username", client_in_loop->username);
		cJSON_AddStringToObject(single_client, "public_key", client_in_loop->public_key);

		//
		//check if client in loop is in different channel that receiving client
		//check if client is in password protected channel
		//

		boole is_hide_client_active = FALSE;

		if (g_server_settings.is_hide_clients_in_password_protected_channels_active)
		{
			if (clients_array[client_receiver_id].channel_id != client_in_loop->channel_id)
			{
				if (channel_array[client_in_loop->channel_id].is_using_password == TRUE)
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

		int audio_state_to_send = client_in_loop->audio_state;

		if (client_in_loop->microphone_active)
		{
			if (client_in_loop->audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
			{
				audio_state_to_send = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
			}
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

		//
		//property country_iso_code will always be part of response
		//the value will be empty if ip address is from unknown country or if server doesnt display flags
		//

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

	if (msg_text == NULL_POINTER)
	{
		return;
	}

	DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

	ws_sendframe_txt(websocket, msg_text);

	memorymanager__free((nuint)msg_text);

	DBG_SERVER_MESSAGE log_info("%s", "got here \n");
}

/**
 * @brief This function sends icon list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param websocket websocket connection
 * @param ws_connection_dh_shared_secret DH key exchange generated shared secret, that must be used later within this function.
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_icon_list_to_single_client(ws_cli_conn_t *websocket, char *ws_connection_dh_shared_secret)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_icons_array = 0;
	client_t *client_in_loop = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_icon_list_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();
	json_icons_array = cJSON_CreateArray();

	// create array of clients
	for (x = 0; x < MAX_ICONS; x++)
	{
		if (icons_array[x].is_existing == FALSE)
		{
			continue;
		}

		cJSON *single_client = cJSON_CreateObject();
		cJSON_AddNumberToObject(single_client, "icon_id", (double)icons_array[x].id);
		cJSON_AddStringToObject(single_client, "base64_icon", icons_array[x].base64);
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
 * @param websocket websocket connection
 * @param ws_connection_dh_shared_secret DH key exchange generated shared secret, that must be used later within this function.
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_tag_list_to_single_client(ws_cli_conn_t *websocket, char *ws_connection_dh_shared_secret)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_tags_array = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();
	json_tags_array = cJSON_CreateArray();

	// create array of clients

	for (x = 0; x < MAX_ICONS; x++)
	{
		if (tags_array[x].is_existing == FALSE)
		{
			continue;
		}

		cJSON *single_tag_id_object = cJSON_CreateObject();
		cJSON_AddNumberToObject(single_tag_id_object, "tag_id", (double)tags_array->id);
		cJSON_AddStringToObject(single_tag_id_object, "tag_name", tags_array[x].name);
		cJSON_AddNumberToObject(single_tag_id_object, "tag_linked_icon_id", (double)tags_array[x].icon_id);
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
 * @param websocket websocket connection
 * @param ws_connection_dh_shared_secret DH key exchange generated shared secret, that must be used later within this function.
 * @param current_channel_id current channel id
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_active_microphone_usage_for_current_channel_to_single_client(ws_cli_conn_t *websocket, char *ws_connection_dh_shared_secret, int current_channel_id)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_clients_array = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client_in_loop = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();
	json_clients_array = cJSON_CreateArray();

	// create array of clients
	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client_in_loop = &clients_array[x];
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

		//have to send even to ourselves
		//if client.client_id == current_client_id {
		//    continue;
		//}

		//only active mics are relevant since this is "active microphone usage"
		//dough

		if (client_in_loop->audio_state != AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
		{
			continue;
		}

		cJSON *single_object = cJSON_CreateObject();
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

	if (msg_text == NULL_POINTER)
	{
		return;
	}

	DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

	ws_sendframe_txt(websocket, msg_text);

	memorymanager__free((nuint)msg_text);
}

/**
 * @brief This function sends tag list to single client. Called when client sents challenge_response message to server during authentication process.
 *
 * @param client_id_of_connected_client self explanatory
 *
 * @attention it doesnt need readlock, already called within writelock in client_message.c
 *
 * @return void
 */
void server_msg__send_client_connect_message_to_all_clients(int client_id_of_connected_client)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_tag_ids_array = 0;
	client_t *local_client = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();
	json_tag_ids_array = cJSON_CreateArray();

	local_client = &clients_array[client_id_of_connected_client];

	cJSON_AddStringToObject(json_message_object1, "type", "client_connect");
	cJSON_AddStringToObject(json_message_object1, "username", local_client->username);
	cJSON_AddStringToObject(json_message_object1, "public_key", local_client->public_key);
	cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)local_client->channel_id);
	cJSON_AddNumberToObject(json_message_object1, "client_id", (double)local_client->client_id);
	cJSON_AddStringToObject(json_message_object1, "country_iso_code", local_client->country_iso_code);

	cJSON_AddItemToObject(json_message_object1, "tag_ids", json_tag_ids_array);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_tag_list_to_single_client: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		if (client->client_id == client_id_of_connected_client)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief This function sends maintainer of current channel where client just joined. Used when client joins the server or when he swiches channel
 *
 * @param client_t* client to send maintainer id to
 * @param int channel id. for some reason needed
 * @param int id/index of maintainer.
 *
 * @attention sometimes parameter id_of_client_that_joined_the_channel can have the value as id_of_client_that_is_maintainer_of_channel
 *
 * @return void
 */
void server_msg__send_maintainer_id_to_single_client(client_t *client, int channel_id, int id_of_client_that_is_maintainer_of_channel)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "channel_maintainer_id");
	cJSON_AddNumberToObject(json_message_object1, "maintainer_id", (double)id_of_client_that_is_maintainer_of_channel);
	cJSON_AddNumberToObject(json_message_object1, "channel_id", (double)channel_id);
	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	//DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");

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
 * @brief This function sends connection check response to client
 *
 * @param client self explanatory
 *
 * @attention thanks for your attention
 *
 * @return void
 */
void server_msg__send_connection_check_response_to_single_client(client_t *client)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "connection_check_response");
	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	//DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");

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
 * @brief This function sends client_rename_message to all authenticated clients
 *
 * @param id_of_client_that_changed_his_username self explanatory
 * @param new_username self explanatory
 *
 * @attention this function is used within acquired readlock within client_message.c client_msg__process_change_client_username function
 *
 * @return void
 */
void server_msg__send_client_rename_message_to_all_clients(int id_of_client_that_changed_his_username, char *new_username)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "client_rename");
	cJSON_AddStringToObject(json_message_object1, "new_username", new_username);
	cJSON_AddNumberToObject(json_message_object1, "client_id", (double)id_of_client_that_changed_his_username);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_client_rename_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client self explanatory
 *
 * @attention this function is used within acquired readlock within client_message.c , multiple functions
 *
 * @return void
 */
void server_msg__send_access_denied_to_single_client(client_t *client)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

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
 * @brief self explanatory
 *
 * @param created_channel_index self explanatory
 * @param channel_creator_client_index self explanatory
 *
 * @attention this function is called within two acquired read locks, clients_global_rwlock_guard and channels_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_channel_create_message_to_all_clients(int created_channel_index, int channel_creator_client_index)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;
	channel_t *channel = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	channel = &channel_array[created_channel_index];

	cJSON_AddStringToObject(json_message_object1, "type", "channel_create");
	cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
	cJSON_AddNumberToObject(json_message_object1, "parent_channel_id", channel->parent_channel_id);
	cJSON_AddStringToObject(json_message_object1, "name", channel->name);
	cJSON_AddStringToObject(json_message_object1, "description", channel->description);
	cJSON_AddNumberToObject(json_message_object1, "maintainer_id", channel->maintainer_id);
	cJSON_AddBoolToObject(json_message_object1, "is_using_password", channel->is_using_password);
	cJSON_AddBoolToObject(json_message_object1, "is_audio_enabled", channel->is_audio_enabled);
	cJSON_AddNumberToObject(json_message_object1, "channel_creator_id", channel_creator_client_index);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_create_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param edited_channel_index self explanatory
 *
 * @attention this function is called within two acquired read locks, clients_global_rwlock_guard and channels_global_rwlock_guard
 *
 * @return void
 */
void server_msg__send_channel_edit_message_to_all_clients(int edited_channel_index, int channel_editor_id)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;
	channel_t *channel = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	channel = &channel_array[edited_channel_index];

	cJSON_AddStringToObject(json_message_object1, "type", "channel_edit");
	cJSON_AddNumberToObject(json_message_object1, "channel_id", channel->channel_id);
	cJSON_AddStringToObject(json_message_object1, "channel_name", channel->name);
	cJSON_AddStringToObject(json_message_object1, "channel_description", channel->description);
	cJSON_AddBoolToObject(json_message_object1, "is_using_password", (cJSON_bool)channel->is_using_password);
	cJSON_AddBoolToObject(json_message_object1, "is_audio_enabled", (cJSON_bool)channel->is_audio_enabled);
	cJSON_AddNumberToObject(json_message_object1, "channel_editor_id", channel_editor_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_edit_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief metadata are sent, so that client knows image is being sent to him.
 *
 * @param client_index self explanatory
 * @param chat_message_id id of server message
 * @param local_message_id id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client(int client_index, int chat_message_id, int local_message_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_server_chat_message_id_for_local_chat_message_id_to_single_client \n");

	client = &clients_array[client_index];

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
 * @param sender_id self explanatory
 * @param msg_receiver_id id of server message
 * @param chat_message_id id of local message
 * @param chat_message_value id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_message_to_single_client(int client_sender_id, int client_receiver_id, int server_chat_message_id, char *chat_message_value)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_receiver;
	client_t *client_sender;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_chat_message_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	client_sender = &clients_array[client_sender_id];
	client_receiver = &clients_array[client_receiver_id];

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
 * @param client_sender_id self explanatory
 * @param receiving_channel_id self explanatory
 * @param server_chat_message_id
 * @param chat_message_value self explanatory
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_message_to_clients_in_same_channel(int client_sender_id, int receiving_channel_id, int server_chat_message_id, char *chat_message_value)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_chat_message_to_clients_in_same_channel \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	client_sender = &clients_array[client_sender_id];

	cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_message");
	cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
	cJSON_AddStringToObject(json_message_object1, "sender_username", client_sender->username);
	cJSON_AddNumberToObject(json_message_object1, "channel_id", receiving_channel_id);
	cJSON_AddNumberToObject(json_message_object1, "server_chat_message_id", server_chat_message_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_message_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		if (client->channel_id != receiving_channel_id)
		{
			continue;
		}

		if (client->client_id == client_sender_id)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client_sender_id self explanatory
 * @param receiving_channel_id self explanatory
 * @param server_chat_message_id
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel(int client_sender_id, int receiving_channel_id, int server_chat_message_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel \n");

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

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		if (client->channel_id != receiving_channel_id)
		{
			DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel :client->channel_id != receiving_channel_id \n");
			DBG_SERVER_MESSAGE log_info("%s %d %s", "client->channel_id", client->channel_id, "\n");
			DBG_SERVER_MESSAGE log_info("%s %d %s", "receiving_channel_id", receiving_channel_id, "\n");

			continue;
		}

		if (client->client_id == client_sender_id)
		{
			continue;
		}

		DBG_SERVER_MESSAGE log_info("%s %d %s", "server_msg__send_channel_chat_picture_metadata_to_clients_in_same_channel: sending to client ", client->client_id, "\n");

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief send chat message itself to all clients in same channel
 *
 * @param client_sender_id self explanatory
 * @param receiving_channel_id self explanatory
 * @param server_chat_message_id
 * @param chat_message_value self explanatory
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_channel_chat_picture_to_clients_in_same_channel(int client_sender_id, int receiving_channel_id, int server_chat_message_id, char *chat_message_value)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_chat_message_to_clients_in_same_channel \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	client_sender = &clients_array[client_sender_id];

	cJSON_AddStringToObject(json_message_object1, "type", "channel_chat_picture");
	cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
	cJSON_AddStringToObject(json_message_object1, "username", client_sender->username);
	cJSON_AddNumberToObject(json_message_object1, "channel_id", receiving_channel_id);
	cJSON_AddNumberToObject(json_message_object1, "picture_id", server_chat_message_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_chat_message_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		if (client->channel_id != receiving_channel_id)
		{
			continue;
		}

		if (client->client_id == client_sender_id)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief This function sends image status to single client so client knows that server received and sent his message to other clients
 *
 * @param client self explanatory
 *
 * @attention thanks for your attention
 *
 * @return void
 */
void server_msg__send_image_status_to_single_client(client_t *client, char *status)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "image_sent_status");
	cJSON_AddStringToObject(json_message_object1, "value", status);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	//DBG_AUTHENTICATION log_info("%s %s %s", "json_root_object1_string ", json_root_object1_string , "\n");

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
 * @param sender_id self explanatory
 * @param msg_receiver_id id of server message
 * @param chat_message_id id of local message
 * @param chat_message_value id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_picture_to_single_client(int client_sender_id, int client_receiver_id, int server_chat_message_id, char *chat_message_value)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_receiver;
	client_t *client_sender;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_chat_picture_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	client_sender = &clients_array[client_sender_id];
	client_receiver = &clients_array[client_receiver_id];

	cJSON_AddStringToObject(json_message_object1, "type", "direct_chat_picture");
	cJSON_AddStringToObject(json_message_object1, "value", chat_message_value);
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
 * @brief send chat message itself to client
 *
 * @param client_sender_id self explanatory
 * @param receiving_channel_id id of server message
 * @param server_chat_message_id id of local message
 *
 * @attention this function is used within read lock on clinets array
 *
 * @return void
 */
void server_msg__send_chat_picture_metadata_to_single_client(int client_sender_id, int client_receiver_id, int server_chat_message_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_receiver;
	client_t *client_sender;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_chat_picture_metadata_to_single_client \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	client_sender = &clients_array[client_sender_id];
	client_receiver = &clients_array[client_receiver_id];

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
 * @param client_that_switched_channel self explanatory
 * @param new_channel id of server message
 *
 * @attention
 * this function gets called AFTER new channel id is assigned to client struct, not before
 * so this function must assume client is already in his new channel, if it is going to do any logical operations based on that
 *
 * @return void
 */
void server_msg__send_channel_join_message_to_all_clients(client_t *client_that_switched_channel, channel_t *new_channel)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	cJSON *json_root_object1_client_hidden_type = 0;
	cJSON *json_message_object1_client_hidden_type = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	char *json_root_object1_string_client_hidden_type = 0;

	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;
	char *song_name = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	json_root_object1_client_hidden_type = cJSON_CreateObject();
	json_message_object1_client_hidden_type = cJSON_CreateObject();

	//
	//clients not in same channel will not receive real time microphone usage information from the client that switched the channel
	//

	boole is_streaming_song = client_that_switched_channel->is_streaming_song;
	int audio_state = client_that_switched_channel->audio_state;
	song_name = client_that_switched_channel->song_name;

	if (client_that_switched_channel->channel_id != new_channel->channel_id)
	{
		if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
		{
			audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
		}
		is_streaming_song = false;
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

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		boole is_hide_client_active = FALSE;

		if (g_server_settings.is_hide_clients_in_password_protected_channels_active)
		{
			if (client->channel_id != new_channel->channel_id)
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
			msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string_client_hidden_type, &size_of_allocated_message_buffer, client->dh_shared_secret);

			if (msg_text == NULL_POINTER)
			{
				goto label_server_msg__send_channel_join_message_to_all_clients_end;
			}
		}
		else
		{
			size_of_allocated_message_buffer = 0;
			msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

			if (msg_text == NULL_POINTER)
			{
				goto label_server_msg__send_channel_join_message_to_all_clients_end;
			}
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

label_server_msg__send_channel_join_message_to_all_clients_end:

	base__free_json_message(json_root_object1, json_root_object1_string);
	base__free_json_message(json_root_object1_client_hidden_type, json_root_object1_string_client_hidden_type);
}

/**
 * @brief sends channel join message to single_client
 *
 * @param client_that_switched_channel self explanatory
 * @param new_channel id of server message
 *
 * @attention
 * t
 *
 * @return void
 */
void server_msg__send_channel_join_message_to_single_client(client_t *client_that_switched_channel, channel_t *new_channel, client_t *receiving_client)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;

	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	char *song_name = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	//
	//clients not in same channel will not receive real time microphone usage information from the client that switched the channel
	//

	boole is_streaming_song = client_that_switched_channel->is_streaming_song;
	int audio_state = client_that_switched_channel->audio_state;
	song_name = client_that_switched_channel->song_name;

	if (client_that_switched_channel->channel_id != new_channel->channel_id)
	{
		if (audio_state == AUDIO_STATE__PUSH_TO_TALK_ACTIVE)
		{
			audio_state = AUDIO_STATE__PUSH_TO_TALK_ENABLED;
		}
		is_streaming_song = false;
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
 * @brief self explanatory
 *
 * @param channel_id self explanatory
 * @param maintainer_id id of server message
 *
 * @attention used within write lock for clients and channels
 *
 * @return void
 */
void server_msg__send_maintainer_id_to_clients_in_same_channel(int channel_id, int maintainer_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_maintainer_id_to_clients_in_same_channel \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "channel_maintainer_id");
	cJSON_AddNumberToObject(json_message_object1, "channel_id", channel_id);
	cJSON_AddNumberToObject(json_message_object1, "maintainer_id", maintainer_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_maintainer_id_to_clients_in_same_channel: json_root_object1_string ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

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

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param int deleted_channel_id
 * @param int channel_deletor_id
 *
 * @attention used within write lock for clients and channels
 *
 * @return void
 */
void server_msg__send_channel_delete_message_to_all_clients(int deleted_channel_id, int channel_deletor_id)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;
	channel_t *channel = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "channel_delete");
	cJSON_AddNumberToObject(json_message_object1, "channel_id", deleted_channel_id);
	cJSON_AddNumberToObject(json_message_object1, "channel_deletor_id", channel_deletor_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_channel_delete_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param int client_index
 *
 * @attention used within acquired write lock for clients_array
 *
 * @return void
 */
void server_msg__send_client_disconnect_message_to_all_clients(int client_index)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "client_disconnect");
	cJSON_AddNumberToObject(json_message_object1, "client_id", client_index);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_client_disconnect_message_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

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

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client_t* client
 * @param int sender_index
 * @param char* message
 *
 * @return void
 */
void server_msg__send_poke_to_single_client(client_t *client, int sender_index, char *poke_message)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_chat_picture_metadata_to_single_client \n");

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
 * @brief self explanatory
 *
 * @param client_t* client
 * @param char* sdp_offer
 *
 * @return void
 */
void server_msg__send_webrtc_sdp_offer_to_single_client(const char *candidate, const char *mid, client_t *client)
{
	cJSON *json_root_object = NULL;
	cJSON *json_message_object = NULL;
	cJSON *json_message_value = NULL;
	cJSON *jmid = NULL;
	cJSON *jsoncandidate = NULL;
	cJSON *jsonlineindex = NULL;
	char *json_root_object_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;

	json_root_object = cJSON_CreateObject();
	json_message_object = cJSON_CreateObject();
	json_message_value = cJSON_CreateObject();

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

	DBG_AUDIOCHANNEL_WEBRTC log_info("peerconnection_oncandidate_callback CANDIDATE SEND \n");
	base__free_json_message(json_root_object, json_root_object_string);

	memorymanager__free((nuint)msg_text);
}

/**
 * @brief self explanatory
 *
 * @param int client_id
 * @param int state
 *
 * @return void
 */
void server_msg__send_audio_state_of_client_to_all_clients(int client_id, int state)
{
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	int x = 0;
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	client_t *client = 0;

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "audio_state_of_single_client");
	cJSON_AddNumberToObject(json_message_object1, "client_id", client_id);
	cJSON_AddNumberToObject(json_message_object1, "value", state);
	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_audio_state_of_client_to_all_clients: json_root_object1_string ", json_root_object1_string, "\n");

	for (x = 0; x < MAX_CLIENTS; x++)
	{
		client = &clients_array[x];

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		boole is_hide_client_active = FALSE;

		if (g_server_settings.is_hide_clients_in_password_protected_channels_active)
		{
			if (client->channel_id != clients_array[client_id].channel_id)
			{
				//if channel of receiving client and client that is sending audio state, isnt same
				//and client that is sending audio state is located in password protected channel
				//skip client
				if (channel_array[clients_array[client_id].channel_id].is_using_password == TRUE)
				{
					is_hide_client_active = TRUE;
				}
			}
		}

		if (is_hide_client_active == TRUE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %d %s %d %s", "server_msg__send_audio_state_of_client_to_all_clients ", client_id, " to client ", client->client_id, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);

		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client_t *client
 *
 * @return void
 */
void server_msg__send_start_song_stream_message_to_clients_in_same_channel(client_t *client_that_streams)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_start_song_stream_message_to_clients_in_same_channel \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "start_song_stream");
	cJSON_AddNumberToObject(json_message_object1, "client_id", client_that_streams->client_id);
	cJSON_AddStringToObject(json_message_object1, "song_name", client_that_streams->song_name);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_start_song_stream_message_to_clients_in_same_channel ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

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

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "send_client_list_to_client: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client_t *client
 *
 * @return void
 */
void server_msg__send_stop_song_stream_message_to_clients_in_same_channel(client_t *client_that_streams)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_stop_song_stream_message_to_clients_in_same_channel \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "stop_song_stream");
	cJSON_AddNumberToObject(json_message_object1, "client_id", client_that_streams->client_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_stop_song_stream_message_to_clients_in_same_channel ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

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

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_stop_song_stream_message_to_clients_in_same_channel: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client_id_of_client_that_got_the_new_tag int
 * @param tag_id int
 *
 * @return void
 */
void server_msg__send_add_tag_to_client_event_to_all_clients(int client_id_of_client_that_got_the_new_tag, int tag_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_add_tag_to_client_event_to_all_clients \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "tag_add_to_client");
	cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
	cJSON_AddNumberToObject(json_message_object1, "client_id", client_id_of_client_that_got_the_new_tag);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_add_tag_to_client_event_to_all_clients ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_add_tag_to_client_event_to_all_clients: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param client_id_of_client_that_got_tag_removed int
 * @param tag_id int
 *
 * @return void
 */
void server_msg__send_remove_tag_from_client_event_to_all_clients(int client_id_of_client_that_got_tag_removed, int tag_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client_sender;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_remove_tag_from_client_event_to_all_clients \n");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "remove_tag_from_client");
	cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
	cJSON_AddNumberToObject(json_message_object1, "client_id", client_id_of_client_that_got_tag_removed);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_remove_tag_from_client_event_to_all_clients ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_remove_tag_from_client_event_to_all_clients: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param new_icon_id int
 * @param icon_base64_value char*
 *
 * @return void
 */

void server_msg__send_add_new_icon_event_to_all_clients(int new_icon_id, char *icon_base64_value)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_add_new_icon_event_to_all_clients");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "icon_add");
	cJSON_AddNumberToObject(json_message_object1, "icon_id", new_icon_id);
	cJSON_AddStringToObject(json_message_object1, "base64_icon", icon_base64_value);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_add_new_icon_event_to_all_clients ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_remove_tag_from_client_event_to_all_clients: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}

/**
 * @brief self explanatory
 *
 * @param tag_id int
 * @param tag_name  char*
 * @param tag_linked_icon_id int
 *
 * @return void
 */

void server_msg__send_create_new_tag_event_to_all_clients(int tag_id, char *tag_name, int tag_linked_icon_id)
{
	char *json_root_object1_string = 0;
	int size_of_allocated_message_buffer = 0;
	char *msg_text = 0;
	int i = 0;
	cJSON *json_root_object1 = 0;
	cJSON *json_message_object1 = 0;
	client_t *client;

	DBG_SERVER_MESSAGE log_info("%s", "server_msg__send_create_new_tag_event_to_all_clients");

	json_root_object1 = cJSON_CreateObject();
	json_message_object1 = cJSON_CreateObject();

	cJSON_AddStringToObject(json_message_object1, "type", "tag_add");
	cJSON_AddNumberToObject(json_message_object1, "tag_id", tag_id);
	cJSON_AddStringToObject(json_message_object1, "tag_name", tag_name);
	cJSON_AddNumberToObject(json_message_object1, "tag_linked_icon_id", tag_linked_icon_id);

	cJSON_AddItemToObject(json_root_object1, "message", json_message_object1);

	json_root_object1_string = cJSON_PrintUnformatted(json_root_object1);

	DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_create_new_tag_event_to_all_clients ", json_root_object1_string, "\n");

	for (i = 0; i < MAX_CLIENTS; i++)
	{
		client = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client->is_existing == FALSE)
		{
			continue;
		}

		if (client->is_authenticated == FALSE)
		{
			continue;
		}

		size_of_allocated_message_buffer = 0;
		msg_text = base__encrypt_cstring_and_convert_to_base64(json_root_object1_string, &size_of_allocated_message_buffer, client->dh_shared_secret);

		if (msg_text == NULL_POINTER)
		{
			return;
		}

		DBG_SERVER_MESSAGE log_info("%s %s %s", "server_msg__send_create_new_tag_event_to_all_clients: msg_text ", msg_text, "\n");

		ws_sendframe_txt(client->p_ws_connection, msg_text);
		memorymanager__free((nuint)msg_text);
	}

	base__free_json_message(json_root_object1, json_root_object1_string);
}