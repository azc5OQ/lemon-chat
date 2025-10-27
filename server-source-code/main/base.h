#ifndef BASE_H
#define BASE_H

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
#define CHANNEL_NAME_MAX_LENGTH 128
#define SONG_NAME_MAX_LENGTH 512
#define TAG_MAX_NAME_LENGTH 32
#define ICON_MAX_LENGTH 8192
#define MAX_TAGS_PER_USER 32
#define ADMIN_TAG_ID 0

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

//
//todo, create memory manager to monitor allocations
//

typedef struct server_settings
{
	int keys_count;
	key_data_t keys[100];
	boole is_logging_of_failed_attempts_active;
	char client_verificaton_message_cleartext[1024]; //should be enough
	int websocket_port;
	int websocket_message_max_length;
	int websocket_chat_message_string_max_length;
	unsigned long long chat_cooldown_milliseconds;
	unsigned long long join_channel_request_cooldown_milliseconds;
	unsigned long long delete_channel_request_cooldown_milliseconds;
	unsigned long long create_channel_request_cooldown_milliseconds;
	char default_client_name[30];
	char admin_password[50];
	boole is_voice_chat_active;
	int channel_count;
	int max_channel_count;
	int client_count;
	int max_client_count;
	boole is_same_ip_address_allowed;
	boole is_hide_clients_in_password_protected_channels_active;
	boole is_restrict_channel_deletion_creation_editing_to_admin_active;
	boole is_display_country_flags_active;
	boole is_display_admin_tag_active;
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

//client id is same as clients index in array
typedef struct client_t
{
	int client_id;
	boole is_existing;
	ws_cli_conn_t *p_ws_connection;
	int websocket_audio_fd;
	boole is_authenticated;
	boole is_admin;
	uint64 timestamp_connected;
	uint64 timestamp_last_action;
	uint64 timestamp_last_maintain_connection_message_received;
	char username[USERNAME_MAX_LENGTH];
	char public_key[1000];
	int channel_id;
	boole microphone_active; //delete, and rely on audio_state
	char websocket_audio_auth_string_base64[100];
	boole is_audio_websocket_authenticated;
	boole is_dh_shared_secret_agreed_upon;
	char dh_shared_secret[1000]; //stored on heap
	char challenge_string[128];
	boole is_public_key_challenge_sent;
	int audio_state; //1 -> active, 2 -> not active bud enabled, 3 -> disabled audio still active, 4 audio disabled
	boole is_streaming_song;
	char song_name[SONG_NAME_MAX_LENGTH];
	char ip_address[INET6_ADDRSTRLEN]; //max size of ivp6
	char country_iso_code[3];
	int *tag_ids; //must be int because function of other library depends on this being int
	//int tag_ids_count;
} client_t;

//channel id is same as channels index in array
typedef struct channel
{
	boole is_existing;
	boole is_channel_maintainer_present;
	int channel_id;
	int parent_channel_id;
	char name[CHANNEL_NAME_MAX_LENGTH];
	int current_clients;
	int max_clients;
	char password[CHANNEL_PASSWORD_MAX_LENGTH];
	char description[CHANNEL_DESCRIPTION_MAX_LENGTH];
	boole is_using_password;
	int type;
	int maintainer_id;
	boole is_audio_enabled;
	pthread_rwlock_t lock;
} channel_t;

typedef struct message
{
	unsigned long long timestamp_sent;
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
//

typedef struct client_stored_data_t
{
	char public_key[1000];
	uint64 tag_ids[30]; //nahradit za vec
	uint64 tag_id_count;
	char username[100];
	char base64_avatar[131072];
} client_stored_data_t;

void base__process_authenticated_client_message(ws_cli_conn_t *websocket, int client_index, char *decrypted_metadata_cstring);
void base__process_not_authenticated_client_message(ws_cli_conn_t *websocket, int index, char *decrypted_metadata_cstring);

//used for encrypting and decrypting metadata
char *base__encrypt_cstring_and_convert_to_base64(char *string, int *out_allocated_buffer_size, char *dh_shared_secret);
void base__get_data_from_base64_and_decrypt_it(int client_id, char *base64_string, unsigned char *out_buffer, int out_buffer_length);

void base__free_json_message(cJSON *json_root_object1, char *json_root_object1_string);

//type
//0 -> not set
//1 - > sdp_answer from peer
//2 -> ice_message from peer
//3 -> new peer connection creation
//4 -> delete peer

typedef struct cross_thread_message
{
	int type;
	int client_id;
	char data[2048];
} cross_thread_message_t;

int base__get_client_count_for_channel(int channel_id);
char *base__encrypt_string_with_public_key(char *public_key_modulus, unsigned char *bytes, uint64 buffer_length);
void base__fill_block_of_data_with_ascii_characters(char *block, int length);
boole base__is_there_a_client_with_same_public_key(cstring public_key);
boole base__assign_username_for_newly_joined_client(int client_index, cstring default_name);
boole base__is_public_key_present_in_client_stored_data(char *public_key);
void base__close_websocket_connection(int client_index, boole use_readlock);
uint64 base__get_timestamp_ms(void);
uint64 base___get_chat_message_id(void);
void base___increment_chat_message_id(void);
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
