#ifndef MYTYPEDEF_H
#define MYTYPEDEF_H

// #define DEBUG_ACTIVE 1

#define ARCHITECTURE_AMD64 1
// #define ARCHITECTURE_I386 1

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

typedef signed char boole; // 1 byte always (George Boole)
typedef unsigned char ubyte; // 1 byte always
typedef unsigned int uint; // 4 bytes always
typedef unsigned short ushort; // 2 bytes always
typedef unsigned long long uint64; // 8 bytes always+
typedef unsigned long long nuint; // size of an address, native unsigned integer (memory manager / raw addresses only)
typedef signed long long int64; // 8 bytes always
typedef unsigned long long timestamp; // 8 bytes always
typedef const char* cstring;
// typedef wchar_t* wstring;

#ifdef DEBUG_ACTIVE
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
#define DBG_IDENTITIES if (0)

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
// force-on even in a release build: identity-restore debugging (flip to if (0) when done)
#define DBG_IDENTITIES if (0)
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
#define MAX_PUBLIC_KEY_LENGTH 1400 // base64 of an 8192-bit modulus is 1368 chars; smaller values truncated 6144+ bit keys
#define PUBLIC_KEY_STRING_MIN_LENGTH 344   // base64 of a 2048-bit modulus, the smallest key any server accepts
#define PUBLIC_KEY_STRING_MAX_LENGTH 1368  // base64 of an 8192-bit modulus, the largest a client can generate
#define CHANNEL_NAME_MAX_LENGTH 128
#define SONG_NAME_MAX_LENGTH 512
#define TAG_MAX_NAME_LENGTH 32
#define ICON_MAX_LENGTH 8192
#define SHARED_SECRET_LENGTH 3000 // a DH value (shared secret or public mix) is < the modulus; an 8192-bit modulus is 2467 decimal digits, so this must exceed ~2468
#define UNAUTH_HANDSHAKE_MAX_LENGTH (SHARED_SECRET_LENGTH + MAX_PUBLIC_KEY_LENGTH + 256)  // public_key_info = DH public mix (< SHARED_SECRET_LENGTH) + RSA public key (< MAX_PUBLIC_KEY_LENGTH) + JSON scaffolding
#define MAX_TAGS_PER_USER 32
#define ADMIN_TAG_ID 0
#define CHALLENGE_STRING_LENGTH 128
#define ADMIN_PASSWORD_MAX_LENGTH 50
#define COUNTRY_ISO_CODE_LENGTH 3
#define MAX_TAGS_FOR_SINGLE_CLIENT 30

// offline messages: text sent to a REGISTERED identity that is not connected right now. the server
// holds them in ram only (never in server_settings.json) and hands them over when that identity
// comes back, so a restart drops whatever is waiting - that is by design. see allow_offline_messages
#define MAX_OFFLINE_MESSAGES 1000  // server-wide ceiling on queued messages
#define MAX_OFFLINE_MESSAGES_PER_IDENTITY 50  // so one sender cannot fill the whole queue
#define MAX_OFFLINE_MESSAGE_LENGTH 8192  // base64 ciphertext cap for ONE queued message
#define IDENTITY_HASH_MAX_LENGTH 64  // base64 of a 32 byte hash is 44 chars + null; 64 is room to spare
// fits a 300 KB raw image as base64 (~410 KB) plus data-url prefix headroom. note the identity
// store is a static array of MAX_CLIENT_STORED_DATA entries carrying this buffer inline, so this
// costs ~42 MB of static memory at 100 slots - deliberate, avatars persist with identities
#define MAX_CLIENT_AVATAR_LENGTH 420000
#define MUSIC_BOT_MAX_FILE_COUNT 200

// debug aid: allow creating several music bots in one channel
// #define MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL 1

// debug aid: assign each connecting client a random real ISO country code instead of doing the GeoIP
// #define DEBUG_ASSIGN_RANDOM_COUNTRY_CODE 1
#define MAX_CLIENT_FILE_UPLOAD_LENGTH 14400000 // must exceed the base64 of the client musicbot gate: 10*1024*1024 raw -> ~13,981,016 base64 chars. the /400 per-part cap (36000) then exceeds the client's ceil(total/400) part size (~34953), so a full 10MiB upload is not rejected
// the encrypted name/size/mime block a chat file carries next to its body; opaque to the server
#define FILE_HEADER_MAX_LENGTH 8192
// raw-byte bounds for the admin's chat file size limit
#define FILE_UPLOAD_MIN_SIZE_BYTES (1024 * 1024)
#define FILE_UPLOAD_MAX_SIZE_BYTES (100 * 1024 * 1024)
#define FILE_UPLOAD_DEFAULT_SIZE_BYTES (10 * 1024 * 1024)
// raw-byte bounds for the admin's inline chat picture limit; the upload gate grows with it
#define PICTURE_MIN_SIZE_BYTES (1024 * 1024)
#define PICTURE_MAX_SIZE_BYTES (15 * 1024 * 1024)
#define PICTURE_DEFAULT_SIZE_BYTES (4 * 1024 * 1024)
// how many countries the join block list can hold (there are ~250 iso codes in total)
#define MAX_BLOCKED_COUNTRIES 100
// the admin log: ram-only dated one-line events, never written to disk (the lines carry ip
// addresses). capped by total size, entries older than the retention period are purged daily
#define ADMIN_LOG_ENTRY_MAX_LENGTH 256
#define ADMIN_LOG_MIN_SIZE_BYTES (1024 * 1024)
#define ADMIN_LOG_MAX_SIZE_BYTES (100 * 1024 * 1024)
#define ADMIN_LOG_DEFAULT_SIZE_BYTES (10 * 1024 * 1024)
#define ADMIN_LOG_MAX_RETENTION_DAYS 30
#define ADMIN_LOG_DEFAULT_RETENTION_DAYS 7
#define MAX_SIMULTANEOUS_FILE_SEND_THREADS 20
#define CHALLENGE_STRING_SIZE 100

#include "../third-party/theldus-websocket/include/ws.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "clib/clib_rwlock.h"

#ifndef WIN32
#include <sys/time.h>   // struct timeval + gettimeofday, used by base__get_timestamp_ms; POSIX (Linux + macOS + BSD), absent on Windows
#endif

typedef struct key_data
{
    int64 key_data_type;
    unsigned char key_value[32];
} key_data_t;

// todo, use int64 whenever possible
typedef struct server_settings
{
    boole is_same_ip_address_allowed;
    boole is_hide_clients_in_password_protected_channels_active;
    boole is_restrict_channel_deletion_creation_editing_to_admin_active;
    boole is_temp_channel_creation_allowed;
    boole is_display_country_flags_active;
    boole hide_admin_country_flag;     // admins are listed without a flag while flags are on (a comfort setting)
    boole is_display_admin_tag_active;
    boole is_idle_mode_allowed;
    boole is_fast_reconnect_allowed;   // a returning identity may adopt its still-open session instead of replacing it
    boole is_identity_takeover_allowed; // a new login with an identity already online replaces that session (off: refused)
    boole is_websocket_ping_active;    // ws-level ping every check tick; 4 unanswered pings shut the socket
    boole is_voice_chat_active;
    boole is_music_bot_audio_active;
    boole is_logging_of_failed_attempts_active;
    boole are_identities_enabled;
    boole persist_identity_in_localstorage;  // bake a flag into the served client so it saves/restores the identity passphrase in localStorage; default off
    boole allow_avatars;  // let each user set an image avatar persisted with their identity; default off
    boole allow_alias_registrations;  // admins may register an alias (display name) on a user's identity; needs identities; default off
    boole allow_stored_clients_list;  // users may fetch the stored identities (alias/avatar/tags only) to list offline people; needs identities; default off
    boole allow_last_seen;  // record and serve when an identity was last connected, so clients can show "last seen"; needs identities; default off
    boole allow_offline_messages;  // queue text messages for registered identities that are offline and deliver them on reconnect. asked ONCE at first-time setup and never editable afterwards, because switching it on makes the server retain each identity's RAW public key (peers need it to encrypt while the owner is away). needs identities + allow_stored_clients_list; default off
    boole allow_typing_indicator;  // clients may tell the people they are writing to that they are typing ("x is typing ..."). carries no message content, only who is typing and where; editable in the server settings tab; default off
    boole allow_client_renames;  // users may rename themselves after connecting; when off a rename request is silently ignored, because the switch is meant against name games - an admin still renames freely; default on
    int64 avatar_max_size_bytes;  // largest accepted raw image size (bytes) for an avatar; larger uploads are silently dropped
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
    // char client_verificaton_message_cleartext[1024]; not used right now, but it was supposed to be a welcome message that the server sends when somebody joins
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
    boole is_sending_text_to_idle_clients_allowed;  // deliver direct text chat to idle clients; their websocket stays open in idle, so delivery works; default on
    boole allow_private_messages;  // clients may send direct text chat to each other at all; off makes this a channels-only server; default on
    boole allow_file_uploads;  // any file may be sent in chat, not just pictures; relayed through ram only, never written to disk; default off
    int64 file_upload_max_size_bytes;  // largest accepted raw file size for a chat file; the server holds ~2x this in ram per transfer
    int64 chat_picture_max_size_bytes;  // largest raw picture clients may send inline; pictures are e2e encrypted, so clients enforce it (the upload gate is the hard bound)
    boole allow_chat_pictures;  // inline chat pictures; on by default, never a setup question, the upload intents enforce it
    boole is_country_blocking_active;  // refuse joins whose ip resolves to a listed country (per connection, works with flags off); set in the settings tab, never in setup; default off
    int64 minimum_rsa_key_bits;  // weakest client rsa key accepted, 2048..8192; a modulus may come out one bit under its nominal size, so the check allows minimum-1. weaker clients are dropped silently
    boole announce_minimum_rsa_key_bits;  // tell a rejected client which size is required so it can offer to regenerate. off keeps the requirement secret and the drop silent; default off
    char blocked_countries[MAX_BLOCKED_COUNTRIES][COUNTRY_ISO_CODE_LENGTH];  // uppercase iso 3166-1 alpha-2, the form the geoip db emits
    uint64 blocked_countries_count;
    boole log_client_joins;  // admin log: record each completed join with username and ip; default off
    boole log_username_changes;  // admin log: record renames (old -> new); default off
    boole log_tag_changes;  // admin log: record tags being added to clients; default off
    boole log_server_settings_updates;  // admin log: record who saved the server settings; default off
    boole log_kicks_and_bans;  // admin log: record kicks and bans (who did it, to whom); default off
    boole log_client_disconnects;  // admin log: record authenticated clients disconnecting; default off
    boole log_failed_attempts;  // admin log: record refused joins (wrong key, banned ip or identity, country, same ip) and wrong admin passwords; default off
    int64 admin_log_max_size_bytes;  // ram cap of the admin log; oldest entries fall out over it
    int64 admin_log_retention_days;  // admin log entries older than this are purged once a day
} server_settings_t;

typedef enum audio_state_e
{
    AUDIO_STATE__PUSH_TO_TALK_ACTIVE = 1,
    AUDIO_STATE__PUSH_TO_TALK_ENABLED = 2,
    AUDIO_STATE__PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS = 3,
    AUDIO_STATE__AUDIO_COMPLETELY_DISABLED = 4
} audio_state_e;

// this enum is only sent from the client; the client uses these values
typedef enum microphone_usage_e
{
    MICROPHONE_USAGE__ACTIVATE_PUSH_TO_TALK_AND_SEND_AUDIO = 1,
    MICROPHONE_USAGE__KEEP_PUSH_TO_TALK_READY_BUT_DONT_SEND_AUDIO = 2,
    MICROPHONE_USAGE__DISABLE_PUSH_TO_TALK = 3,
} microphone_usage_e;

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 1025
#endif


typedef struct music_bot_single_song_data_t
{
    boole is_existing;
    boole is_being_uploaded;
    uint64 length_seconds;
    char song_name[SONG_NAME_MAX_LENGTH];
    boole is_this_song_scheduled_for_deletion;
    boole is_currently_playing;
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
    ubyte* file_upload_buffer; // stores the base64 content of the file
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
    boole is_temp_admin_channel; // TRUE if this client owns a temp channel
    uint64 client_id; // client id is the same as the index of the client_t in clients_array
    uint64 channel_id;
    uint64 temp_channel_id; // if is_temp_admin_channel, the id of the temp channel this client owns
    int64 audio_state; // 1 -> active, 2 -> not active but enabled, 3 -> disabled but audio still active, 4 -> audio disabled
    uint64 timestamp_connected;
    uint64 timestamp_last_action;
    uint64 timestamp_last_maintain_connection_message_received;
    boole has_pending_maintainer_reset_vote;
    uint64 maintainer_reset_vote_channel_id;
    uint64 maintainer_reset_vote_generation;
    char username[USERNAME_MAX_LENGTH];
    char alias[USERNAME_MAX_LENGTH];  // admin-registered display name restored from the identity store; empty = none
    boole is_registered;  // the admin has registered this identity (it carries an alias); only such users may list the stored clients
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    char dh_shared_secret[SHARED_SECRET_LENGTH];
    char challenge_string[CHALLENGE_STRING_LENGTH];
    char song_name[SONG_NAME_MAX_LENGTH];
    char ip_address[INET6_ADDRSTRLEN]; // max size of an ipv6 address
    char country_iso_code[COUNTRY_ISO_CODE_LENGTH];
    char* base64_avatar;  // heap-allocated on demand (MEMALLOC_AVATAR), NULL when none; the live avatar served to others. persistent copy lives in the identity store
    int* tag_ids; // must be int because a function of another library depends on this being int
    // int tag_ids_count;
    music_bot_client_extension_t music_bot_client_extension;
    client_file_upload_extension_t file_upload_extension;
} client_t;

// channel id is the same as the channel's index in the array
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
    // bumped on EVERY maintainer state change; a reset vote carries the generation it complains
    // about, so a vote fired against a previous maintainer never counts against the new one
    uint64 maintainer_generation;
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
    boole has_icon; // a tag may exist without a linked icon; icon_id is only meaningful when this is TRUE
    char name[TAG_MAX_NAME_LENGTH];
} tag_t;

// one persisted ban. matching is by ip address; the rest (country/identity/extra data) is recorded for the admin
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

// if someone wishes to use just chat without tags, make it possible to disable it

// data of clients are linked to public keys.
// just some metadata of clients, but this struct is not used right now
typedef struct client_stored_data_t
{
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    uint64 tag_ids[MAX_TAGS_FOR_SINGLE_CLIENT];
    uint64 tag_id_count;
    char username[USERNAME_MAX_LENGTH];
    char base64_avatar[MAX_CLIENT_AVATAR_LENGTH];
    char alias[USERNAME_MAX_LENGTH];  // admin-registered display name for this identity; empty = none
    int64 last_seen_unix_seconds;  // when this identity was last connected; 0 = never recorded. only filled/served when allow_last_seen is on
    char raw_public_key[MAX_PUBLIC_KEY_LENGTH];  // the ACTUAL rsa public key, not the hash in public_key above. only collected, persisted and served while allow_offline_messages is on - it is what lets a peer encrypt a message to this identity while it is offline. empty = not known (yet)
} client_stored_data_t;

// one text message waiting for a registered identity to come back. the payload is opaque to the
// server: the sender encrypted it with the RECIPIENT's public key exactly like a normal direct
// message, so queueing never gives the server anything readable
typedef struct offline_chat_message_t
{
    boole is_used;
    char recipient_identity_hash[IDENTITY_HASH_MAX_LENGTH];  // routing key is the IDENTITY, never the alias - an admin may move an alias to another identity while this message waits
    char sender_identity_hash[IDENTITY_HASH_MAX_LENGTH];
    char sender_alias[USERNAME_MAX_LENGTH];  // what the recipient sees it came from
    char* base64_encrypted_message;  // heap, MEMALLOC_OFFLINE_MESSAGE
    uint64 message_length;
    int64 queued_unix_seconds;
    uint64 sequence_number;                                 // delivery order, and lets a client drop duplicates
} offline_chat_message_t;

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
    MEMALLOC_FILE_DOWNLOAD_BY_PARTS,
    MEMALLOC_AVATAR,
    MEMALLOC_OFFLINE_MESSAGE,
    MEMALLOC_OFFLINE_MESSAGES_ARRAY,
    MEMALLOC_ADMIN_LOG
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
    uint64 receiving_client_ids[MAX_CLIENTS];
    uint64 receiving_clients_count;
    uint64 client_receiver_id; // in case it is sent to a single client
    char* buffer; // this is the buffer that must be split into parts
    uint64 size;
    uint64 client_sender_id;
    uint64 server_chat_message_id;
    uint64 local_chat_message_id;
    char receive_type[32]; // what the receivers are told arrived: direct/channel chat picture or file
} data_for_file_send_thread_t;

#endif
