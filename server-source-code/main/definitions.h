#ifndef MYTYPEDEF_H
#define MYTYPEDEF_H

//#define DEBUG_ACTIVE 1

#define ARCHITECTURE_AMD64 1
/* #define ARCHITECTURE_I386 1 */

#ifndef MYTYPEDEF_DEFINITIONS
#define MYTYPEDEF_DEFINITIONS 1

#define FALSE 0
#define TRUE 1
#define NULL_POINTER 0
#define SEEK_END 2
#define SEEK_SET 0

#endif

#define MATH_PI 3.141592f

typedef union safe_byte_t
{
    signed char safe1;
    unsigned char saf2;
} safe_byte_t;

typedef signed char boole; /* 1 byte always (George Boole) */
typedef unsigned char ubyte; /* 1 byte always */
typedef unsigned int uint; /* 4 bytes always */
typedef unsigned short ushort; /* 2 bytes always */
typedef unsigned long long uint64; /* 8 bytes always */
typedef unsigned long long nuint; /* size of an address, native unsigned integer (memory manager / raw addresses only) */
typedef signed long long int64; /* 8 bytes always */
typedef unsigned long long timestamp; /* 8 bytes always */
typedef const char* cstring;
/* typedef wchar_t* wstring; */

#ifdef DEBUG_ACTIVE
#define DBG_DLLMAIN if (1)
#define DBG_CLIENT_MESSAGE if (0)
#define DBG_CLIENT_MESSAGE_MAIN_FUNCTION if (0)
#define DBG_AUTHENTICATION if (0)
#define DBG_ENCRYPTION if (0)
#define DBG_SERVER_MESSAGE if (0)
#define DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE if (1)
#define DBG_CLOSE_CONNECTION if (0)
#define DBG_ONMESSAGE if (0)
#define DBG_MEMORY_MANAGER if (0)
#define DBG_CONNECTION_CHECK_THREAD if (0)
#define DBG_CLIENT_DISCONNECT if (0)
#define DBG_AUDIOCHANNEL_WEBRTC if (0)
#define DBG_VIOLET if (0)
#define DBG_DBG_MEMORY_ALLOCATIONS if (0)
#define DBG_IP_TOOLS if (0)
#define DBG_FILE_UPLOAD if (0)
#define DBG_MUSIC_BOT if (0)
#define DBG_RWLOCKS if (0)

#endif

#ifndef DEBUG_ACTIVE
#define DBG_DLLMAIN if (0)
#define DBG_CLIENT_MESSAGE if (0)
#define DBG_CLIENT_MESSAGE_MAIN_FUNCTION if (0)
#define DBG_AUTHENTICATION if (0)
#define DBG_ENCRYPTION if (0)
#define DBG_SERVER_MESSAGE if (0)
#define DBG_SERVER_MESSAGE_HIGH_LVL_PERSPECTIVE if (0)
#define DBG_CLOSE_CONNECTION if (0)
#define DBG_ONMESSAGE if (0)
#define DBG_MEMORY_MANAGER if (0)
#define DBG_CONNECTION_CHECK_THREAD if (0)
#define DBG_CLIENT_DISCONNECT if (0)
#define DBG_AUDIOCHANNEL_WEBRTC if (0)
#define DBG_VIOLET if (0)
#define DBG_DBG_MEMORY_ALLOCATIONS if (0)
#define DBG_IP_TOOLS if (0)
#define DBG_FILE_UPLOAD if (0)
#define DBG_MUSIC_BOT if (0)
#define DBG_RWLOCKS if (0)
#endif

#ifdef DONT_USE_AUDIO_CHANNEL
#define AUDIO_CHANNEL_CHECK_IF_RETURN_NEEDED if (1)
#endif

#ifndef DONT_USE_AUDIO_CHANNEL
#define AUDIO_CHANNEL_CHECK_IF_RETURN_NEEDED if (0)
#endif

int mytypedef__check_data_types_for_consistency(void);

#define MAX_CHANNELS 100
#define MAX_CLIENTS 500
#define MAX_CLIENT_STORED_DATA 100
#define MAX_ICONS 1000
#define MAX_TAGS 1000
#define MAX_BANS 1024
#define BAN_IP_MAX_LENGTH 46
#define BAN_EXTRA_DATA_MAX_LENGTH 256
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
#define SHARED_SECRET_LENGTH 3000 /* a DH value (shared secret or public mix) is < the modulus; an 8192-bit modulus is 2467 decimal digits, so this must exceed ~2468 */
#define UNAUTH_HANDSHAKE_MAX_LENGTH (SHARED_SECRET_LENGTH + MAX_PUBLIC_KEY_LENGTH + 256) /* public_key_info = DH public mix (< SHARED_SECRET_LENGTH) + RSA public key (< MAX_PUBLIC_KEY_LENGTH) + JSON scaffolding */
#define MAX_TAGS_PER_USER 32
#define ADMIN_TAG_ID 0
#define CHALLENGE_STRING_LENGTH 128
#define ADMIN_PASSWORD_MAX_LENGTH 50
#define COUNTRY_ISO_CODE_LENGTH 3
#define MAX_TAGS_FOR_SINGLE_CLIENT 30
#define MAX_CLIENT_AVATAR_LENGTH 131072
#define MUSIC_BOT_MAX_FILE_COUNT 200

/* debug aid: allow creating several music bots in one channel (several simultaneous audio senders, for
   testing multi-speaker mixing without needing several people). keep commented out for normal builds -
   one bot per channel */
//#define MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL 1
#define MAX_CLIENT_FILE_UPLOAD_LENGTH 14400000 /* must exceed the base64 of the client musicbot gate: 10*1024*1024 raw -> ~13,981,016 base64 chars. the /400 per-part cap (36000) then exceeds the client's ceil(total/400) part size (~34953), so a full 10MiB upload is not rejected */
#define MAX_SIMULTANEOUS_FILE_SEND_THREADS 20
#define CHALLENGE_STRING_SIZE 100

#include "../third-party/theldus-websocket/include/ws.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "clib/clib_rwlock.h"

#ifdef __linux__
#include <sys/time.h>
#endif

typedef struct key_data
{
    int64 key_data_type;
    unsigned char key_value[32];
} key_data_t;

/* todo, use int64 whenever possible */
typedef struct server_settings
{
    boole is_same_ip_address_allowed;
    boole is_hide_clients_in_password_protected_channels_active;
    boole is_restrict_channel_deletion_creation_editing_to_admin_active;
    boole is_temp_channel_creation_allowed;
    boole is_display_country_flags_active;
    boole is_display_admin_tag_active;
    boole is_idle_mode_allowed;
    boole is_voice_chat_active;
    boole is_music_bot_audio_active;
    boole is_logging_of_failed_attempts_active;
    boole are_identities_enabled;
    uint64 chat_cooldown_milliseconds;
    uint64 join_channel_request_cooldown_milliseconds;
    uint64 delete_channel_request_cooldown_milliseconds;
    uint64 create_channel_request_cooldown_milliseconds;
    uint64 channel_count;
    uint64 max_channel_count;
    uint64 client_count;
    uint64 max_client_count;
    uint64 keys_count;
    int64 websocket_port;
    int64 websocket_message_max_length;
    int64 websocket_chat_message_string_max_length;
    key_data_t keys[100];
    /* char client_verificaton_message_cleartext[1024]; not used right now, but it was supposed to be a welcome message that the server sends when somebody joins */
    char default_client_name[30];
    char admin_password[ADMIN_PASSWORD_MAX_LENGTH];
    boole admin_password_is_initial;

    boole restart_on_crash;

    boole use_stunnel;
    int64 wss_port;
    char stunnel_domain[256];
    char stunnel_cert_fullchain[512];
    char stunnel_cert_privkey[512];
    char client_html_dest[512];

    boole serve_client_http;
    int64 http_port;
    char http_webroot[512];
    boole serve_https;
    int64 https_port;
    char default_theme[32];
    boole embed_client_config;
} server_settings_t;

typedef enum audio_state_e
{
    AUDIO_STATE__PUSH_TO_TALK_ACTIVE = 1,
    AUDIO_STATE__PUSH_TO_TALK_ENABLED = 2,
    AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS = 3,
    AUDIO_STATE__AUDIO_COMPLETELY_DISABLED = 4
} audio_state_e;

/* this enum is only sent from the client; the client uses these values */
typedef enum microphone_usage_e
{
    MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO = 1,
    MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO = 2,
    MICROPHONE_USAGE__DISABLE_PUSH_TO_TALK = 3,
} microphone_usage_e;

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 1025
#endif

/* for now the music_bot_client_extension_t struct will be part of each client_t struct, regardless of whether the client is actually a music bot or not.
   it is done for simplicity, so the code is less prone to errors.
   the few extra wasted bytes of memory are negligible in today's world where js bloat wastes hundreds of mb of memory.
   song data itself is not included; songs won't be part of the client directly, they will be allocated as needed on the heap. */
typedef struct music_bot_single_song_data_t
{
    boole is_existing;
    boole is_being_uploaded;
    uint64 length_seconds;
    char song_name[SONG_NAME_MAX_LENGTH];
    boole is_this_song_scheduled_for_deletion;
    boole is_currently_playing;
    /* mp3 data buffer stored on the heap, not pcm, not base64, not opus, just the mp3 buffer as-is.
       for testing purposes, simply load the mp3 file into the buffer from the file system.
       the plan is to send it to the server in chunks as base64, then decode that base64 into a buffer stored on the heap. */
    ubyte* mp3_data_buffer;
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
    /* file_upload_buffer is the single in-progress signal: non-NULL while a file is buffered, NULL otherwise */
    ubyte* file_upload_buffer; /* stores the base64 content of the file */
    uint64 buffer_cursor;
    uint64 expected_file_length;
} client_file_upload_extension_t;

typedef struct client_t
{
    ws_cli_conn_t* p_ws_connection;
    boole is_existing;
    boole is_authenticated;
    boole is_admin;
    boole is_audio_websocket_authenticated;
    boole is_dh_shared_secret_agreed_upon;
    boole is_streaming_song;
    boole is_public_key_challenge_sent;
    boole is_idle;
    boole is_music_bot;
    boole is_temp_admin_channel; /* TRUE if this client owns a temp channel */
    uint64 client_id; /* client id is the same as the index of the client_t in clients_array */
    uint64 channel_id;
    uint64 temp_channel_id; /* if is_temp_admin_channel, the id of the temp channel this client owns */
    int64 audio_state; /* 1 -> active, 2 -> not active but enabled, 3 -> disabled but audio still active, 4 -> audio disabled */
    uint64 timestamp_connected;
    uint64 timestamp_last_action;
    uint64 timestamp_last_maintain_connection_message_received;
    char username[USERNAME_MAX_LENGTH];
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    char dh_shared_secret[SHARED_SECRET_LENGTH];
    char challenge_string[CHALLENGE_STRING_LENGTH];
    char song_name[SONG_NAME_MAX_LENGTH];
    char ip_address[INET6_ADDRSTRLEN]; /* max size of an ipv6 address */
    char country_iso_code[COUNTRY_ISO_CODE_LENGTH];
    int* tag_ids; /* must be int because a function of another library depends on this being int */
    /* int tag_ids_count; */
    music_bot_client_extension_t music_bot_client_extension;
    client_file_upload_extension_t file_upload_extension;
} client_t;

/* channel id is the same as the channel's index in the array */
typedef struct channel
{
    boole is_existing;
    boole is_root_channel;
    boole is_channel_maintainer_present;
    boole is_using_password;
    boole is_audio_enabled;
    boole is_music_bot_active_in_channel;
    boole is_temp_channel;
    boole is_client_limit_active;
    uint64 channel_id;
    uint64 parent_channel_id;
    uint64 current_clients;
    uint64 max_client_count;
    uint64 type;
    uint64 maintainer_id;
    boole has_channel_icon;
    uint64 icon_id;
    char name[CHANNEL_NAME_MAX_LENGTH];
    char password[CHANNEL_PASSWORD_MAX_LENGTH];
    char description[CHANNEL_DESCRIPTION_MAX_LENGTH];
} channel_t;

typedef struct message
{
    uint64 timestamp_sent;
    uint64 id_sender;
} message_t;

typedef struct chat_message_entry_t
{
    uint64 messsage_id;
    uint64 message_type;
    uint64 receiver_id;
} chat_message_entry_t;

typedef struct tag_t
{
    boole is_existing;
    uint64 id;
    uint64 icon_id;
    boole has_icon; /* a tag may exist without a linked icon; icon_id is only meaningful when this is TRUE */
    char name[TAG_MAX_NAME_LENGTH];
} tag_t;

/* one persisted ban. matching is by ip address; the rest (country/identity/extra data) is recorded for the admin */
typedef struct ban_entry_t
{
    boole is_existing;
    uint64 timestamp_banned;
    char ip_address[BAN_IP_MAX_LENGTH];
    char country_iso_code[COUNTRY_ISO_CODE_LENGTH];
    char identity[MAX_PUBLIC_KEY_LENGTH];
    char extra_data[BAN_EXTRA_DATA_MAX_LENGTH];
} ban_entry_t;

typedef struct icon_t
{
    boole is_existing;
    uint64 id;
    char base64[ICON_MAX_LENGTH];
} icon_t;

/* if someone wishes to use just chat without tags, make it possible to disable it */

/* data of clients are linked to public keys.
   just some metadata of clients, but this struct is not used right now */
typedef struct client_stored_data_t
{
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    uint64 tag_ids[MAX_TAGS_FOR_SINGLE_CLIENT];
    uint64 tag_id_count;
    char username[USERNAME_MAX_LENGTH];
    char base64_avatar[MAX_CLIENT_AVATAR_LENGTH];
} client_stored_data_t;

typedef enum memory_manager_allocation_type_e
{
    MEMALLOC_TYPE_CJSON,
    MEMALLOC_TYPE_MAIN,
    MEMALLOC_TYPE_WEBSOCKET,
    MEMALLOC_TYPE_OTHER,
    MEMALLOC_TYPE_CHALLENGE,
    MEMALLOC_TYPE_ENCRYPT,
    MEMALLOC_TYPE_DECRYPT,
    MEMALLOC_MARKED_CHANNEL_INDICES,
    MEMALLOC_DHPROCESS,
    MEMALLOC_PUBLIC_KEY_ENCRYPT,
    MEMALLOC_CLIENTS_ARRAY,
    MEMALLOC_CHANNELS_ARRAY,
    MEMALLOC_CLIENT_STORED_DATA_ARRAY,
    MEMALLOC_BANS_ARRAY,
    MEMALLOC_FIND_MAINTAINER,
    MEMALLOC_MARKED_CLIENT_INDICES,
    MEMALLOC_OPUS_DATA_BUFFER_ENTRY,
    MEMALLOC_WEBRTC_PEERS,
    MEMALLOC_AUDIOCHANNEL_ONMESSAGE,
    MEMALLOC_MUSICBOT_AUDIOCHANNEL_ONMESSAGE,
    MEMALLOC_MUSICBOT_SONG,
    MEMALLOC_FILE_UPLOAD_BY_PARTS,
    MEMALLOC_FILE_UPLOAD_BY_PARTS1,
    MEMALLOC_FILE_UPLOAD_BY_PARTS2,
    MEMALLOC_FILE_DOWNLOAD_BY_PARTS
} memory_manager_allocation_type_e;

typedef struct webrtc_peer_t
{
    int peer_connection_handle;
    int data_channel_handle;
    boole connected;
    audio_state_e last_sent_audio_state;
    boole is_sending_audio_right_now;
    uint64 channel_id;
    uint64 client_id;
    boole is_existing;
    char dh_shared_secret[SHARED_SECRET_LENGTH];
    ws_cli_conn_t* p_ws_connection;
} webrtc_peer_t;

typedef enum file_send_type_e
{
    FILE_SEND_TYPE_TO_CLIENT,
    FILE_SEND_TYPE_TO_CHANNEL
} file_send_type_e;

typedef struct data_for_file_send_thread_t
{
    boole is_existing;
    file_send_type_e send_type;
    /* receiving client ids need to be examined from a historical perspective, not the current one,
       that is why they are in the array */
    uint64 receiving_client_ids[MAX_CLIENTS];
    uint64 receiving_clients_count;
    uint64 client_receiver_id; /* in case it is sent to a single client */
    char* buffer; /* this is the buffer that must be split into parts */
    uint64 size;
    uint64 client_sender_id;
    uint64 server_chat_message_id;
    uint64 local_chat_message_id;
} data_for_file_send_thread_t;

#endif
