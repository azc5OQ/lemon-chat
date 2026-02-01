#ifndef BASE_H
#define BASE_H

//everything should be as simple as possible, but not simpler!

#define MAX_CHANNELS 100
#define MAX_CLIENTS 500
#define MAX_CLIENT_STORED_DATA 100
#define MAX_ICONS 1000
#define MAX_TAGS 1000
#define ROOT_CHANNEL_ID 0
#define USERNAME_MAX_LENGTH 100
#define TIMESTAMP_LAST_ACTION_COOLDOWN_MS 100
#define CHANNEL_PASSWORD_MAX_LENGTH 128
#define CHANNEL_DESCRIPTION_MAX_LENGTH 1000
#define MAX_PUBLIC_KEY_LENGTH 1000
#define CHANNEL_NAME_MAX_LENGTH 128
#define SONG_NAME_MAX_LENGTH 512
#define TAG_MAX_NAME_LENGTH 32
#define ICON_MAX_LENGTH 8192
#define SHARED_SECRET_LENGTH 1000
#define MAX_TAGS_PER_USER 32
#define ADMIN_TAG_ID 0
#define CHALLENGE_STRING_LENGTH 128
#define ADMIN_PASSWORD_MAX_LENGTH 50
#define COUNTRY_ISO_CODE_LENGTH 3
#define MAX_TAGS_FOR_SINGLE_CLIENT 30
#define MAX_CLIENT_AVATAR_LENGTH 131072
#define MUSIC_BOT_MAX_FILE_COUNT 200
#define MAX_CLIENT_FILE_UPLOAD_LENGTH 13946000

#include "../theldus-websocket/include/ws.h"
#include "mytypedef.h"
#include <pthread.h>

typedef struct key_data
{
	int key_data_type;
	unsigned char key_value[32];
	unsigned char key_iv[16];
} key_data_t;

//
// todo, use int64 whenever possible
//

typedef struct server_settings
{
	boole is_same_ip_address_allowed;
	boole is_hide_clients_in_password_protected_channels_active;
	boole is_restrict_channel_deletion_creation_editing_to_admin_active;
	boole is_display_country_flags_active;
	boole is_display_admin_tag_active;
	boole is_idle_mode_allowed;
	boole is_voice_chat_active;
	boole is_logging_of_failed_attempts_active;
	uint64 chat_cooldown_milliseconds;
	uint64 join_channel_request_cooldown_milliseconds;
	uint64 delete_channel_request_cooldown_milliseconds;
	uint64 create_channel_request_cooldown_milliseconds;
	int channel_count;
	int max_channel_count;
	int client_count;
	int max_client_count;
	int keys_count;
	int websocket_port;
	int websocket_message_max_length;
	int websocket_chat_message_string_max_length;
	key_data_t keys[100];
	//char client_verificaton_message_cleartext[1024]; //not used right now, but it was supposed to be welcome message that server sends if somebody joins
	char default_client_name[30];
	char admin_password[ADMIN_PASSWORD_MAX_LENGTH];
} server_settings_t;

typedef enum audio_state_e
{
	AUDIO_STATE__PUSH_TO_TALK_ACTIVE = 1,
	AUDIO_STATE__PUSH_TO_TALK_ENABLED = 2,
	AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS = 3,
	AUDIO_STATE__AUDIO_COMPLETELY_DISABLED = 4
} audio_state_e;

//this is enum only sent from client, client uses these values
typedef enum microphone_usage_e
{
	MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO = 1,
	MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO = 2,
	MICROPHONE_USAGE__DISABLE_PUSH_TO_TALk = 3,
} microphone_usage_e;

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 1025
#endif

//for now the music_bot_client_extension_t struct will be part of each client_t struct, no matter if client is actually a music bot or not
//its done for simplicity, so the code is less prone to errors
//Few more bytes of memory that are wasted are negligible in todays world where js bloat wastes hundres of mb of memory
//but not song data itself, songs wont be part of client directly, they will be allocated as needed on heap

typedef struct music_bot_single_song_data_t
{
	boole is_existing;
	uint64 length_seconds;
	char song_name[SONG_NAME_MAX_LENGTH];
	boole is_this_song_scheduled_for_deletion;
	boole is_currently_playing;
	//mp3 data buffer stored on heap, not pcm, not base64, not opus, just mp3 buffer as-is.
	//for purposes of testing, simply load mp3 file to buffer from file system
	//plan is to send it to server by chunks as base64, then decode that base64 to buffer stored on heap
	ubyte *mp3_data_buffer;
	uint64 mp3_data_buffer_length;
} music_bot_single_song_data_t;

typedef struct music_bot_client_extension_t
{
	uint64 music_bot_pthread_handle;
	boole is_music_bot_running;
	uint64 music_bot_songs_count;
	music_bot_single_song_data_t songs[MUSIC_BOT_MAX_FILE_COUNT];
} music_bot_client_extension_t;

typedef struct client_file_upload_extension_t
{
	boole is_file_being_uploaded;
	ubyte *file_upload_buffer; //stores base64 content of the file
	uint64 buffer_cursor;
	uint64 expected_file_length;
} client_file_upload_extension_t;

typedef struct client_t
{
	ws_cli_conn_t *p_ws_connection;
	boole is_existing;
	boole is_authenticated;
	boole is_admin;
	boole is_audio_websocket_authenticated;
	boole is_dh_shared_secret_agreed_upon;
	boole is_streaming_song;
	boole is_public_key_challenge_sent;
	boole is_idle;
	boole is_music_bot;
	int client_id; //client id is same as index of client_t in clients_array
	int channel_id;
	int audio_state; //1 -> active, 2 -> not active bud enabled, 3 -> disabled audio still active, 4 audio disabled
	uint64 timestamp_connected;
	uint64 timestamp_last_action;
	uint64 timestamp_last_maintain_connection_message_received;
	char username[USERNAME_MAX_LENGTH];
	char public_key[MAX_PUBLIC_KEY_LENGTH];
	char dh_shared_secret[SHARED_SECRET_LENGTH];
	char challenge_string[CHALLENGE_STRING_LENGTH];
	char song_name[SONG_NAME_MAX_LENGTH];
	char ip_address[INET6_ADDRSTRLEN]; //max size of ivp6
	char country_iso_code[COUNTRY_ISO_CODE_LENGTH];
	int *tag_ids; //must be int because function of other library depends on this being int
	//int tag_ids_count;
	music_bot_client_extension_t music_bot_client_extension;
	client_file_upload_extension_t file_upload_extension;
} client_t;

//channel id is same as channels index in array
typedef struct channel
{
	boole is_existing;
	boole is_channel_maintainer_present;
	boole is_using_password;
	boole is_audio_enabled;
	boole is_music_bot_active_in_channel;
	int channel_id;
	int parent_channel_id;
	int current_clients;
	int max_clients;
	int type;
	int maintainer_id;
	char name[CHANNEL_NAME_MAX_LENGTH];
	char password[CHANNEL_PASSWORD_MAX_LENGTH];
	char description[CHANNEL_DESCRIPTION_MAX_LENGTH];
} channel_t;

typedef struct message
{
	uint64 timestamp_sent;
	int id_sender;
} message_t;

typedef struct chat_message_entry_t
{
	uint64 messsage_id;
	int message_type;
	uint64 receiver_id;
} chat_message_entry_t;

typedef struct tag_t
{
	boole is_existing;
	int id;
	int icon_id;
	char name[TAG_MAX_NAME_LENGTH];
} tag_t;

typedef struct icon_t
{
	boole is_existing;
	int id;
	char base64[ICON_MAX_LENGTH]; //hadam bude stacit
} icon_t;

//
//if someone wishes to use just chat without tags make it possible to disable it
//

//
//data of clients are linked to public keys..
//just some metadata of clients, but right now this struct is not used
//

typedef struct client_stored_data_t
{
	char public_key[MAX_PUBLIC_KEY_LENGTH];
	uint64 tag_ids[MAX_TAGS_FOR_SINGLE_CLIENT];
	uint64 tag_id_count;
	char username[USERNAME_MAX_LENGTH];
	char base64_avatar[MAX_CLIENT_AVATAR_LENGTH];
} client_stored_data_t;

int base__get_new_index_for_client(void);
void base__process_authenticated_client_message(ws_cli_conn_t *websocket, int client_index, char *decrypted_metadata_cstring);
void base__process_not_authenticated_client_message(ws_cli_conn_t *websocket, int index, char *decrypted_metadata_cstring);

//used for encrypting and decrypting metadata
char *base__encrypt_cstring_and_convert_to_base64(char *string, int *out_allocated_buffer_size, char *dh_shared_secret);
void base__get_data_from_base64_and_decrypt_it(int client_id, char *base64_string, unsigned char *out_buffer, int out_buffer_length);
void base__free_json_message(cJSON *json_root_object1, char *json_root_object1_string);
int base__get_client_count_for_channel(int channel_id);
char *base__encrypt_string_with_public_key(char *public_key_modulus, unsigned char *bytes, uint64 buffer_length);
void base__fill_block_of_data_with_ascii_characters(char *block, int length);
boole base__is_there_a_client_with_same_public_key(cstring public_key);
boole base__assign_username_for_newly_joined_client(int client_index, cstring default_name);
boole base__is_public_key_present_in_client_stored_data(char *public_key);
void base__close_websocket_connection(int client_index, boole use_readlock);
uint64 base__get_timestamp_ms(void);
void base__sleep_for_milliseconds(int milliseconds);
uint64 base__get_chat_message_id(void);
void base__increment_chat_message_id(void);
boole base__is_request_allowed_based_on_spam_protection(int client_index);
boole base__find_new_maintainer_for_channel(int *_out__new_index_of_maintainer, int channel_id, int index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer);
boole base__is_there_a_client_with_same_ip_address(cstring ip_address);
void base__mark_channels_for_deletion(int channel_id, int *current_index, int *channel_indices);
void base__set_server_settings(void);
void base__init_channel_list(void);
void base__init_tags_and_icons(void);
boole base__is_tag_id_real(int tag_id);
boole base__is_client_already_assigned_this_tag_id(int client_id, int this_tag_id);
int base__get_index_of_tag_id_of_client(int client_id, int this_tag_id);
client_t *base__find_music_bot_in_channel(int channel_id);
void base__process_client_disconnect(int client_index);

void webrtc_thread(void);

extern client_t *clients_array;
extern channel_t *channel_array;
extern icon_t *icons_array;
extern tag_t *tags_array;

extern server_settings_t g_server_settings;
extern pthread_rwlock_t clients_global_rwlock_guard;
extern pthread_rwlock_t channels_global_rwlock_guard;
extern pthread_rwlock_t icons_global_rwlock_guard;
extern pthread_rwlock_t tags_global_rwlock_guard;

extern pthread_mutex_t chat_message_id_mutex;
extern pthread_rwlock_t webrtc_muggles_rwlock_guard;

#endif
