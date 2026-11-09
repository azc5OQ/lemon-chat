#ifndef MYTYPEDEF_H
#define MYTYPEDEF_H

// Server-wide limits and data types shared by the main modules.

// Change this to #define DEBUG_ACTIVE to select the debug switches below.
#undef DEBUG_ACTIVE

// --- Includes ---

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#ifndef WIN32
// POSIX timestamps used by util__get_timestamp_ms.
#include <sys/time.h>
#endif

// --- Basic types ---

typedef signed char boole;
typedef unsigned char ubyte;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long long uint64;
typedef signed long long int64;
// Match the compiled target's address width.
#if defined(_WIN64) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
typedef unsigned long long nuint;
typedef signed long long nint;
#elif defined(_WIN32) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)
typedef unsigned int nuint;
typedef signed int nint;
#elif (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 2)
typedef unsigned short nuint;
typedef signed short nint;
#else
#error Unsupported target address width
#endif
typedef unsigned long long timestamp;
typedef const char *cstring;

// Wire integers and audio samples have the same widths on every supported target.
_Static_assert(CHAR_BIT == 8 && sizeof(ushort) == 2 && sizeof(short) == 2 && sizeof(uint) == 4 && sizeof(int) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "Unsupported integer widths");
_Static_assert(sizeof(nuint) == sizeof(void *) && sizeof(nint) == sizeof(void *), "Native integers must match pointer width");

typedef union safe_byte_t
{
    signed char safe1;
    ubyte saf2;
} safe_byte_t;

// --- Server dependencies ---

#include "../third-party/theldus-websocket/include/ws.h"
#include "clib/clib_rwlock.h"

// --- Basic values ---

#ifndef MYTYPEDEF_DEFINITIONS
#define MYTYPEDEF_DEFINITIONS 1

#define FALSE 0
#define TRUE 1
#define NULL_POINTER 0
#define SEEK_END 2
#define SEEK_SET 0

#endif

#define MATH_PI 3.141592f

// Application WebSocket close codes, also recognized by the clients.
#define SERVER_CLOSE_KICKED 4001
#define SERVER_CLOSE_BANNED 4002

// --- Debug and release switches ---

// These blocks are independent so debug and release can enable different logs.
// Each DBG_* macro is a guard; set it to if (1) to enable that log category.
#define DBG_CONNECTION_PROGRESS if (0)

#ifdef DEBUG_ACTIVE
// Debug build.
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
#define DBG_AUDIO_WEBRTC if (0)
#define DBG_VIOLET if (0)
#define DBG_DBG_MEMORY_ALLOCATIONS if (0)
#define DBG_IP_TOOLS if (0)
#define DBG_FILE_UPLOAD if (0)
#define DBG_MUSIC_BOT if (0)
#define DBG_RWLOCKS if (0)
#define DBG_IDENTITIES if (0)
#define DBG_VIDEO_CHANNEL if (0)
#define DBG_AUDIO_CHANNEL if (0)
#define DBG_AUDIO_WEBSOCKET if (0)
#define DBG_MEDIA_AUTH if (0)
#define DBG_VIDEO_WEBSOCKET if (0)
#define DBG_VIDEO_WEBRTC if (0)
#define DBG_HTTP_SERVER if (0)
#define DBG_SETTINGS if (0)
#define DBG_FIRST_TIME_SETUP if (0)
#define DBG_SERVER_LOGS if (0)
#define DBG_UTIL if (0)

#else
// Release build.
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
#define DBG_AUDIO_WEBRTC if (0)
#define DBG_VIOLET if (0)
#define DBG_DBG_MEMORY_ALLOCATIONS if (0)
#define DBG_IP_TOOLS if (0)
#define DBG_FILE_UPLOAD if (0)
#define DBG_MUSIC_BOT if (0)
#define DBG_RWLOCKS if (0)
#define DBG_IDENTITIES if (0)
#define DBG_VIDEO_CHANNEL if (0)
#define DBG_AUDIO_CHANNEL if (0)
#define DBG_AUDIO_WEBSOCKET if (0)
#define DBG_MEDIA_AUTH if (0)
#define DBG_VIDEO_WEBSOCKET if (0)
#define DBG_VIDEO_WEBRTC if (0)
#define DBG_HTTP_SERVER if (0)
#define DBG_SETTINGS if (0)
#define DBG_FIRST_TIME_SETUP if (0)
#define DBG_SERVER_LOGS if (0)
#define DBG_UTIL if (0)
#endif

#ifdef DONT_USE_AUDIO_CHANNEL
#define AUDIO_CHANNEL_CHECK_IF_RETURN_NEEDED if (0)
#else
#define AUDIO_CHANNEL_CHECK_IF_RETURN_NEEDED if (0)
#endif

// Optional debug aids.
// Allow several music bots in one channel.
// #define MUSICBOT_DEBUG_ALLOW_MULTIPLE_BOTS_PER_CHANNEL 1

// Assign a random real ISO country code instead of using GeoIP.
// #define DEBUG_ASSIGN_RANDOM_COUNTRY_CODE 1

// --- Server and client capacity ---

#define DEFAULT_SERVER_SLOTS 500
#define MAX_SERVER_SLOTS 100000
#define WEBSOCKET_CONNECTIONS_PER_CLIENT 3 // chat, audio and video may each have their own socket
#define USERNAME_MAX_LENGTH 100
#define TIMESTAMP_LAST_ACTION_COOLDOWN_MS 100

// --- Channels ---

#define DEFAULT_CHANNEL_COUNT 100
// Channel IDs travel through signed 32-bit JSON/client fields.
#define MAX_CHANNEL_COUNT INT_MAX
#define ROOT_CHANNEL_ID 0
#define CHANNEL_NAME_MAX_LENGTH 128
#define CHANNEL_PASSWORD_MAX_LENGTH 128
#define CHANNEL_DESCRIPTION_MAX_LENGTH 1000

// --- Stored identities and avatars ---

#define MAX_CLIENT_STORED_DATA 100
// base64 of a 32 byte hash is 44 chars + null; 64 is room to spare
#define IDENTITY_HASH_MAX_LENGTH 64
// Fits a 300 KB raw image as base64 (~410 KB) plus data-url prefix headroom. The identity
// store allocates MAX_CLIENT_STORED_DATA entries at startup, each with this buffer inline.
// At 100 slots the avatar buffers alone reserve ~42 MB; avatars persist with identities.
#define MAX_CLIENT_AVATAR_LENGTH 420000

// --- Connection keys and login ---

// base64 of an 8192-bit modulus is 1368 chars; smaller values truncated 6144+ bit keys
#define MAX_PUBLIC_KEY_LENGTH 1400
// base64 of a 2048-bit modulus, the smallest key any server accepts
#define PUBLIC_KEY_STRING_MIN_LENGTH 344
// base64 of an 8192-bit modulus, the largest a client can generate
#define PUBLIC_KEY_STRING_MAX_LENGTH 1368
// a DH value (shared secret or public mix) is < the modulus; an 8192-bit modulus is 2467 decimal
// digits, so this must exceed ~2468
#define SHARED_SECRET_LENGTH 3000
// Room for multiple DH public mixes, the RSA public key and the surrounding JSON.
#define UNAUTH_HANDSHAKE_MAX_LENGTH (5 * SHARED_SECRET_LENGTH + MAX_PUBLIC_KEY_LENGTH + 2048)
#define CHALLENGE_STRING_LENGTH 128
#define CHALLENGE_STRING_SIZE 100
#define ADMIN_PASSWORD_MAX_LENGTH 50

// --- Groups, bans and country rules ---

#define MAX_GROUPS 1000
#define MAX_GROUPS_FOR_SINGLE_CLIENT MAX_GROUPS
#define ADMIN_GROUP_ID 0
#define GROUP_MAX_NAME_LENGTH 32
#define MAX_BANS 1024
#define BAN_IP_MAX_LENGTH 46
#define BAN_EXTRA_DATA_MAX_LENGTH 256
#define IPV4_ADDRESS_STRING_LENGTH 16
#define COUNTRY_ISO_CODE_LENGTH 3
// how many countries the join block list can hold (there are ~250 iso codes in total)
#define MAX_BLOCKED_COUNTRIES 100

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 1025
#endif

// --- Icons, emojis and server banner ---

#define MAX_ICONS 1000
// base64 of a 30000-byte icon with its data-url prefix; the ceiling behind icon_max_size_bytes
#define ICON_MAX_LENGTH 40960
#define MAX_SERVER_EMOJIS 256
#define SERVER_EMOJI_MAX_BYTES 30000
#define SERVER_EMOJI_GIF_MAX_BYTES (256 * 1024)
#define SERVER_BANNER_MAX_BYTES 32768

// --- Offline messages ---

// offline messages: text sent to a REGISTERED identity that is not connected right now. the server
// holds them in ram only (never in server_settings.json) and hands them over when that identity
// comes back, so a restart drops whatever is waiting - that is by design. see
// is_offline_messaging_allowed
#define MAX_OFFLINE_MESSAGES 1000 // server-wide ceiling on queued messages
#define MAX_OFFLINE_MESSAGES_PER_IDENTITY 50 // so one sender cannot fill the whole queue
#define MAX_OFFLINE_MESSAGE_LENGTH 8192 // base64 ciphertext cap for ONE queued message

// --- Chat attachments ---

// must exceed the base64 of the client musicbot gate: 10*1024*1024 raw -> ~13,981,016 base64 chars.
// the /400 per-part cap (36000) then exceeds the client's ceil(total/400) part size (~34953), so a
// full 10MiB upload is not rejected
#define MAX_CLIENT_FILE_UPLOAD_LENGTH 14400000
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
#define MAX_SIMULTANEOUS_FILE_SEND_THREADS 20

// --- Audio and music bots ---

// datachannel attempts a client gets before the server stops building peers for it for a while
#define WEBRTC_DATACHANNEL_ATTEMPTS_BEFORE_COOLDOWN 10
// how far ahead of playback a music bot streams: sent as a burst at song start, then paced. the
// player worklet holds the same amount (BOT_TARGET in audio.js), keep the two in step
#define MUSIC_BOT_LEAD_MS 3000
#define MUSIC_BOT_MAX_FILE_COUNT 200
#define SONG_NAME_MAX_LENGTH 512

// --- Video streams ---

// Video uses a second, reliable and ordered data channel beside audio.
// A keyframe can span many SCTP fragments; losing one fragment loses the frame.
#define VIDEO_DATACHANNEL_LABEL "video"
// a viewer whose video channel has this much queued is skipped for the current frame instead of
// stalling everyone; it notices the gap and asks the streamer for a keyframe
#define VIDEO_RELAY_MAX_BUFFERED_BYTES (1024 * 1024)
// the streamer is asked for a keyframe at most this often, no matter how many viewers ask
#define VIDEO_KEYFRAME_REQUEST_MIN_INTERVAL_MS 1000
// what the streamer announces about its stream, echoed to viewers in the offer
#define VIDEO_STREAM_SOURCE_MAX_LENGTH 16 // "screen" or "file"
#define VIDEO_STREAM_CODEC_MAX_LENGTH 32 // a webcodecs codec string such as avc1.42E01E or vp8
#define VIDEO_STREAM_MAX_DIMENSION 4096
#define VIDEO_STREAM_MAX_FPS 60

// --- Admin log ---

// the admin log: ram-only dated one-line events, never written to disk (the lines carry ip
// addresses). capped by total size, entries older than the retention period are purged daily
#define ADMIN_LOG_ENTRY_MAX_LENGTH 256
#define ADMIN_LOG_MIN_SIZE_BYTES (1024 * 1024)
#define ADMIN_LOG_MAX_SIZE_BYTES (100 * 1024 * 1024)
#define ADMIN_LOG_DEFAULT_SIZE_BYTES (10 * 1024 * 1024)
#define ADMIN_LOG_MAX_RETENTION_DAYS 30
#define ADMIN_LOG_DEFAULT_RETENTION_DAYS 7

// --- Connection encryption ---

enum
{
    LEMON_AES256 = 1,
    LEMON_CHACHA20 = 3,
    LEMON_CAMELLIA256 = 4,
    LEMON_TWOFISH256 = 5,
    LEMON_SERPENT256 = 6,
    LEMON_TRIVIUM80 = 7
};

#define LEMON_MAX_DH_EXCHANGES 4
#define LEMON_MAX_SESSION_LAYERS 8
#define LEMON_MAX_CONNECTION_LAYERS (LEMON_MAX_SESSION_LAYERS + 1)

typedef struct lemon_session_policy
{
    uint64 exchanges;
    uint64 layers;
    int64 cipher;
    boole random_layer;
} lemon_session_policy;

typedef struct lemon_session_layer
{
    int64 type;
    ubyte bytes[32];
} lemon_session_layer;

typedef struct lemon_session_keys
{
    uint64 count;
    lemon_session_layer client_to_server[LEMON_MAX_CONNECTION_LAYERS];
    lemon_session_layer server_to_client[LEMON_MAX_CONNECTION_LAYERS];
    ubyte client_mac[32];
    ubyte server_mac[32];
} lemon_session_keys;

// Encryption key types use the cipher IDs above.
// HMAC-SHA256 is an authentication key type, not an encryption cipher.
typedef enum crypto_key_type
{
    CRYPTO_KEY_NONE = 0,
    CRYPTO_KEY_AES_256_CTR,
    CRYPTO_KEY_HMAC_SHA256,
    CRYPTO_KEY_CHACHA20 = LEMON_CHACHA20,
    CRYPTO_KEY_CAMELLIA_256_CTR = LEMON_CAMELLIA256,
    CRYPTO_KEY_TWOFISH_256_CTR = LEMON_TWOFISH256,
    CRYPTO_KEY_SERPENT_256_CTR = LEMON_SERPENT256,
    CRYPTO_KEY_TRIVIUM_80 = LEMON_TRIVIUM80
} crypto_key_type_t;

// The union names refer to the same 32 bytes, not separate keys.
typedef struct crypto_key
{
    crypto_key_type_t type;
    union
    {
        ubyte raw[32];
        ubyte aes_256_ctr[32];
        ubyte hmac_sha256[32];
    } material;
} crypto_key_t;

// Immutable connection-password snapshot, shared only with that socket's queued work.
typedef struct crypto_password_set
{
    atomic_uint_fast64_t references;
    uint64 count;
    crypto_key_t keys[];
} crypto_password_set_t;

typedef struct crypto_session_keys
{
    crypto_key_t encryption;
    crypto_key_t authentication;
    boole is_ready;
    lemon_session_keys negotiated; // count == 0 retains the legacy one-layer wire format
} crypto_session_keys_t;

typedef enum crypto_direction
{
    CRYPTO_ENCRYPT,
    CRYPTO_DECRYPT
} crypto_direction_t;

// --- Server settings ---

typedef struct server_settings
{
    // Connection and visibility switches.
    // Voice may use /audio; the admin can change this in "audio connections" at runtime.
    boole is_audio_over_websocket_allowed;

    // voice may use the webrtc datachannel; both on means the client picks by its own preference
    boole is_audio_over_webrtc_allowed;
    boole is_same_ip_address_allowed;
    boole is_hide_clients_in_password_protected_channels_active;
    boole is_restrict_channel_deletion_creation_editing_to_admin_active;
    boole is_temp_channel_creation_allowed;
    boole is_display_country_flags_active;

    // admins are listed without a flag while flags are on (a comfort setting)
    boole is_admin_country_flag_hidden;
    boole is_permission_system_enabled;
    boole is_display_admin_group_active;
    boole is_idle_mode_allowed;

    // a returning identity may adopt its still-open session instead of replacing it
    boole is_fast_reconnect_allowed;

    // a new login with an identity already online replaces that session (off: refused)
    boole is_identity_takeover_allowed;

    // ws-level ping every check tick; 4 unanswered pings shut the socket
    boole is_websocket_ping_active;
    boole is_connection_check_enforced;

    // Channel key reuse.
    boole is_channel_key_reuse_enabled;
    boole is_channel_key_reuse_on_join_allowed;
    boole is_channel_key_reuse_on_leave_allowed;
    int64 channel_key_reuse_join_min_clients;
    int64 channel_key_reuse_leave_min_clients;

    // Login admission and media features.
    int64 max_concurrent_logins;
    boole is_blocking_direct_chat_pictures_active;
    boole is_blocking_direct_chat_files_active;
    boole is_blocking_channel_chat_pictures_active;
    boole is_blocking_channel_chat_files_active;

    // no new peers for a client after 10 attempts that never connected
    int64 webrtc_datachannel_cooldown_seconds;

    // the "now playing" marquee also for people outside the bot's channel
    boole is_music_bot_marquee_visible_to_everyone;
    boole is_voice_chat_active;
    boole is_music_bot_audio_active;

    // clients may stream their screen or a video file to their channel; every channel also has its
    // own toggle. off by default and never a setup question, the admin switches it on in the server
    // settings tab; while off no client shows any stream ui
    boole is_video_streaming_active;
    boole is_logging_of_failed_attempts_active;

    // Saved identities and client features.
    boole is_identity_storage_enabled;

    // bake a flag into the served client so it saves/restores the identity passphrase in
    // localStorage; default off
    boole is_identity_localstorage_enabled;

    // let each user set an image avatar persisted with their identity; default off
    boole is_avatar_upload_allowed;
    boole is_stored_client_list_allowed;

    // record and serve when an identity was last connected, so clients can show "last seen"; needs
    // identities; default off
    boole is_last_seen_enabled;
    boole is_offline_messaging_allowed;

    // clients may tell the people they are writing to that they are typing ("x is typing ...").
    // carries no message content, only who is typing and where; editable in the server settings
    // tab; default off
    boole is_typing_indicator_allowed;

    // users may rename themselves after connecting; when off a rename request is silently ignored,
    // because the switch is meant against name games - an admin still renames freely; default on
    boole is_client_rename_allowed;
    boole is_link_preview_allowed;

    // Image limits and request cooldowns.
    // largest accepted raw image size (bytes) for an avatar; larger uploads are silently dropped
    int64 avatar_max_size_bytes;

    // largest accepted raw image size (bytes) for a group/channel icon, 1000-30000; editable in the
    // server settings tab
    int64 icon_max_size_bytes;
    uint64 chat_cooldown_milliseconds;
    uint64 join_channel_request_cooldown_milliseconds;
    uint64 delete_channel_request_cooldown_milliseconds;
    uint64 create_channel_request_cooldown_milliseconds;

    // Current counts and capacity.
    uint64 channel_count;
    _Atomic(uint64) max_channel_count; // allocated slots; resize under clients and channels write locks

    // configured allocation size, including root and temporary channels
    uint64 channel_count_limit;
    uint64 client_count;
    uint64 max_client_count;
    uint64 reserved_slots;
    uint64 keys_count;
    int64 websocket_port;

    // Public WebRTC address and forwarded UDP ports.
    // In WireGuard mode, a router forwards this server's ports through a tunnel. WebRTC uses
    // the fixed UDP range below, and host candidates include the public address so clients know
    // where to send. When disabled, the address and port range below are ignored.
    // See selfhosting-tools/wireguard/README.md.
    boole is_wireguard_mode_active;
    char webrtc_public_address[64]; // ipv4 as text, the router's public address
    int64 webrtc_port_range_begin;
    int64 webrtc_port_range_end;

    // WebSocket message limits.
    int64 websocket_message_max_length;
    int64 websocket_chat_message_string_max_length;

    // Connection passwords and negotiated session policy.
    crypto_key_t keys[100];
    char plaintext_keys[100][256];
    uint64 dh_exchange_count;
    uint64 session_encryption_layer_count;
    crypto_key_type_t session_cipher;
    boole is_random_cipher_layer_enabled;

    // Default name and administrator login.
    char default_client_name[30];
    char admin_password[ADMIN_PASSWORD_MAX_LENGTH];
    boole is_admin_password_initial;

    // Process restart.
    boole is_restart_on_crash_enabled;

    // TLS proxies and served client destination.
    boole is_stunnel_enabled;
    boole is_external_wss_enabled;
    boole is_tailscale_funnel_enabled;
    int64 wss_port;
    char stunnel_domain[256];
    char stunnel_cert_fullchain[512];
    char stunnel_cert_privkey[512];
    char client_html_dest[512];

    // Optional music storage; uploads and playback otherwise remain in RAM.
    char music_bot_snapshot_previous[64]; // old snapshot awaiting deletion after an atomic save
    char music_bot_snapshot[64]; // optional snapshot consumed after successful restoration

    // Built-in client web server.
    boole is_client_http_enabled;
    boole is_client_http_blank_when_disabled;
    int64 http_port;
    char http_webroot[512];
    boole is_https_enabled;
    int64 https_port;

    // Client appearance and published server information.
    char default_theme[32];
    char default_mobile_theme[32];
    char *server_banner;
    boole is_server_uptime_published;
    boole is_server_slots_published;
    boole is_server_banner_published;
    boole is_server_features_published;
    boole is_client_config_embedded;

    // Chat delivery and attachment limits.
    // deliver direct text chat to idle clients; their websocket stays open in idle, so delivery
    // works; default on
    boole is_sending_text_to_idle_clients_allowed;

    // clients may send direct text chat to each other at all; off makes this a channels-only
    // server; default on
    boole is_private_messaging_allowed;

    // any file may be sent in chat, not just pictures; relayed through ram only, never written to
    // disk; default off
    boole is_file_upload_allowed;

    // largest accepted raw file size for a chat file; the server holds ~2x this in ram per transfer
    int64 file_upload_max_size_bytes;

    // largest raw picture clients may send inline; pictures are e2e encrypted, so clients enforce
    // it (the upload gate is the hard bound)
    int64 chat_picture_max_size_bytes;

    // inline chat pictures; on by default, never a setup question, the upload intents enforce it
    boole is_chat_picture_allowed;

    // Country and RSA admission rules.
    // refuse joins whose ip resolves to a listed country (per connection, works with flags off);
    // set in the settings tab, never in setup; default off
    boole is_country_blocking_active;

    // weakest client rsa key accepted, 2048..8192; a modulus may come out one bit under its nominal
    // size, so the check allows minimum-1. weaker clients are dropped silently
    int64 minimum_rsa_key_bits;

    // tell a rejected client which size is required so it can offer to regenerate. off keeps the
    // requirement secret and the drop silent; default off
    boole is_minimum_rsa_key_bits_announced;

    // uppercase iso 3166-1 alpha-2, the form the geoip db emits
    char blocked_countries[MAX_BLOCKED_COUNTRIES][COUNTRY_ISO_CODE_LENGTH];
    uint64 blocked_countries_count;

    // Admin log categories and retention.
    // admin log: record each completed join with username and ip; default off
    boole is_logging_client_joins_enabled;

    // admin log: record renames (old -> new); default off
    boole is_logging_username_changes_enabled;

    // admin log: record groups being added to clients; default off
    boole is_logging_group_changes_enabled;

    // admin log: record who saved the server settings; default off
    boole is_logging_server_settings_updates_enabled;

    // admin log: record kicks and bans (who did it, to whom); default off
    boole is_logging_kicks_and_bans_enabled;

    // admin log: record authenticated clients disconnecting; default off
    boole is_logging_client_disconnects_enabled;

    // Record every socket opening and closing, including unauthenticated connections; default off.
    boole is_logging_socket_opens_and_closes_enabled;

    // admin log: record refused joins (wrong key, banned ip or identity, country, same ip) and
    // wrong admin passwords; default off
    boole is_logging_failed_attempts_enabled;
    int64 admin_log_max_size_bytes; // ram cap of the admin log; oldest entries fall out over it
    int64 admin_log_retention_days; // admin log entries older than this are purged once a day
} server_settings_t;

// --- Audio state and music bots ---

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

typedef struct music_bot_single_song_data_t
{
    uint64 generation;
    boole is_existing;
    boole is_being_uploaded;
    uint64 length_seconds;
    char song_name[SONG_NAME_MAX_LENGTH];
    boole is_this_song_scheduled_for_deletion;
    boole is_currently_playing;
    uint64 playback_buffered_until_ms;
    ubyte *mp3_data_buffer;
    uint64 mp3_data_buffer_length;
} music_bot_single_song_data_t;

typedef struct music_bot_audio_keys_t
{
    ubyte keys[4][32];
    uint64 packet_counter;
} music_bot_audio_keys_t;

typedef struct music_bot_client_extension_t
{
    music_bot_audio_keys_t *audio_keys;
    uint64 music_bot_pthread_handle;
    _Atomic(boole) is_music_bot_running;
    _Atomic(boole) is_restore_pending;
    uint64 next_song_generation;
    _Atomic(boole) is_playback_paused;
    _Atomic(boole) is_skip_requested;
    uint64 music_bot_songs_count;
    uint64 pending_song_uploads; // pins the song table until detached upload workers finish
    music_bot_single_song_data_t *songs; // allocated once for an actual music bot
} music_bot_client_extension_t;

// --- Client sessions and uploads ---

typedef struct client_file_upload_extension_t
{
    ubyte *file_upload_buffer; // stores the base64 content of the file
    uint64 buffer_cursor;
    uint64 expected_file_length;
    uint64 started_ms;
    uint64 last_progress_ms;
    char intent[31];
} client_file_upload_extension_t;

typedef struct client_t
{
    // Per-client attachment preferences.
    boole block_direct_chat_pictures;
    boole block_direct_chat_files;
    boole block_channel_chat_pictures;
    boole block_channel_chat_files;

    // Transport connections.
    // the session of this client's /audio socket (audio_websocket.c), NULL_POINTER without one
    struct audio_websocket_session *audio_websocket;

    // the session of this client's /video socket (video_websocket.c), NULL_POINTER without one
    struct video_websocket_session *video_websocket;
    uint64 timestamp_last_music_bot_key_request;
    ws_cli_conn_t *p_ws_connection;

    // Connection and authentication state.
    boole is_existing;
    boole is_authenticated;
    boole is_disconnect_ban; // Retained until disconnect notification, then cleared with the session.
    boole is_admin;
    boole is_audio_websocket_authenticated;
    boole is_video_websocket_authenticated;
    boole is_dh_shared_secret_agreed_upon;
    boole is_streaming_song;
    boole is_public_key_challenge_sent;

    // Policy captured for this login.
    char crypto_policy_nonce[33];
    lemon_session_policy crypto_policy;

    // Admission, idle state and channel placement.
    boole is_waiting_for_login_slot;
    boole is_idle;
    boole is_music_bot;
    boole is_temp_admin_channel; // TRUE if this client owns a temp channel
    uint64 client_id; // client id is the same as the index of the client_t in clients_array
    uint64 channel_id;
    uint64 temp_channel_id; // if is_temp_admin_channel, the id of the temp channel this client owns
    uint64 idle_return_channel_id;

    // authorization snapshot, never sent to clients
    char idle_return_channel_password[CHANNEL_PASSWORD_MAX_LENGTH];

    // Effective permissions and moderation.
    uint64 talk_power;
    uint64 channel_message_power;
    uint64 channel_picture_power;
    uint64 stream_power;
    uint64 channel_group_role;
    uint64 muted_until_ms;

    // Values are defined by audio_state_e.
    int64 audio_state;

    // Activity and channel-key reset voting.
    uint64 timestamp_connected;
    uint64 timestamp_last_action;
    uint64 timestamp_last_url_lookup;
    uint64 timestamp_last_maintain_connection_message_received;
    boole has_pending_maintainer_reset_vote;
    uint64 maintainer_reset_vote_channel_id;
    uint64 maintainer_reset_vote_generation;

    // Identity and connection encryption.
    char username[USERNAME_MAX_LENGTH];
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    char dh_shared_secret[SHARED_SECRET_LENGTH];

    // These keys are made from the shared secret when the client connects.
    // Metadata messages, audio and video all use them. When a new connection
    // takes over an existing session, transfer the session keys and password
    // snapshot together so they match the client that just reconnected.
    crypto_session_keys_t session_keys;
    crypto_password_set_t *password_keys;

    // counts the frames the server sent to this client; every relay thread increases it atomically
    _Atomic(uint64) media_send_counter;
    char challenge_string[CHALLENGE_STRING_LENGTH];

    // Music, address and avatar.
    char song_name[SONG_NAME_MAX_LENGTH];
    char ip_address[INET6_ADDRSTRLEN]; // max size of an ipv6 address
    char country_iso_code[COUNTRY_ISO_CODE_LENGTH];

    // heap-allocated on demand (MEMALLOC_AVATAR), NULL when none; the live avatar served to others.
    // persistent copy lives in the identity store
    char *base64_avatar;

    // Video streaming: this client is the channel's streamer, with the details it announced.
    boole is_streaming_video;
    char video_stream_source[VIDEO_STREAM_SOURCE_MAX_LENGTH];
    char video_stream_codec[VIDEO_STREAM_CODEC_MAX_LENGTH];
    int64 video_stream_width;
    int64 video_stream_height;
    int64 video_stream_fps;

    // the streamer sends sound packets next to the picture packets (the relay does not tell them
    // apart; the viewers show a volume control)
    boole is_video_stream_sound_included;
    uint64 timestamp_last_video_keyframe_request_forwarded_ms;

    // Viewer state: the streamer allowed this client to watch (implicit for members present at
    // the start, explicit for later joiners), and the client pressed connect. Keep both flags
    // here: rebuilding a WebRTC peer on a retry or closing its transport must retain that consent.
    boole is_video_viewer_allowed;
    boole is_video_viewer_watching;

    // Group membership and optional client extensions.
    int *group_ids; // must be int because a function of another library depends on this being int
    music_bot_client_extension_t music_bot_client_extension;
    client_file_upload_extension_t file_upload_extension;
} client_t;

// --- Channels and membership ---

typedef struct channel_group_member_t
{
    char identity[IDENTITY_HASH_MAX_LENGTH];
    uint64 role;
    uint64 muted_until_ms;
} channel_group_member_t;

// channel id is the same as the channel's index in the array
typedef struct channel
{
    // Channel state and access.
    boole is_existing;
    boole is_root_channel;
    boole is_channel_maintainer_present;
    boole is_using_password;
    boole is_audio_enabled;

    // Required powers.
    uint64 required_talk_power;
    uint64 required_message_power;
    uint64 required_picture_power;
    uint64 required_stream_power;

    // Video, music and temporary-channel state.
    // the per-channel video streaming toggle, persisted; only meaningful while the server-wide
    // switch is on. default on, so switching the server on lights every channel up and admins /
    // temp channel owners switch single channels off
    boole is_video_stream_enabled;

    // somebody streams in here right now (one stream per channel at a time)
    boole is_video_stream_active;
    uint64 video_streamer_client_id;
    boole is_music_bot_active_in_channel;
    boole is_temp_channel;
    boole is_client_limit_active;

    // Tree position and capacity.
    uint64 channel_id;
    uint64 parent_channel_id;
    uint64 current_clients;
    uint64 max_client_count;
    uint64 type;

    // Channel-key maintainer.
    uint64 maintainer_id;

    // bumped on EVERY maintainer state change; a reset vote carries the generation it complains
    // about, so a vote fired against a previous maintainer never counts against the new one
    uint64 maintainer_generation;

    // Presentation, password and saved membership.
    boole has_channel_icon;
    uint64 icon_id;
    char name[CHANNEL_NAME_MAX_LENGTH];
    char password[CHANNEL_PASSWORD_MAX_LENGTH];
    char description[CHANNEL_DESCRIPTION_MAX_LENGTH];
    channel_group_member_t *group_members;
} channel_t;

// --- Chat message records ---

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

// --- Permissions and groups ---

typedef enum permission_t
{
    PERMISSION_CREATE_CHANNEL,
    PERMISSION_CREATE_TEMP_CHANNEL,
    PERMISSION_KICK,
    PERMISSION_BAN,
    PERMISSION_VIEW_STORED_USERS,
    PERMISSION_VIEW_GROUP_PERMISSIONS,
    PERMISSION_VIEW_GROUP_MEMBERS,
    PERMISSION_CREATE_GROUPS,
    PERMISSION_EDIT_GROUPS,
    PERMISSION_DELETE_GROUPS,
    PERMISSION_ASSIGN_GROUPS,
    PERMISSION_REMOVE_GROUPS,
    PERMISSION_EDIT_GROUP_PERMISSIONS,
    PERMISSION_UPLOAD_ICONS,
    PERMISSION_VIEW_CLIENT_DETAILS,
    PERMISSION_VIEW_SERVER_LOGS,
    PERMISSION_VIEW_SERVER_INFO,
    PERMISSION_SEND_OFFLINE_MESSAGES,
    PERMISSION_RECEIVE_OFFLINE_MESSAGES,
    PERMISSION_MESSAGE_IDLE_CLIENTS,
    PERMISSION_CALL_IDLE_CLIENTS,
    PERMISSION_DELETE_ICONS,
    PERMISSION_UPLOAD_EMOJIS,
    PERMISSION_DELETE_EMOJIS,
    PERMISSION_VIEW_IP_ADDRESSES,
    PERMISSION_VIEW_BANS,
    PERMISSION_REMOVE_BANS,
    PERMISSION_MOVE_CLIENTS,
    PERMISSION_KICK_FROM_CHANNEL,
    PERMISSION_MUTE_CLIENTS,
    PERMISSION_STOP_CLIENT_STREAMS,
    PERMISSION_EDIT_CHANNEL_NAME,
    PERMISSION_EDIT_CHANNEL_DESCRIPTION,
    PERMISSION_EDIT_CHANNEL_PASSWORD,
    PERMISSION_EDIT_CHANNEL_POWERS,
    PERMISSION_EDIT_CHANNEL_OPTIONS,
    PERMISSION_DELETE_CHANNEL,
    PERMISSION_ASSIGN_CHANNEL_GROUPS,
    PERMISSION_SEND_DIRECT_PICTURES,
    PERMISSION_SEND_DIRECT_FILES,
    PERMISSION_SEND_CHANNEL_PICTURES,
    PERMISSION_SEND_CHANNEL_FILES,
    PERMISSION_CREATE_MUSIC_BOTS,
    PERMISSION_DELETE_MUSIC_BOTS,
    PERMISSION_UPLOAD_MUSIC_TRACKS,
    PERMISSION_DELETE_MUSIC_TRACKS,
    PERMISSION_CONTROL_MUSIC_PLAYBACK,
    PERMISSION_USE_RESERVED_SLOTS,
    PERMISSION_CREATE_CHANNEL_GROUPS,
    PERMISSION_EDIT_CHANNEL_GROUPS,
    PERMISSION_DELETE_CHANNEL_GROUPS,
    PERMISSION_VIEW_CLIENT_CONNECTION_TIME,
    PERMISSION_BYPASS_CHANNEL_PASSWORD,
    PERMISSION_COUNT
} permission_t;

enum
{
    PERMISSION_DEFAULT,
    PERMISSION_DENY,
    PERMISSION_GRANT
};

#define PERMISSION_POWER_MAX 1000U

enum
{
    PERMISSION_FLAG_NEGATE = 1,
    PERMISSION_FLAG_SKIP = 2
};

typedef enum permission_power_t
{
    PERMISSION_KICK_POWER,
    PERMISSION_KICK_PROTECTION,
    PERMISSION_BAN_POWER,
    PERMISSION_BAN_PROTECTION,
    PERMISSION_GROUP_MANAGEMENT_POWER,
    PERMISSION_GROUP_PROTECTION,
    PERMISSION_TALK_POWER,
    PERMISSION_CHANNEL_MESSAGE_POWER,
    PERMISSION_CHANNEL_PICTURE_POWER,
    PERMISSION_STREAM_POWER,
    PERMISSION_MOVE_POWER,
    PERMISSION_MOVE_PROTECTION,
    PERMISSION_MODERATION_POWER,
    PERMISSION_MODERATION_PROTECTION,
    PERMISSION_EDIT_POWER,
    PERMISSION_POWER_COUNT
} permission_power_t;

typedef struct group_permissions_t
{
    uint64 values[PERMISSION_COUNT];
    uint64 powers[PERMISSION_POWER_COUNT];
    uint64 flags[PERMISSION_COUNT];
    uint64 power_flags[PERMISSION_POWER_COUNT];
    uint64 grants[PERMISSION_COUNT];
    uint64 power_grants[PERMISSION_POWER_COUNT];
} group_permissions_t;

typedef enum group_type_t
{
    GROUP_TYPE_NORMAL,
    GROUP_TYPE_GUEST,
    GROUP_TYPE_MODERATOR,
    GROUP_TYPE_ADMIN
} group_type_t;

typedef struct group_t
{
    boole is_existing;
    uint64 id;
    uint64 icon_id;
    group_type_t group_type;
    boole is_default_guest;

    // a group may exist without a linked icon; icon_id is only meaningful when this is TRUE
    boole has_icon;
    char name[GROUP_MAX_NAME_LENGTH];
    group_permissions_t permissions;
} group_t;

// --- Bans and icons ---

// one persisted ban. matching is by ip address; the rest (country/identity/extra data) is recorded
// for the admin
typedef struct ban_entry_t
{
    boole is_existing;
    char removal_token[33];
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
    char *base64; // heap (MEMALLOC_ICON), NUL-terminated data url; NULL while the slot is free
} icon_t;

// --- Stored identities and offline messages ---

// Saved identity data restored by core/identity_store.c. public_key stores the identity hash;
// raw_public_key stores the RSA public key used to encrypt messages for an offline identity.
typedef struct client_stored_data_t
{
    char public_key[MAX_PUBLIC_KEY_LENGTH];
    uint64 group_ids[MAX_GROUPS_FOR_SINGLE_CLIENT];
    uint64 group_id_count;
    char username[USERNAME_MAX_LENGTH];
    char base64_avatar[MAX_CLIENT_AVATAR_LENGTH];

    // when this identity was last connected; 0 = never recorded. only filled/served when
    // is_last_seen_enabled is on
    int64 last_seen_unix_seconds;

    // the ACTUAL rsa public key, not the hash in public_key above. only collected, persisted and
    // served while is_offline_messaging_allowed is on - it is what lets a peer encrypt a message to
    // this identity while it is offline. empty = not known (yet)
    char raw_public_key[MAX_PUBLIC_KEY_LENGTH];
} client_stored_data_t;

// one text message waiting for a saved identity to come back. the payload is opaque to the
// server: the sender encrypted it with the RECIPIENT's public key exactly like a normal direct
// message, so queueing never gives the server anything readable
typedef struct offline_chat_message_t
{
    boole is_used;
    char recipient_identity_hash[IDENTITY_HASH_MAX_LENGTH];
    char sender_identity_hash[IDENTITY_HASH_MAX_LENGTH];
    char sender_name[USERNAME_MAX_LENGTH]; // what the recipient sees it came from
    char *base64_encrypted_message; // heap, MEMALLOC_OFFLINE_MESSAGE
    uint64 message_length;
    int64 queued_unix_seconds;

    // delivery order, and lets a client drop duplicates
    uint64 sequence_number;
} offline_chat_message_t;

// --- Memory allocation tags ---

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
    MEMALLOC_VIDEOCHANNEL_ONMESSAGE,
    MEMALLOC_MUSICBOT_AUDIOCHANNEL_ONMESSAGE,
    MEMALLOC_MUSICBOT_SONG,
    MEMALLOC_FILE_UPLOAD_BY_PARTS,
    MEMALLOC_FILE_UPLOAD_BY_PARTS1,
    MEMALLOC_FILE_UPLOAD_BY_PARTS2,
    MEMALLOC_FILE_DOWNLOAD_BY_PARTS,
    MEMALLOC_AVATAR,
    MEMALLOC_OFFLINE_MESSAGE,
    MEMALLOC_OFFLINE_MESSAGES_ARRAY,
    MEMALLOC_ADMIN_LOG,
    MEMALLOC_ICON,
    MEMALLOC_SERVER_BANNER
} memory_manager_allocation_type_e;

// --- WebRTC peers ---

typedef struct webrtc_peer_t
{
    struct webrtc_teardown_arg_t *teardown;
    int peer_connection_handle;
    int data_channel_handle;

    // the second, reliable channel of this peer (VIDEO_DATACHANNEL_LABEL); 0 while not created
    int video_data_channel_handle;
    boole is_video_channel_connected; // its open callback fired and its close callback has not
    boole connected;
    audio_state_e last_sent_audio_state;
    boole is_sending_audio_right_now;
    uint64 channel_id;
    uint64 client_id;
    boole is_existing;
    char dh_shared_secret[SHARED_SECRET_LENGTH];
    ws_cli_conn_t *p_ws_connection;

    // create requests since the transport last reached RTC_CONNECTED
    uint64 attempts_since_connected;
    uint64 cooldown_until_ms; // while in the future, create requests are refused
} webrtc_peer_t;

// --- File delivery workers ---

typedef enum file_send_type_e
{
    FILE_SEND_TYPE_TO_CLIENT,
    FILE_SEND_TYPE_TO_CHANNEL
} file_send_type_e;

typedef struct data_for_file_send_thread_t
{
    boole is_existing;
    file_send_type_e send_type;
    uint64 receiving_clients_count;
    uint64 client_receiver_id; // in case it is sent to a single client
    char *buffer; // this is the buffer that must be split into parts
    uint64 size;
    uint64 client_sender_id;
    uint64 server_chat_message_id;
    uint64 local_chat_message_id;

    // what the receivers are told arrived: direct/channel chat picture or file
    char receive_type[32];
    crypto_key_t sender_authentication_key;
    crypto_key_t *receiver_authentication_keys;
    uint64 receiving_client_ids[]; // channel transfers allocate room for the startup slot count
} data_for_file_send_thread_t;

#endif
