#include "definitions.h"

// use forward slashes "/" when specifying paths, not backward slashes "\" linux enviroment has trouble finding files that way
// windows compiler will work with both

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" //needed by base.h
#include "base.h"

#include "../third-party/ITH-sha/sha256.h"

#include "../third-party/libviolet-0.5.4/src/options.h"
#include "../third-party/libviolet-0.5.4/src/utils.h"

#include "../third-party/rxi-log/log.h"

#include "memory_manager.h"
#include "audio_channel.h"

uint64 thread_id0 = 0;
uint64 thread_id1 = 0;
uint64 thread_id2 = 0;
uint64 thread_id3 = 0;

boole is_server_running = TRUE;

static void _main_internal__init_channel_list(void);
static void _main_internal__init_tags_and_icons(void);
static void _main_internal__set_server_settings(void);
static int _main_internal__get_client_index_by_ws_client_pointer(ws_cli_conn_t *p_ws_connection);
static void _main_internal__print_debug_information(void);
static void _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void);

//

/**
 * @brief see title
 *
 * @param ws_cli_conn_t* p_ws_connection
 *
 * @return int client count
 *
 */
int _main_internal__get_client_index_by_ws_client_pointer(ws_cli_conn_t *p_ws_connection)
{
	int i;
	int result = -1;

	if (p_ws_connection == NULL_POINTER)
	{
		return result;
	}

	clib__read_lock(&clients_global_rwlock_guard);

	for (i = 0; i < g_server_settings.max_client_count; i++)
	{
		if (clients_array[i].p_ws_connection == p_ws_connection)
		{
			result = i;
			break;
		}
	}

	clib__unlock(&clients_global_rwlock_guard);

	return result;
}

/**
 * @brief gets called by invididuals websocket thread
 *
 * @param client websocket client structure
 *
 * @return void
 *
 * @attention onopen is called by different thread everytime
 */
void onopen(ws_cli_conn_t *client)
{
	char *ip_address = NULL_POINTER;
	boole ip_address_already_in_use = FALSE;

	/*
     * mutex is needed,, in case onopen is called too fast (each onopen is called by different thread)
     * (each onopen is ran within its own thread)
     * */

	clib__write_lock(&clients_global_rwlock_guard);

	g_server_settings.client_count = g_server_settings.client_count + 1;

	DBG_AUTHENTICATION log_info("%s %p %s", "client connected , ", client, "\n");

	int index = base__get_new_index_for_client();

	if ((g_server_settings.client_count + 1) >= g_server_settings.max_client_count)
	{
		DBG_AUTHENTICATION log_info("%s", "max client reached. Closing connection with client");

		ws_close_client(client);
		goto label_onopen_end;
	}

	if (index == -1)
	{
		DBG_AUTHENTICATION log_info("base__get_new_index_for_client returned -1, closing socket");

		ws_close_client(client);
		goto label_onopen_end;
	}

	ip_address = ws_getaddress(client);
	if (ip_address == NULL_POINTER)
	{
		DBG_AUTHENTICATION log_info("failed to get ip address of a client");
		ws_close_client(client);
		goto label_onopen_end;
	}

	if (g_server_settings.is_same_ip_address_allowed == FALSE)
	{
		ip_address_already_in_use = base__is_there_a_client_with_same_ip_address(ip_address);

		if (ip_address_already_in_use)
		{
			DBG_AUTHENTICATION log_info("ip address already in use, closing socket");
			ws_close_client(client);
			goto label_onopen_end;
		}
	}

	DBG_AUTHENTICATION log_info("%s%d", "[i] onopen : new client id: ", index);

	clients_array[index].is_authenticated = FALSE;
	clients_array[index].timestamp_connected = base__get_timestamp_ms();
	clients_array[index].p_ws_connection = client;
	clients_array[index].is_existing = TRUE;
	clients_array[index].client_id = index;
	clients_array[index].audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
	clib__copy_memory(ip_address, clients_array[index].ip_address, clib__utf8_string_length(ip_address), 45);

label_onopen_end:
	clib__unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param ws_cli_conn_t* websocket
 *
 * @return void
 * */
void onclose(ws_cli_conn_t *websocket)
{
	int i;
	int client_index = -1;

	clib__write_lock(&clients_global_rwlock_guard);
	clib__write_lock(&channels_global_rwlock_guard);

	g_server_settings.client_count = g_server_settings.client_count - 1;

	for (i = 0; i < g_server_settings.max_client_count; i++)
	{
		if (clients_array[i].p_ws_connection == websocket)
		{
			client_index = i;
			break;
		}
	}

	DBG_AUTHENTICATION log_info("%s %d %s", "onclose", client_index, "\n");

	if (client_index == -1)
	{
		goto label_onclose_end;
	}

	base__process_client_disconnect(client_index);

label_onclose_end:
	clib__unlock(&channels_global_rwlock_guard);
	clib__unlock(&clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param ws_cli_conn_t* websocket
 * @param const unsigned char* base64_to_process_and_decrypt
 * @param uint64_t size
 * @param int type
 *
 * @return void
 * */
void onmessage(ws_cli_conn_t *websocket, unsigned char *base64_to_process_and_decrypt, uint64_t size, int type)
{
	boole is_authenticated = FALSE;
	boole is_existing = FALSE;
	int client_index = 0;
	char *decrypted_metadata_cstring = 0;

	//will this affect negatively
	client_index = _main_internal__get_client_index_by_ws_client_pointer(websocket);
	if (client_index == -1)
	{
		return;
	}

	DBG_ONMESSAGE log_info("%s %d %s", "onmessage() : ", client_index, "\n");

	DBG_ONMESSAGE log_info("%s %llu %s", "onmessage received websocket data size is : ", size, "\n");

	if (size > g_server_settings.websocket_message_max_length)
	{
		base__close_websocket_connection(client_index, TRUE);
		//ws_close_client(websocket);
		return;
	}

	if (size == 0)
	{
		base__close_websocket_connection(client_index, TRUE);
		// ws_close_client(websocket);
		return;
	}

	decrypted_metadata_cstring = (char *)(unsigned char *)memorymanager__allocate(size, MEMALLOC_TYPE_DECRYPT);

	if (decrypted_metadata_cstring == NULL_POINTER)
	{
		DBG_ONMESSAGE log_info("%s %d %s", "onmessage decrypted_metadata_cstring is NULL", client_index, "\n");
		return;
	}

	/* just a simple readlock, nothing expensive */

	clib__read_lock(&clients_global_rwlock_guard);

	is_authenticated = clients_array[client_index].is_authenticated;
	is_existing = clients_array[client_index].is_existing;
	base__get_data_from_base64_and_decrypt_it(client_index, (char *)base64_to_process_and_decrypt, decrypted_metadata_cstring, size);

	clib__unlock(&clients_global_rwlock_guard);

	if (is_existing)
	{
		if (is_authenticated)
		{
			base__process_authenticated_client_message(websocket, client_index, decrypted_metadata_cstring);
		}
		else
		{
			base__process_not_authenticated_client_message(websocket, client_index, decrypted_metadata_cstring);
		}
	}

	memorymanager__free((nuint)decrypted_metadata_cstring);
	decrypted_metadata_cstring = 0;
}

/**
 * @brief this is function used that is used as an entry point for websocket thread
 *
 * this function calls theldus internal function that handles incoming websocket connections and that takes it from here
 * @return void
 *
 * */
void websocket_thread(void)
{
	//#ifdef DEBUG_PROGRAM
	printf("%s%d%s", "starting websocket server on port : ", g_server_settings.websocket_port, "\n");
	//#endif

	struct ws_events evs;
	evs.onopen = &onopen;
	evs.onclose = &onclose;
	evs.onmessage = &onmessage;
	ws_socket(&evs, g_server_settings.websocket_port, 1, 2000); /* Never returns. */
}

/**
 * @brief this is function used as entry point function of a thread that checks clients connectivity
 * *
 * @return void
 *
 * */
void websocket_connection_check_thread(void)
{
	static uint64 timestamp_now = 0;
	int i = 0;
	int size_of_allocated_message_buffer = 0;
	int *marked_client_ids_for_disconnect;
	char *msg = 0;
	int number_of_marked_clients = 0;

	marked_client_ids_for_disconnect = (int *)memorymanager__allocate(sizeof(int) * g_server_settings.max_client_count, MEMALLOC_MARKED_CLIENT_INDICES);

	while (is_server_running)
	{
		timestamp_now = base__get_timestamp_ms();

		//clib__null_memory(marked_client_ids_for_disconnect, sizeof(int) * g_server_settings.max_client_count);
		number_of_marked_clients = 0;

		clib__read_lock(&clients_global_rwlock_guard);

		for (i = 0; i < g_server_settings.max_client_count; i++)
		{
			if (!clients_array[i].is_existing && clients_array[i].timestamp_connected == 0)
			{
				continue;
			}

			if (clients_array[i].is_authenticated == TRUE)
			{
				if (clients_array[i].is_music_bot == TRUE)
				{
					continue;
				}

				timestamp_now = base__get_timestamp_ms();

				//
				//disconnect client who has not sent maintain_connection_message in given time limit
				//

				if (clients_array[i].timestamp_last_maintain_connection_message_received + 180000 < timestamp_now)
				{
					DBG_CONNECTION_CHECK_THREAD log_info("%s %p %s", "trying to disconnect client. did not receive maintain connection message : ", clients_array[i].p_ws_connection, "\n");

					marked_client_ids_for_disconnect[number_of_marked_clients] = i;
					number_of_marked_clients++;
				}
			}

			//
			//remove client who does not authenticate within given time limit
			//
			else
			{
				if (clients_array[i].timestamp_connected + 60000 < timestamp_now)
				{
					DBG_CONNECTION_CHECK_THREAD log_info("%s %p %s", "trying to disconnect client : ", clients_array[i].p_ws_connection, "\n");

					marked_client_ids_for_disconnect[number_of_marked_clients] = i;
					number_of_marked_clients++;
				}
			}
		}

		clib__unlock(&clients_global_rwlock_guard);

		if (number_of_marked_clients > 0)
		{
			clib__write_lock(&clients_global_rwlock_guard);
			clib__write_lock(&channels_global_rwlock_guard);

			for (i = 0; i < number_of_marked_clients; i++)
			{
				base__process_client_disconnect(marked_client_ids_for_disconnect[i]);
			}

			clib__unlock(&channels_global_rwlock_guard);
			clib__unlock(&clients_global_rwlock_guard);
		}

		sleep(60); //60 seconds, same in windows and linux
	}
}

/**
 * @brief self explanatory
 * *
 * @return void
 * */
static void _main_internal__init_channel_list(void)
{
	char channel_name[] = "root";
	char description[] = "this is default entry channel";

	channel_t *root_channel = &channel_array[0];

	root_channel->channel_id = 0;
	root_channel->parent_channel_id = -1;
	root_channel->is_existing = TRUE;
	root_channel->is_audio_enabled = TRUE;

	clib__copy_memory((void *)&channel_name, (void *)&root_channel->name, strlen(channel_name), CHANNEL_NAME_MAX_LENGTH);
	clib__copy_memory((void *)&description, (void *)&root_channel->description, strlen(description), CHANNEL_DESCRIPTION_MAX_LENGTH);
	root_channel->type = 1;
	root_channel->maintainer_id = -1;
}

/**
 * @brief self explanatory
 * *
 * @return void
 * */
static void _main_internal__init_tags_and_icons(void)
{
	char tag_name[] = "admin";
	tag_t *admin_tag = NULL_POINTER;
	icon_t *admin_icon = NULL_POINTER;

	admin_tag = &tags_array[0];
	admin_tag->id = ADMIN_TAG_ID; // na co bude toto?
	admin_tag->icon_id = 0;
	admin_tag->is_existing = TRUE;
	clib__copy_memory((void *)&tag_name, (void *)&admin_tag->name, strlen(tag_name), TAG_MAX_NAME_LENGTH);

	admin_icon = &icons_array[0];
	char base64_icon[] = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAACXBIWXMAAAsSAAALEgHS3X78AAACU0lEQVQ4jX2TX0hTYRjGf0eOicqUydEES9L+iHQRnNafu0ASYayrXWiS0S4HFQQhCQMzDEUGQUzIGyOJbrr03LSI/hFY5DBLhyut3CkkN9eZ7sx5bHVx9LgD0nv1vc/3PA/Py/d+AmYJLV3+fC67BsBGLst2bRo5CmszpwPw8fkzAUAAhI5AMF/RcNJGnBn02vobj6MAVJWXAnDlWjsRRXEIHYHg393Eo6Nu8iX1OJ0i0Q+fCfQ84erYe/ZJEmoiQVlpOb2BSxRlM9p/xQDNxw7TP9DG3Ysu1ESCaHzJ4hcVinV1GtklAeB0injdIbzukHUvuyTGw+MATMbmARC3hQDGyyCX+85QWduI1x2i4UIvAF53H+GJIfSsztj9IF+4vpNgXV/D3+mjpeUcajxjG8ff6cPf6bNhajxj8a0ED15NWoTK2kYcVfUAdLsP2MSNTUeB1za+aOR0dHUaR1k5sksiLzjQUinCE0M28eL8AgtzM8guieWv73YMAA41nyB6r52bg11oKylWV5aZWzOXKp02X0ksLqa6uoazrQaPHoao840Rm3phGhw/cpCnW/PHPk2hpdOMDM/aEnT3nLaM1HiGvdovs/+d+GGRlha/AjAyPEvrbcVMkDNXeeiWl/6BNpLJpM24COC7GrcALZ0GQCrbA0BFSQli3gAgXsBbSZrLJADFssezAVA69xaA83fCJPQNAPStDwbwpr8DgGzTKQAiiuIQASOiKAJQIXs8GkDs2zxVNfst8fpqCiO7WigUtk2tQ+FYssfzZxeciKLUAT8LsX+oaO/ttIYBtAAAAABJRU5ErkJggg==";
	admin_icon->id = 0;
	admin_icon->is_existing = TRUE;
	clib__copy_memory((void *)&base64_icon, (void *)&admin_icon->base64, strlen(base64_icon), ICON_MAX_LENGTH);
}

/**
 * @brief self explanatory
 * *
 * @return void
 * */
static void _main_internal__set_server_settings(void)
{
	char input[256];
	int i = 0;
	char verification_message[] = "welcome";
	char default_client_name[30] = "user";

	//
	//initialization vector must match iv defined in client.html
	//

	ITH_SHA256_CTX ctx;
	unsigned char custom_iv[16] = { 90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10 };

	clib__null_memory(&g_server_settings, sizeof(server_settings_t));
	//clib__copy_memory(verification_message, g_server_settings.client_verificaton_message_cleartext, strlen(verification_message), 1024);
	g_server_settings.websocket_message_max_length = 5000000;
	g_server_settings.websocket_chat_message_string_max_length = 8000;
	g_server_settings.chat_cooldown_milliseconds = 100;
	g_server_settings.join_channel_request_cooldown_milliseconds = 100;
	g_server_settings.create_channel_request_cooldown_milliseconds = 1000;
	g_server_settings.is_same_ip_address_allowed = TRUE;
	g_server_settings.is_voice_chat_active = TRUE;
	g_server_settings.is_hide_clients_in_password_protected_channels_active = TRUE;
	g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active = FALSE;
	g_server_settings.is_display_country_flags_active = FALSE;
	g_server_settings.is_display_admin_tag_active = TRUE;
	g_server_settings.is_idle_mode_allowed = TRUE;

	clib__copy_memory(default_client_name, g_server_settings.default_client_name, strlen(default_client_name), 100);

	printf("WebSocket port: ");
	fgets(input, sizeof(input), stdin);
	clib__sanitize_stdin(input);
	g_server_settings.websocket_port = strtol(input, 0, 10);
	clib__null_memory(input, sizeof(input));

	printf("%s", "enter number of keys to be used. (1-100) : ");
	fgets(input, sizeof(input), stdin);

	g_server_settings.keys_count = atoi(input);

	for (i = 0; i < g_server_settings.keys_count; i++)
	{
		clib__null_memory(input, sizeof(input));
		printf("%s%d%s", "specify key ", i, " : ");
		fgets(input, sizeof(input), stdin);
		clib__sanitize_stdin(input);

		ith_sha256_init(&ctx);
		ith_sha256_update(&ctx, (unsigned char *)input, strlen(input));
		ith_sha256_final(&ctx, g_server_settings.keys[i].key_value);

		//
		//destination,                 source,             length
		//

		clib__copy_memory(custom_iv, &g_server_settings.keys[i].key_iv, 16, 16);
	}

	clib__null_memory(input, sizeof(input));

	//clib__null_memory(input, sizeof(input));
	//printf("%s", "max allowed number of clients {from 1 to 499} : ");
	//fgets(input, sizeof(input), stdin);
	//clib__sanitize_stdin(input);

	g_server_settings.max_client_count = MAX_CLIENTS;
	g_server_settings.max_channel_count = MAX_CHANNELS;

	//g_server_settings.max_client_count = atoi(input);
	//zakomentovane, kvoli rychlejsiemu startu
	//if(g_server_settings.max_client_count > 499)
	// {
	//    printf("SETUP FAIL");
	//   return;
	// }

	//clib__null_memory(input, sizeof(input));
	//printf("%s", "max allowed number of channels {from 1 to 99} : ");
	//fgets(input, sizeof(input), stdin);
	//clib__sanitize_stdin(input);
	//g_server_settings.max_channel_count = atoi(input);

	//if(g_server_settings.max_client_count > 99)
	//{
	//    printf("SETUP FAIL");
	//    return;
	//}

	printf("%s", "enter admin password (50 chars max length): ");
	fgets(input, sizeof(input), stdin);
	clib__sanitize_stdin(input);
	clib__copy_memory(input, &g_server_settings.admin_password[0], clib__utf8_string_length(input), 50);
	clib__null_memory(input, sizeof(input));

	printf("%s", "disable voice chat? (y/n) ");
	fgets(input, sizeof(input), stdin);
	clib__sanitize_stdin(input);
	if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
	{
		g_server_settings.is_voice_chat_active = FALSE;
		printf("audio data (webrtc datachannels) disabled \n");
	}
	clib__null_memory(input, sizeof(input));

	printf("%s", "Prevent multiple clients with same ip address from connecting? (y/n) ");
	fgets(input, sizeof(input), stdin);
	clib__sanitize_stdin(input);
	if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
	{
		g_server_settings.is_same_ip_address_allowed = FALSE;
		printf("clients with same ip address not allowed \n");
	}
	clib__null_memory(input, sizeof(input));

	printf("%s", "Display flags near client? (y/n) ");
	fgets(input, sizeof(input), stdin);
	clib__sanitize_stdin(input);
	if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
	{
		g_server_settings.is_display_country_flags_active = TRUE;
		printf("server will send country flag info for client to each client \n");
	}
	clib__null_memory(input, sizeof(input));

	printf("%s", "disable idle clients? (y/n) ");
	fgets(input, sizeof(input), stdin);
	clib__sanitize_stdin(input);
	if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
	{
		g_server_settings.is_idle_mode_allowed = FALSE;
		printf("server will not allow clients to go idle \n");
	}
	clib__null_memory(input, sizeof(input));
}

/**
 * @brief
 *
 * @param juice_log_level_t as
 * @param const char *
 *
 * @return boole returns true if username has been assigned, false if not. username is assigned within this function, to avoid need allocating on heap (stack returning meh)
 *
 * @attention bad code
 */
static void log_handler(juice_log_level_t level, const char *message)
{
#ifndef WIN32

	FILE *file = stdout;
	time_t t = time(NULL);
	struct tm lt;
	char buffer[32];
	if (!localtime_r(&t, &lt) || strftime(buffer, 32, "%Y-%m-%d %H:%M:%S", &lt) == 0)
	{
		buffer[0] = '\0';
	}
	fprintf(file, "%s %-7s %s\n", buffer, log_level_to_string(level), message);
	fflush(file);
#endif

	DBG_VIOLET log_info("%s %s %s", "[violet]", message, "\n");
}

/**
 * @brief starts violet thread
 * *
 * @return char* encrypted string
 *
 */
static void _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void)
{
	violet_options_t vopts;
	violet_options_init(&vopts);

	//printf("%s", "[important] start_stun_turn_listener_for_webrtc_datachannel started \n");
	//char* argv[] = {
	//   "violet",
	//   "--credentials=usweger123:pw1wegweg23Q --log-level=verbose",
	//   0
	//};

	char *argv[] = { "violet", "--log-level=fatal", 0 };

	//char *argv[] = { "violet", "--log-level=error", 0 };

	//char *argv[] = { "violet", "--log-level=warn", 0 };
	//char *argv[] = { "violet", "--log-level=info", 0 };
	//char *argv[] = { "violet", "--log-level=verbose", 0 };

	if (violet_options_from_arg(2, argv, &vopts) < 0)
	{
		printf("%s", "[important] !violet_options_from_arg error \n");
		goto error;
	}

	juice_set_log_handler(log_handler);
	juice_set_log_level(vopts.log_level);

	vopts.config.port = 3478;

	juice_server_t *server = juice_server_create(&vopts.config);
	if (!server)
	{
		fprintf(stderr, "Server initialization failed\n");
		goto error;
	}

	//juice_server_destroy(server);
	//violet_options_destroy(&vopts);

error:

	//violet_options_destroy(&vopts);
	return;
}

/**
 * @brief prints out debug information at start
 *
 */
void _main_internal__print_debug_information(void)
{
	DBG_DLLMAIN printf("DBG_DLLMAIN active \n");
	DBG_CLIENT_MESSAGE printf("DBG_CLIENT_MESSAGE active \n");
	DBG_CLIENT_MESSAGE_MAIN_FUNCTION printf("DBG_CLIENT_MESSAGE_MAIN_FUNCTION active \n");
	DBG_AUTHENTICATION printf("DBG_AUTHENTICATION active \n");
	DBG_ENCRYPTION printf("DBG_ENCRYPTION active \n");
	DBG_SERVER_MESSAGE printf("DBG_SERVER_MESSAGE active \n");
	DBG_CLOSE_CONNECTION printf("DBG_CLOSE_CONNECTION active \n");
	DBG_ONMESSAGE printf("DBG_ONMESSAGE active \n");
	DBG_MEMORY_MANAGER printf("DBG_MEMORY_MANAGER active \n");
	DBG_CONNECTION_CHECK_THREAD printf("DBG_CONNECTION_CHECK_THREAD active \n");
	DBG_CLIENT_DISCONNECT printf("DBG_CLIENT_DISCONNECT active \n");
	DBG_AUDIOCHANNEL_WEBRTC printf("DBG_AUDIOCHANNEL_WEBRTC active \n");
	DBG_VIOLET printf("DBG_VIOLET active \n");
	DBG_DBG_MEMORY_ALLOCATIONS printf("DBG_DBG_MEMORY_ALLOCATIONS active \n");
	DBG_IP_TOOLS printf("DBG_IP_TOOLS active \n");
	DBG_MUSIC_BOT printf("DBG_MUSIC_BOT active \n");
	DBG_FILE_UPLOAD printf("DBG_FILE_UPLOAD active \n");
}

/**
 * @brief entry point
 *
 */
int main(void)
{
#ifdef DEBUG_ACTIVE
	printf("this is debug build \n");
	_main_internal__print_debug_information();
#endif

	//run this so rand() gives random output every time
	srand(time(0));

	char input[50];

	clib__rwlock_init(&clients_global_rwlock_guard);
	clib__rwlock_init(&webrtc_muggles_rwlock_guard);
	clib__rwlock_init(&channels_global_rwlock_guard);
	clib__rwlock_init(&tags_global_rwlock_guard);
	clib__rwlock_init(&icons_global_rwlock_guard);

	if (pthread_mutex_init(&chat_message_id_mutex, NULL))
	{
		log_info("%s", "pthread_rwlock_init chat_message_id_mutex init failed", 100, "\n");
		exit(0);
	}

	memorymanager__init();

	_main_internal__set_server_settings();
	clients_array = (client_t *)memorymanager__allocate(sizeof(client_t) * g_server_settings.max_client_count, MEMALLOC_CLIENTS_ARRAY);
	channel_array = (channel_t *)memorymanager__allocate(sizeof(channel_t) * g_server_settings.max_channel_count, MEMALLOC_CHANNELS_ARRAY);
	client_stored_data = (client_stored_data_t *)memorymanager__allocate(sizeof(client_stored_data_t) * MAX_CLIENT_STORED_DATA, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
	icons_array = (icon_t *)memorymanager__allocate(sizeof(icon_t) * MAX_ICONS, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
	tags_array = (tag_t *)memorymanager__allocate(sizeof(tag_t) * MAX_TAGS, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
	webrtc_muggles_array = (webrtc_peer_t *)memorymanager__allocate(sizeof(webrtc_peer_t) * g_server_settings.max_client_count, MEMALLOC_WEBRTC_PEERS);

	_main_internal__init_channel_list();
	_main_internal__init_tags_and_icons();

	pthread_create((pthread_t *)&thread_id0, 0, (void *)&websocket_thread, 0);
	pthread_create((pthread_t *)&thread_id1, 0, (void *)&websocket_connection_check_thread, 0);

	if (g_server_settings.is_voice_chat_active == TRUE)
	{
		pthread_create((pthread_t *)&thread_id2, 0, (void *)&_main_internal__start_stun_turn_listener_for_webrtc_datachannel, 0);
	}

	for (;;)
	{
		clib__null_memory(input, sizeof(input));
		fgets(input, sizeof(input), stdin);
		clib__sanitize_stdin(input);
	}

	return 0;
}
