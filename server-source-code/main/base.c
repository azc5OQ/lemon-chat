#include "definitions.h"

// use forward slashes "/" when specifying paths, not backward slashes "\" linux enviroment has trouble finding files that way
// windows compiler will work with both

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"

#include "../third-party/zhicheng/base64.h"
#include "../third-party/ITH-sha/sha256.h"
#include "../third-party/kokke-tiny-aes-c/aes.h"

#include "../third-party/libviolet/src/options.h"
#include "../third-party/libviolet/src/utils.h"

#include "../third-party/libviolet/deps/libjuice/include/juice/juice.h"

#include "../third-party/rxi-log/log.h"

#include "client_message.h"
#include "server_message.h"

#include "../third-party/libtom/libtommath/tommath.h"

#include "../third-party/mbedtls-3.5.1/include/mbedtls/build_info.h"
#include "../third-party/mbedtls-3.5.1/include/mbedtls/platform.h"
#include "../third-party/mbedtls-3.5.1/include/mbedtls/rsa.h"
#include "../third-party/mbedtls-3.5.1/include/mbedtls/entropy.h"
#include "../third-party/mbedtls-3.5.1/include/mbedtls/ctr_drbg.h"
#include "../third-party/mbedtls-3.5.1/include/mbedtls/bignum.h"

#include "memory_manager.h"
#include "audio_channel.h"

#include "../third-party/eteran-cvector/cvector.h"

#include "util.h"

#ifdef WIN32
#include <Windows.h>
#include <crtdbg.h>
#endif

custom_rwlock_t clients_global_rwlock_guard;
custom_rwlock_t channels_global_rwlock_guard;
custom_rwlock_t icons_global_rwlock_guard;
custom_rwlock_t tags_global_rwlock_guard;
custom_rwlock_t webrtc_muggles_rwlock_guard;

pthread_mutex_t chat_message_id_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64 chat_message_id;
client_t *clients_array;
channel_t *channel_array;
client_stored_data_t *client_stored_data;
icon_t *icons_array;
tag_t *tags_array;
server_settings_t g_server_settings;

/**
 * @brief self explanatory
 *
 * @param client_index
 *
 * @attention uses write lock on clients_global_rwlock_guard
 *
 * @return void
 */
boole base__is_request_allowed_based_on_spam_protection(int client_index)
{
	uint64 timestamp_now;
	boole skip = FALSE;

	clib__write_lock(&clients_global_rwlock_guard);
	timestamp_now = base__get_timestamp_ms();
	if (timestamp_now < (clients_array[client_index].timestamp_last_action + TIMESTAMP_LAST_ACTION_COOLDOWN_MS))
	{
		DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_client_username TIMESTAMP_LAST_ACTION_COOLDOWN_MS \n");
		skip = TRUE;
	}
	if (skip == FALSE)
	{
		clients_array[client_index].timestamp_last_action = timestamp_now;
	}

	clib__unlock(&clients_global_rwlock_guard);

	if (skip == TRUE)
	{
		return FALSE;
	}
	else
	{
		return TRUE;
	}
}

/**
 * @brief self explanatory
 *
 * @note hmmm
 *
 * @return void
 */
uint64 base__get_chat_message_id(void)
{
	uint64 to_return = 0;
	pthread_mutex_lock(&chat_message_id_mutex);

	to_return = chat_message_id;
	pthread_mutex_unlock(&chat_message_id_mutex);

	return to_return;
}

/**
 * @brief self explanatory
 * *
 * @note hmmm
 *
 * @return void
 */
void base__increment_chat_message_id(void)
{
	pthread_mutex_lock(&chat_message_id_mutex);

	chat_message_id++;

	pthread_mutex_unlock(&chat_message_id_mutex);
}

/**
 * @brief self explanatory
 *
 * @param int tag_id
 *
 * @return boole
 *
 * @attention
 */
boole base__is_tag_id_real(int tag_id)
{
	boole result;
	int i;

	result = FALSE;

	for (i = 0; i < MAX_TAGS; i++)
	{
		if (tags_array[i].is_existing == FALSE)
		{
			continue;
		}

		if (tags_array[i].id == tag_id)
		{
			result = TRUE;
			break;
		}
	}

	return result;
}

/**
 * @brief self explanatory
 *
 * @param int client_id
 * @param int this_tag_id

 *
 * @return boole
 *
 * @attention this function assumes that client_id is correct and that this_tag_id is also correct
 */
boole base__is_client_already_assigned_this_tag_id(int client_id, int this_tag_id)
{
	int cvector_loop_index = 0;
	client_t *client;

	client = &clients_array[client_id];

	boole is_tag_found = FALSE;
	if (client->tag_ids != NULL_POINTER)
	{
		for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
		{
			if (client->tag_ids[cvector_loop_index] == this_tag_id)
			{
				is_tag_found = TRUE;
				break;
			}
		}
	}

	return is_tag_found;
}

/**
 * @brief self explanatory
 *
 * @param int client_id
 * @param int this_tag_id

 *
 * @return boole
 *
 * @attention this function assumes that client_id is correct and that this_tag_id is also correct
 */
int base__get_index_of_tag_id_of_client(int client_id, int this_tag_id)
{
	int cvector_loop_index = 0;
	client_t *client;

	client = &clients_array[client_id];

	int tag_index = -1;

	if (client->tag_ids != NULL_POINTER)
	{
		for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
		{
			if (client->tag_ids[cvector_loop_index] == this_tag_id)
			{
				tag_index = cvector_loop_index;
				break;
			}
		}
	}

	return tag_index;
}

/**
 * @brief closes connection in thread safe way
 *
 * @param int client_index
 * @param boole use_readlock
 *
 * @return void returns nothing
 *
 * @attention closing connection takes more lines of code, and this function was created to eliminate that.
 */
void base__close_websocket_connection(int client_index, boole use_readlock)
{
	if (use_readlock)
	{
		clib__read_lock(&clients_global_rwlock_guard);
	}
	if (clients_array[client_index].is_existing)
	{
		if (clients_array[client_index].is_authenticated)
		{
			DBG_CLOSE_CONNECTION log_info("%s %d %s", "base__close_websocket_connection closing with authenticated client", client_index, "\n");
			ws_close_client(clients_array[client_index].p_ws_connection);
		}
		else
		{
			DBG_CLOSE_CONNECTION log_info("%s %d %s", "base__close_websocket_connection client with not authenticated client", client_index, "\n");
			ws_close_client(clients_array[client_index].p_ws_connection);
		}
	}
	else
	{
		DBG_CLOSE_CONNECTION log_info("%s %d %s", "base__close_websocket_connection client doesnt exist", client_index, "\n");
	}

	if (use_readlock)
	{
		clib__unlock(&clients_global_rwlock_guard);
	}
}

/**
 * @brief see title
 *
 * @param int channel_id
 *
 * @return int client count
 *
 */
int base__get_client_count_for_channel(int channel_id)
{
	int x;
	client_t *client;
	int result = 0;

	for (x = 0; x < g_server_settings.max_client_count; x++)
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

		if (client->is_music_bot == TRUE)
		{
			continue;
		}

		if (client->channel_id == channel_id)
		{
			result = result + 1;
		}
	}

	return result;
}

/**
 * @brief closes connection in thread safe way
 *
 * @param int channel_id
 * @param int* current_index cannot be NULL
 * @param int* channel_indices cannot be NULL
 *
 * @return void
 *
 * @attention closing connection takes more lines of code, and this function was created to eliminate that.
 */
void base__mark_channels_for_deletion(int channel_id, int *current_index, int *channel_indices)
{
	int i;

	if (current_index == NULL_POINTER || channel_indices == NULL_POINTER)
	{
		return;
	}

	for (i = 0; i < g_server_settings.max_channel_count; i++)
	{
		if (channel_id == channel_array[i].parent_channel_id)
		{
			channel_indices[*current_index] = i;
			*current_index += 1;
			DBG_CLIENT_MESSAGE log_info("%s %s %s", "mark_channels_for_deletion need to remove channel ", channel_array[i].name, " \n");
			base__mark_channels_for_deletion(channel_array[i].channel_id, current_index, channel_indices);
		}
	}
}

/**
 * @brief self explanatory
 *
 * @param char* public_key
 *
 * @return boole
 *
 * @attention
 */
boole base__is_public_key_present_in_client_stored_data(char *public_key)
{
	boole result = FALSE;
	boole status = FALSE;

	for (int i = 0; i < MAX_CLIENT_STORED_DATA; i++)
	{
		status = clib__is_string_equal(client_stored_data[i].public_key, public_key);
		if (status)
		{
			log_info("%s %s %s %s", "public key found in client stored data ", client_stored_data[i].public_key, public_key, "\n");
			result = TRUE;
			break;
		}
	}
	return result;
}

/**
 * @brief finds new index of maintainer of channel
 *
 * @param int* _out__new_index_of_maintainer pointer to is_existing int variable, where new index of maintainer will be stored
 * @param int channel_id
 * @param int index_of_client_that_left
 * @param boole do_not_include_client_that_left_when_searching_for_new_maintainer
 *
 * @note this is used function in situation where client leaves the channel for whatever reason and he happens to be the maintainer of it
 *
 * @return TRUE if maintainer is found, FALSE if not
 *
 * @attention bad code
 */
boole base__find_new_maintainer_for_channel(int *_out__new_index_of_maintainer, int channel_id, int index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer)
{
	int i;
	client_t *client;
	boole maintainer_found = FALSE;
	int *possible_new_maintainers = 0;
	int possible_new_maintainers_count = 0;
	int random_index = 0;

	possible_new_maintainers = (int *)memorymanager__allocate(sizeof(int) * g_server_settings.max_client_count, MEMALLOC_FIND_MAINTAINER);
	//maintainer will be random chosen

	for (i = 0; i < g_server_settings.max_client_count; i++)
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

		if (client->is_music_bot == TRUE)
		{
			continue;
		}

		if (do_not_include_client_that_left_when_searching_for_new_maintainer)
		{
			if (i == index_of_client_that_left)
			{
				continue;
			}
		}

		DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__find_new_maintainer_for_channel client hopped on is: ", i, "\n");

		maintainer_found = TRUE;

		possible_new_maintainers[possible_new_maintainers_count] = i;
		possible_new_maintainers_count++;
	}

	if (maintainer_found)
	{
		random_index = (int)(rand() % (possible_new_maintainers_count));
		*_out__new_index_of_maintainer = possible_new_maintainers[random_index];
		DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__find_new_maintainer_for_channel random chosen maintainer index", *_out__new_index_of_maintainer, "\n");
	}
	else
	{
		DBG_CLIENT_DISCONNECT log_info("%s ", "base__find_new_maintainer_for_channel maintainer not found \n");
	}

	memorymanager__free((nuint)possible_new_maintainers);

	return maintainer_found;
}

/**
 * @brief finds new index of maintainer of channel
 *
 * @param int* _out__new_index_of_maintainer pointer to is_existing int variable, where new index of maintainer will be stored
 * @param int channel_id
 * @param int index_of_client_that_left
 * @param boole do_not_include_client_that_left_when_searching_for_new_maintainer
 *
 * @note this is used function in situation where client leaves the channel for whatever reason and he happens to be the maintainer of it
 *
 * @return TRUE if maintainer is found, FALSE if not
 *
 * @attention bad code
 */
client_t *base__find_music_bot_in_channel(int channel_id)
{
	int i;
	client_t *client_in_loop = NULL_POINTER;

	client_t *result = NULL_POINTER;

	//maintainer will be random chosen

	for (i = 0; i < g_server_settings.max_client_count; i++)
	{
		client_in_loop = &clients_array[i];

		//
		//if statements that are most probable to run should be first in loop
		//

		if (client_in_loop->is_existing == FALSE)
		{
			continue;
		}

		if (client_in_loop->is_authenticated == FALSE)
		{
			continue;
		}

		if (client_in_loop->channel_id != channel_id)
		{
			continue;
		}

		if (client_in_loop->is_music_bot == FALSE)
		{
			continue;
		}

		DBG_DLLMAIN log_info("%s %s %s", "base__find_music_bot_in_channel client ", client_in_loop->username, " is the music bot \n");

		result = client_in_loop;
	}

	return result;
}

/**
 * @brief assigns username to client
 *
 * @param client_index index of client that needs new username
 * @param default_name default part of name string
 *
 * @return boole returns true if username has been assigned, false if not. username is assigned within this function, to avoid need allocating on heap (stack returning meh)
 *
 * @attention bad code
 */
boole base__assign_username_for_newly_joined_client(int client_index, cstring default_name)
{
	boole status = FALSE;
	char *public_key = 0;
	char loop_username_buffer[USERNAME_MAX_LENGTH];
	char tempbuf[16];
	int usernameindex;
	boole status1 = false;
	boole result = false;

	//kontrola ci server nahodou nepozna public key

	public_key = &clients_array[client_index].public_key[0];
	DBG_AUTHENTICATION log_info("%s %s %s", "newly joined client public key is -> ", public_key, "\n");

	status = base__is_public_key_present_in_client_stored_data(public_key);

	if (status)
	{
		DBG_AUTHENTICATION log_info("public key present \n");
		//handle .. na neskor
	}
	else
	{
		DBG_AUTHENTICATION log_info("public key not present \n");
		//two assumptions for safe operation
		//default name wont be too long
		//max number of clinets will fit into 16 digits (easy), edge cases will have 3 digit, extremely edge cases 4

		//choose index that will be appended to nickname
		for (usernameindex = 0; usernameindex < g_server_settings.max_client_count; usernameindex++)
		{
			clib__null_memory(loop_username_buffer, USERNAME_MAX_LENGTH);
			clib__copy_memory(g_server_settings.default_client_name, loop_username_buffer, clib__utf8_string_length(g_server_settings.default_client_name), USERNAME_MAX_LENGTH);

			//as long as i know what im doing, shouldnt be dangerous
			clib__null_memory(tempbuf, 16);

			//itoa(usernameindex, tempbuf, 10); gcc on linux does not support itoa so im using sprintf

			sprintf(tempbuf, "%d", usernameindex);

			int destination_index = clib__utf8_string_length(loop_username_buffer);
			clib__copy_memory(&tempbuf[0], &loop_username_buffer[destination_index], clib__utf8_string_length(tempbuf), clib__utf8_string_length(g_server_settings.default_client_name));

			DBG_AUTHENTICATION log_info("%s %s %s", "tempbuf -> ", tempbuf, "\n");
			DBG_AUTHENTICATION log_info("%s %s %s", "loop_username_buffer -> ", loop_username_buffer, "\n");

			for (int i = 0; i < g_server_settings.max_client_count; i++)
			{
				//is_existing needs to be guarded with mutex, while opening client, rwlock is not usable

				//log_info("%s %s %d %s", "trying username -> ", loop_username_buffer , i, "\n");

				if (!clients_array[i].is_existing)
				{ //client not is_existing, skip, this need global lock
					goto final_check;
				}

				if (!clients_array[i].is_authenticated)
				{
					goto final_check;
				}

				if (i == client_index)
				{ //skip current client
					goto final_check;
				}

				status = clib__is_string_equal(clients_array[i].username, loop_username_buffer);

				if (status)
				{ //username used by some of the clients, start another loop, with incremented numeric part of clients username
					DBG_AUTHENTICATION log_info("%s %s %s", "username ", loop_username_buffer, " it is not available \n");
					break;
				}

final_check:

				if ((i + 1) == g_server_settings.max_client_count)
				{ //if loop reached its end and username currently used in this loop was found to not be used by any of the clients
					//go to end_loop where this newly found username will be assigned to client
					DBG_AUTHENTICATION log_info("%s", "(i + 1) == g_server_settings.max_client_count goto end_loop \n");
					result = true;
					goto end_loop;
				}

				//still some clients that need to be checked, not needed , included just for
			}
		}
	}

	return result; //either code jumps to end_loop or it returns false here

end_loop:
	clib__copy_memory(&loop_username_buffer[0], &clients_array[client_index].username[0], clib__utf8_string_length(loop_username_buffer), USERNAME_MAX_LENGTH);
	DBG_AUTHENTICATION log_info("%s %d %s %s", "client: ", client_index, clients_array[client_index].username, "\n");

	return result;
}

/**
 * @brief self explanatory
 *
 * @param char* block
 * @param int length
 *
 * @attention used within wrlock on clients_global_rwlock_guard
 *
 * @return void
 */
int base__get_new_index_for_client(void)
{
	int new_index = -1;
	int i = 0;

	if ((g_server_settings.client_count + 1) < g_server_settings.max_client_count)
	{
		for (i = 0; i < g_server_settings.max_client_count; i++)
		{
			if (clients_array[i].timestamp_connected == 0)
			{
				new_index = i;
				break;
			}
		}
	}

	return new_index;
}

/**
 * @brief self explanatory
 *
 * @param char* block
 * @param int length
 *
 * @return void
 */
void base__fill_block_of_data_with_ascii_characters(char *block, int length)
{
	int lowerbound = 33;
	int upperbound = 126;
	for (int i = 0; i < length; i++)
	{
		block[i] = (char)(rand() % (upperbound - lowerbound + 1)) + lowerbound;
	}
}

/**
 * @brief frees some artifacts from working with json, used mostly in server_message.c
 *
 * @param cJSON* json_root_object1
 * @param char* json_root_object1_string
 *
 * @return void
 *
 * @attention Function not really nessecary
 */
void base__free_json_message(cJSON *json_root_object1, char *json_root_object1_string)
{
	if (json_root_object1 != NULL_POINTER)
	{
		//log_info("base__free_json_message() cJSON_Delete(json_root_object1); \n");
		cJSON_Delete(json_root_object1);
	}

	if (json_root_object1_string != NULL_POINTER)
	{
		free(json_root_object1_string);
	}
}

/**
 * @brief returns timestamp in milliseconds
 * *
 * @return void
 *
 * @attention should work on windows and linux
 */
uint64 base__get_timestamp_ms(void)
{
#ifdef WIN32

	uint64 timestamp_msec = GetTickCount64();
	return timestamp_msec;
#endif

#ifdef __linux__
	/*static struct timeb timer_msec;
	static uint64 timestamp_msec;
	timestamp_msec = 0;
	if (!ftime(&timer_msec))
	{
		timestamp_msec = ((uint64)timer_msec.time) * 1000 + (uint64)timer_msec.millitm;
	}
	else
	{
		timestamp_msec = -1;
	}*/

	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64 timestamp_msec = (uint64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
	return timestamp_msec;
#endif
}

/**
 * @brief sleeps for X milliseconds
 * *
 * @return void
 *
 * @attention should work on windows and linux
 */
#ifdef WIN32
#include <windows.h>
#elif _POSIX_C_SOURCE >= 199309L
#include <time.h> // for nanosleep
#else
#include <unistd.h> // for usleep
#endif

void base__sleep_for_milliseconds(int milliseconds)
{ // cross-platform sleep function
#ifdef WIN32
	Sleep(milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
	struct timespec ts;
	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (milliseconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
#else
	if (milliseconds >= 1000)
	{
		sleep(milliseconds / 1000);
	}
	usleep((milliseconds % 1000) * 1000);
#endif
}

/**
 * @brief little helper function
 *
 * @param public_key websocket client structure
 *
 * @return boole
 *
 * @attention output of this function is stored on heap, and must be freed manually
 */
boole base__is_there_a_client_with_same_public_key(cstring public_key)
{
	boole result = FALSE;
	int i = 0;
	for (i = 0; i < g_server_settings.max_client_count; i++)
	{
		if (clients_array[i].is_existing == FALSE)
		{
			continue;
		}

		if (clients_array[i].is_authenticated == FALSE)
		{
			continue;
		}

		if (clib__is_string_equal(clients_array[i].public_key, public_key))
		{
			result = TRUE;
			break;
		}
	}
	return result;
}

/**
 * @brief part of authentication process.
 *
 * @param cstring ip address
 *
 * @return boole
 *
 * @attention output of this function is stored on heap, and must be freed manually
 */
boole base__is_there_a_client_with_same_ip_address(cstring ip_address)
{
	boole result = FALSE;
	int i = 0;
	for (i = 0; i < g_server_settings.max_client_count; i++)
	{
		if (clients_array[i].is_existing == FALSE)
		{
			continue;
		}

		//doesnt have to be authenticated

		if (clib__is_string_equal(clients_array[i].ip_address, ip_address))
		{
			result = TRUE;
			break;
		}
	}
	return result;
}

/**
 * @brief encrypts string and converts it to base64 string.
 *
 * @param char* string_to_encrypt
 * @param int* out_allocated_buffer_size
 * @param char* dh_shared_secret
 *
 * @return char* encrypted string
 *
 * @attention the input string needs to be null-terminated. Its length is determined by position of null-terminator
 */
char *base__encrypt_cstring_and_convert_to_base64(char *string_to_encrypt, int *out_allocated_buffer_size, char *dh_shared_secret)
{
	int encryption_buffer_size = 0;
	int base64_out_string_size = 0;
	unsigned char *encryption_buffer = 0;
	char *base64_out_string = 0;
	struct AES_ctx ctx;
	int i = 0;
	struct ITH_SHA256_CTX sha256ctx;
	unsigned char shared_key_iv[16] = { 90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10 };
	unsigned char shared_key[32];
	int passed_string_length;
	int shared_secret_length;

	passed_string_length = (int)clib__utf8_string_length(string_to_encrypt);

	DBG_ENCRYPTION log_info("%s %d %s", "base__encrypt_cstring_and_convert_to_base64() string length, ", passed_string_length, "\n");

	//why is this done this way?
	if (passed_string_length < 1026)
	{
		encryption_buffer_size = 1026;
		base64_out_string_size = ((4 * encryption_buffer_size / 3) + 3) & ~3;
	}
	else
	{
		encryption_buffer_size = passed_string_length;
		base64_out_string_size = ((4 * encryption_buffer_size / 3) + 3) & ~3;
	}

	base64_out_string_size += 4;

	//check size

	if (base64_out_string_size > g_server_settings.websocket_message_max_length)
	{
		DBG_ENCRYPTION log_info("%s", "base__encrypt_cstring_and_convert_to_base64()  base64_out_string_size > g_server_settings.websocket_message_max_length) returning null \n");
		return 0;
	}

	*out_allocated_buffer_size = base64_out_string_size;

	DBG_ENCRYPTION log_info("%s %d %s", "base__encrypt_cstring_and_convert_to_base64() encryption_buffer_size", encryption_buffer_size, "\n");
	DBG_ENCRYPTION log_info("%s %d %s", "base__encrypt_cstring_and_convert_to_base64() passed_string_length", passed_string_length, "\n");
	DBG_ENCRYPTION log_info("%s %d %s", "base64_out_string_size() base64_out_string_size", base64_out_string_size, "\n");

	encryption_buffer = (unsigned char *)memorymanager__allocate(encryption_buffer_size, MEMALLOC_TYPE_ENCRYPT);
	base64_out_string = (char *)memorymanager__allocate(base64_out_string_size, MEMALLOC_TYPE_ENCRYPT);

	clib__copy_memory(string_to_encrypt, encryption_buffer, passed_string_length, encryption_buffer_size);

	for (i = 0; i < g_server_settings.keys_count; i++)
	{
		AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, g_server_settings.keys[i].key_iv);
		AES_CTR_xcrypt_buffer(&ctx, encryption_buffer, encryption_buffer_size);
	}

	DBG_ENCRYPTION log_info("%s", "base__encrypt_cstring_and_convert_to_base64() 1 \n");

	//if shared secret isnt null, use it
	if (dh_shared_secret != NULL_POINTER)
	{
		shared_secret_length = clib__utf8_string_length(dh_shared_secret);

		clib__null_memory(shared_key, sizeof(shared_key));

		ith_sha256_init(&sha256ctx);
		ith_sha256_update(&sha256ctx, (unsigned char *)dh_shared_secret, shared_secret_length);
		ith_sha256_final(&sha256ctx, shared_key);

		AES_init_ctx_iv(&ctx, shared_key, shared_key_iv);
		AES_CTR_xcrypt_buffer(&ctx, encryption_buffer, encryption_buffer_size);
	}

	DBG_ENCRYPTION log_info("%s", "base__encrypt_cstring_and_convert_to_base64() 2 \n");

	zchg_base64_encode(encryption_buffer, encryption_buffer_size, base64_out_string);

	//
	//encryption_buffer is freed here but the base64_out_string is freed outside of function
	//

	if (encryption_buffer != NULL_POINTER)
	{
		memorymanager__free((nuint)encryption_buffer);
	}
	//DBG_ENCRYPTION log_info("%s","base__encrypt_cstring_and_convert_to_base64() 3 \n");

	//DBG_ENCRYPTION log_info("%s %lu %s","base64_out_string_size() strlen(base64_out_string)" , clib__utf8_string_length(base64_out_string), "\n");

	return base64_out_string;
}

/**
 * @brief decrypts base64 string (used for decrypting metadata of message, not contents, contents are decrypted on clients end)
 *
 * @param int client_id
 * @param char* base64_string
 * @param unsigned char* out_buffer
 * @param int out_buffer_length
 *
 * @attention this function is used within read lock on clients_array, result from this function must be manually freed
 *
 * @return void
 */
void base__get_data_from_base64_and_decrypt_it(int client_id, char *base64_string, unsigned char *out_buffer, int out_buffer_length)
{
	client_t *client = &clients_array[client_id];

	DBG_ENCRYPTION log_info("%s %d %s", "base__get_data_from_base64_and_decrypt_it ", client_id, "\n");

	if (client != NULL_POINTER && client->is_existing == TRUE && client->is_dh_shared_secret_agreed_upon == TRUE)
	{
		//
		//decrypt without shared secret
		//

		struct AES_ctx ctx;
		int i = 0;

		DBG_ENCRYPTION log_info("%s %lu %s", "get_data_from_base64_and_derypt_it", i, "\n");

		zchg_base64_decode(base64_string, strlen(base64_string), out_buffer);

		//
		//first key used in decryption is last key used in encryption
		//this only decrypts metadata. Does not decrypt content of messages
		//

		for (i = (g_server_settings.keys_count - 1); i >= 0; i--)
		{
			DBG_ENCRYPTION log_info("%s %d %s", "decrypting data with key", i, "\n");

			AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, g_server_settings.keys[i].key_iv);
			AES_CTR_xcrypt_buffer(&ctx, out_buffer, out_buffer_length);
		}

		unsigned char shared_key_iv[16] = { 90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10 };

		DBG_ENCRYPTION log_info("%s %s %s", "trying to decrypt with shared secret ", client->dh_shared_secret, "\n");

		struct ITH_SHA256_CTX sha256ctx;

		unsigned char shared_key[32] = { 0 };

		ith_sha256_init(&sha256ctx);
		ith_sha256_update(&sha256ctx, (unsigned char *)client->dh_shared_secret, strlen(client->dh_shared_secret));
		ith_sha256_final(&sha256ctx, shared_key);

		AES_init_ctx_iv(&ctx, shared_key, shared_key_iv);
		AES_CTR_xcrypt_buffer(&ctx, out_buffer, out_buffer_length);
	}
	else
	{
		//
		//decrypt with shared secret
		//

		struct AES_ctx ctx;
		int i = 0;

		DBG_ENCRYPTION log_info("%s %d %s", "get_data_from_base64_and_derypt_it ", strlen(base64_string), "\n");

		zchg_base64_decode(base64_string, strlen(base64_string), out_buffer);

		//
		//first key used in decryption is last key used in encryption
		//this only decrypts metadata. Does not decrypt content of messages
		//

		for (i = (g_server_settings.keys_count - 1); i >= 0; i--)
		{
			DBG_ENCRYPTION log_info("%s %d %s", "decrypting data with key", i, "\n");

			AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, g_server_settings.keys[i].key_iv);
			AES_CTR_xcrypt_buffer(&ctx, out_buffer, out_buffer_length);
		}
	}
}

/**
 * @brief encrypts string with public key
 *
 * @param char* public_key_modulus
 * @param unsigned char* bytes
 * @param buffer_length buffer_length
 *
 * @return char* encrypted string
 *
 * @attention only modulus is needed, exponent is defined in code, base is defined in code
 * @note this function is used only once, when server sends out public key challenge to client to find out if client is owner of that public key
 */
char *base__encrypt_string_with_public_key(char *public_key_modulus, unsigned char *bytes, uint64 buffer_length)
{
	int status = 0;
	mbedtls_rsa_context rsa;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_mpi N;
	mbedtls_mpi E;
	const char *pers = "rsa_encrypt";

	DBG_ENCRYPTION log_info("%s %s %s", "public_key_modulus_base64 ", public_key_modulus, "\n");

	char *public_key_modulus_binary = (char *)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

	unsigned int buffer_modulus_bin_outsize = zchg_base64_decode(public_key_modulus, strlen(public_key_modulus), (unsigned char *)public_key_modulus_binary);

	DBG_ENCRYPTION log_info("%s %d %s", "buffer_modulus_bin_outsize ", buffer_modulus_bin_outsize, "\n");

	mbedtls_rsa_init(&rsa);
	mbedtls_ctr_drbg_init(&ctr_drbg);

	mbedtls_entropy_init(&entropy);

	int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers));
	if (ret != 0)
	{
		DBG_ENCRYPTION mbedtls_printf(" failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret);
	}

	//modulus and exponent; others are not needed for successful import
	mbedtls_mpi_init(&N);
	mbedtls_mpi_read_binary(&N, public_key_modulus_binary, buffer_modulus_bin_outsize);
	//status = mbedtls_mpi_read_string(&N, 64, public_key_modulus_base64);
	//N -> modulus

	if (status != 0)
	{
		DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string N failed ", status, "\n");
	}

	//ok problem zisteny, dokaze citat iba stringove cislo v dvojkovej az 16 tkovej sustave
	//nedokaze parsnut base64 cislo

	mbedtls_mpi_init(&E);
	//E -> exponent

	//load exponent from string (3)
	status = mbedtls_mpi_read_string(&E, 10, "3");

	if (status != 0)
	{
		DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string E failed ", status, "\n");
	}

	status = mbedtls_rsa_import(&rsa, &N, NULL, NULL, NULL, &E);

	if (status != 0)
	{
		DBG_ENCRYPTION log_info("%s %d %s", " [FAIL] status ", status, "\n");
	}

	unsigned char *inputbuffer = (unsigned char *)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

	clib__copy_memory(bytes, inputbuffer, buffer_length, 256);

	unsigned char *outbuffer = (unsigned char *)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

	status = mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg, buffer_length, inputbuffer, outbuffer);

	//status = mbedtls_rsa_public(&rsa, inputbuffer, outbuffer);
	//mbedtls_rsa_pkcs1_encrypt()

	if (status != 0)
	{
		DBG_ENCRYPTION log_info("%s %X %s", "[!] base__encrypt_string_with_public_key failed ", status, " \n");
	}
	else
	{
		DBG_ENCRYPTION log_info("%s %d %s", "[!] base__encrypt_string_with_public_key succeeded ", status, " \n");
	}

	size_t base64_out_string_size = ((4 * 256 / 3) + 3) & ~3;

	DBG_ENCRYPTION log_info("%s %d %s", "base64_out_string_size -> ", base64_out_string_size, "\n");

	void *base64_out_buffer = (void *)memorymanager__allocate(base64_out_string_size * 2, MEMALLOC_PUBLIC_KEY_ENCRYPT);

	zchg_base64_encode(outbuffer, 256, base64_out_buffer);

	memorymanager__free((nuint)public_key_modulus_binary);
	memorymanager__free((nuint)outbuffer);
	memorymanager__free((nuint)inputbuffer);

	return base64_out_buffer;
}

/**
 * @brief this function takes care of events that must happen when client disconnects
 *
 * @param int client_index
 * @attention caller of this function must place this function in write lock
 * @return void
 */
void base__process_client_disconnect(int client_index)
{
	int channel_id;
	int new_maintainer_index;
	boole status;
	boole is_client_also_channel_maintainer;
	client_t *client;

	DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__process_client_disconnect ", client_index, "\n");

	client = &clients_array[client_index];

	if (client->is_existing)
	{
		DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__process_client_disconnect is_existing TRUE ", client_index, "\n");

		audio_channel__process_client_disconnect(client);

		channel_id = clients_array[client_index].channel_id;

		is_client_also_channel_maintainer = (boole)(channel_array[channel_id].maintainer_id == client_index);

		DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__process_client_disconnect setting client struct to null ", client_index, "\n");

		if (client->tag_ids != NULL_POINTER)
		{
			cvector_free(client->tag_ids);
		}

		//tag_ids are the only thing stored in fector
		//if vector is NULL, thats okay

		//clear out file upload buffer
		if (client->file_upload_extension.file_upload_buffer != NULL_POINTER)
		{
			memorymanager__free((nuint)client->file_upload_extension.file_upload_buffer);
		}

		clib__null_memory(client, sizeof(client_t));

		server_msg__send_client_disconnect_message_to_all_clients(client_index);

		if (is_client_also_channel_maintainer == TRUE)
		{
			DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__process_client_disconnect client was maintainer of channel ", client_index, "\n");

			status = base__find_new_maintainer_for_channel(&new_maintainer_index, channel_id, client_index, FALSE);

			if (status)
			{
				DBG_CLIENT_DISCONNECT log_info("%s %d %s", "base__process_client_disconnect new maintainer found ", new_maintainer_index, "\n");
				channel_array[channel_id].is_channel_maintainer_present = TRUE;
				channel_array[channel_id].maintainer_id = new_maintainer_index;

				server_msg__send_maintainer_id_to_clients_in_same_channel(channel_id, new_maintainer_index);
			}
			else
			{
				channel_array[channel_id].is_channel_maintainer_present = FALSE;
				channel_array[channel_id].maintainer_id = 0;
				DBG_CLIENT_DISCONNECT log_info("%s", "base__process_client_disconnect failed to find new maintainer \n");
			}
		}
	}
}

/**
 * @brief self explanatory
 *
 * @param ws_cli_conn_t* websocket
 * @param int client_index
 * @param char *decrypted_metadata_cstring
 *
 * @return void
 * */
void base__process_authenticated_client_message(ws_cli_conn_t *websocket, int client_index, char *decrypted_metadata_cstring)
{
	cJSON *json_root = 0;
	boole status = FALSE;
	char *message_type = 0;
	client_t *client;

	boole is_sender_idle = FALSE;

	//DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %s %s", "base__process_authenticated_client_message message : ", decrypted_metadata_cstring, "\n");

	json_root = cJSON_Parse(decrypted_metadata_cstring);

	if (json_root == NULL_POINTER)
	{
		DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %d %s", "client : ", client_index, " json_root is null \n");
		ws_close_client(websocket);
		//there is no json object to call cJSON_Delete on so just disconnect the client
		return;
	}

	status = client_msg__is_message_correct_at_first_sight_and_get_message_type(json_root, client_index, &message_type);

	if (!status)
	{
		DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s%d%s", "client : ", client_index, "client_msg__is_message_correct_at_first_sight_and_get_message_type failed \n");
		ws_close_client(websocket);
		cJSON_Delete(json_root);
		return;
	}

	DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %d %s %s %s", "client : ", client_index, " detected message type is ", message_type, "\n");

	clib__read_lock(&clients_global_rwlock_guard);
	is_sender_idle = clients_array[client_index].is_idle;
	clib__unlock(&clients_global_rwlock_guard);

	//todo , ignore audio related messages if audio is completely disabled by server

	//first two messages are the ones where it doesnt matter if client is in idle state or not
	if (clib__is_string_equal(message_type, "client_connection_check"))
	{
		client_msg__process_client_connection_check(json_root, client_index);
	}
	else if (clib__is_string_equal(message_type, "create_new_webrtc_datachannel_connection"))
	{
		client_msg__process_create_new_webrtc_datachannel_connection(json_root, client_index);
	}
	else
	{
		//else block checks messages where it matters if client is in idle state or not

		if (is_sender_idle == FALSE)
		{
			if (clib__is_string_equal(message_type, "change_client_username"))
			{
				client_msg__process_change_client_username(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "create_channel_request"))
			{
				client_msg__process_create_channel_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "edit_channel_request"))
			{
				client_msg__process_edit_channel_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "direct_chat_message"))
			{
				client_msg__process_direct_chat_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "channel_chat_message"))
			{
				client_msg__process_channel_chat_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "join_channel_request"))
			{
				client_msg__process_join_channel_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "delete_channel_request"))
			{
				client_msg__process_delete_channel_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "poke_client"))
			{
				client_msg__process_poke_client_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "sdp_answer"))
			{
				client_msg__process_sdp_answer(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "ice_candidate"))
			{
				client_msg__process_ice_candidate(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "microphone_usage"))
			{
				client_msg__process_microphone_usage(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "start_song_stream"))
			{
				client_msg__process_start_song_stream_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "stop_song_stream"))
			{
				client_msg__process_stop_song_stream_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "admin_password"))
			{
				client_msg__process_admin_password_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "add_tag_to_client"))
			{
				client_msg__process_add_tag_to_client_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "remove_tag_from_client"))
			{
				client_msg__process_remove_tag_from_client_message(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "server_settings_icon_upload"))
			{
				client_msg__process_set_server_settings_icon_upload(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "server_settings_add_new_tag"))
			{
				client_msg__process_set_server_settings_add_new_tag(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "call_idle_client_request"))
			{
				if (g_server_settings.is_idle_mode_allowed == TRUE)
				{
					client_msg__process_call_idle_client_message(json_root, client_index);
				}
			}
			else if (clib__is_string_equal(message_type, "go_to_idle_mode_request"))
			{
				if (g_server_settings.is_idle_mode_allowed == TRUE)
				{
					client_msg__process_go_to_idle_mode_request(json_root, client_index);
				}
			}
			else if (clib__is_string_equal(message_type, "kick"))
			{
				client_msg__process_kick_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "ban"))
			{
				client_msg__process_ban_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "create_music_bot"))
			{
				client_msg__process_create_music_bot_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "delete_music_bot"))
			{
				client_msg__process_delete_music_bot_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "musicbot_get_song_list"))
			{
				client_msg__process_musicbot_get_song_list_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "remove_song_from_music_bot"))
			{
				client_msg__process_remove_song_from_music_bot_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "file_send"))
			{
				client_msg__process_file_send_request(json_root, client_index);
			}
			else if (clib__is_string_equal(message_type, "file_send_completed"))
			{
				//
				//client_msg__process_direct_chat_picture
				//and
				//client_msg__process_channel_chat_picture
				//gets called by client_msg__process_file_send_completed_request if needed
				//

				client_msg__process_file_send_completed_request(json_root, client_index);
			}
		}

		if (is_sender_idle == TRUE)
		{
			if (clib__is_string_equal(message_type, "come_back_from_idle_mode_request"))
			{
				if (g_server_settings.is_idle_mode_allowed == TRUE)
				{
					client_msg__process_come_back_from_idle_mode_request(json_root, client_index);
				}
			}
		}
	}

	DBG_DBG_MEMORY_ALLOCATIONS memorymanager__print_allocations_count();

	cJSON_Delete(json_root);
	json_root = 0;
}

/**
 * @brief self explanatory
 *
 * @param ws_cli_conn_t* websocket
 * @param int index
 * @param char *decrypted_metadata_cstring
 *
 * @return void
 * */
void base__process_not_authenticated_client_message(ws_cli_conn_t *websocket, int index, char *decrypted_metadata_cstring)
{
	cJSON *json_root = 0;
	boole status = FALSE;
	int status1 = 0;
	char *message_type = 0;

	DBG_AUTHENTICATION log_info("%s %p %s", "[i] authenticating client ", websocket, "\n");
	DBG_AUTHENTICATION log_info("%s %s %s", "decrypted client verification message : ", decrypted_metadata_cstring, "\n");

	//
	//check message length first. If string is too long( more than 1000 chars) drop it
	//

	status1 = clib__utf8_string_length_check_max_length(decrypted_metadata_cstring, 1000);

	if (status1 == -1)
	{
		DBG_AUTHENTICATION log_info("%s %s %s", "base__process_not_authenticated_client_message message has more chars than allowed : ", decrypted_metadata_cstring, "\n");
		ws_close_client(websocket);
		return;
	}

	json_root = cJSON_Parse(decrypted_metadata_cstring);

	DBG_AUTHENTICATION log_info("%s", "base__process_not_authenticated_client_message decrypted_metadata_cstring \n");

	if (json_root == 0)
	{
		DBG_AUTHENTICATION log_info("%s %d %s", "client : ", index, " json_root is null \n");
		ws_close_client(websocket);
		//there is no json object to cJSON_Delete, since its 0
		return;
	}

	status = client_msg__is_message_correct_at_first_sight_and_get_message_type(json_root, index, &message_type);

	if (!status)
	{
		DBG_AUTHENTICATION log_info("%s%d%s", "client : ", index, "client_msg__is_message_correct_at_first_sight_and_get_message_type failed \n");
		ws_close_client(websocket);
		cJSON_Delete(json_root);
		return;
	}

	DBG_AUTHENTICATION log_info("%s %d %s %s %s", "client : ", index, " detected message type is ", message_type, "\n");

	if (clib__is_string_equal(message_type, "public_key_info"))
	{
		client_msg__process_public_key_info(json_root, index);
	}
	else if (clib__is_string_equal(message_type, "public_key_challenge_response"))
	{
		client_msg__process_public_key_challenge_response(json_root, index);
	}

	cJSON_Delete(json_root);
	json_root = 0;
}



int base__get_other_clients_in_channel(int client_to_ignore, int channel_id, int *receiving_client_ids)
{
	int count = 0;
	for (int i = 0; i < g_server_settings.max_client_count; i++)
	{
		boole status = util__is_client_valid(i);

		if (status == TRUE && i != client_to_ignore)
		{
			receiving_client_ids[count] = i;
			count++;
		}
	}

	return count;
}