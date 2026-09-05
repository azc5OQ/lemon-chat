#include "definitions.h"


#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"

#include "../third-party/zhicheng/base64.h"
#include "../third-party/ITH-sha/sha256.h"
#include "../third-party/kokke-tiny-aes-c/aes.h"

#include "../third-party/rxi-log/log.h"

#include "client_message.h"
#include "server_message.h"
#include "server_logs.h"

#include "../third-party/libtom/libtommath/tommath.h"

#include "../third-party/mbedtls-3.6.6/include/mbedtls/rsa.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/entropy.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/ctr_drbg.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/bignum.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/hkdf.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/md.h"

#include "memory_manager.h"
#include "audio_channel.h"

#include "../third-party/eteran-cvector/cvector.h"

#include "util.h"

#ifdef WIN32
#include <Windows.h>
#include <crtdbg.h>
#include <io.h>
#else
#include <unistd.h>
#endif

custom_rwlock_t g_clients_global_rwlock_guard;
custom_rwlock_t g_channels_global_rwlock_guard;
custom_rwlock_t g_icons_global_rwlock_guard;
custom_rwlock_t g_tags_global_rwlock_guard;
custom_rwlock_t g_webrtc_muggles_rwlock_guard;
custom_rwlock_t g_bans_global_rwlock_guard;

pthread_mutex_t g_chat_message_id_mutex = PTHREAD_MUTEX_INITIALIZER;

// guards the identity store (g_client_stored_data). taken as the innermost lock so restore-on-auth and the
// snapshot at save time never tear each other's reads/writes; the save handler only holds the tags read lock,
// which does not exclude a concurrent restore reader
pthread_mutex_t g_client_stored_data_mutex = PTHREAD_MUTEX_INITIALIZER;

// guards the offline message queue (g_offline_messages). a leaf lock like the identity store one:
// never take another lock while holding it
pthread_mutex_t g_offline_messages_mutex = PTHREAD_MUTEX_INITIALIZER;
offline_chat_message_t* g_offline_messages = NULL_POINTER;
uint64 g_offline_message_sequence_counter = 0;

uint64 g_chat_message_id;
client_t* g_clients_array;
channel_t* g_channel_array;
client_stored_data_t* g_client_stored_data;
icon_t* g_icons_array;
tag_t* g_tags_array;
ban_entry_t* g_ban_array;
server_settings_t g_server_settings;

static void _base_internal__serialize_identities(cJSON* json_root);
static boole _base_internal__derive_keys_from_shared_secret(char* dh_shared_secret, unsigned char* out_enc_key, unsigned char* out_mac_key);
static boole _base_internal__compute_metadata_tag(unsigned char* mac_key, char* iv_base64, char* data_base64, unsigned char* out_tag);
static boole _base_internal__constant_time_str_equal(char* a, char* b);
static boole _base_internal__are_strings_equal_ignoring_ascii_case(char* string1, char* string2);

/**
 * @brief writes contents to path atomically: it writes a temp file, flushes it to disk, then atomically
 *        replaces the target. so a crash or power loss mid-write can never truncate or corrupt the real
 *        file - the previous good version stays intact until the replace succeeds.
 *
 * @param char* path -> the destination file path
 * @param char* contents -> the NUL-terminated text to write
 *
 * @return boole -> TRUE only if the file was fully written and atomically put in place
 */
boole base__write_file_atomically(char* path, char* contents)
{
    char tmp_path[1024];
    FILE* file = NULL_POINTER;
    uint64 contents_length = 0;
    uint64 written = 0;

    clib__null_memory(tmp_path, sizeof(tmp_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    file = fopen(tmp_path, "wb");
    if (file == NULL_POINTER)
    {
        return FALSE;
    }

    contents_length = strlen(contents);
    written = fwrite(contents, 1, contents_length, file);
    fflush(file);
#ifdef WIN32
    _commit(_fileno(file));
#else
    fsync(fileno(file));
#endif
    fclose(file);

    if (written != contents_length)
    {
        remove(tmp_path);
        return FALSE;
    }

#ifdef WIN32
    // rename() on windows fails if the destination already exists, so use MoveFileEx with replace
    if (MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        remove(tmp_path);
        return FALSE;
    }
#else
    if (rename(tmp_path, path) != 0)
    {
        remove(tmp_path);
        return FALSE;
    }
#endif

    return TRUE;
}

/**
 * @brief checks whether an ip address is currently banned
 *
 * @param char* ip_address -> the ip to check
 *
 * @return boole TRUE if the ip is in the ban list
 */
boole base__is_ip_banned(char* ip_address)
{
    boole is_banned = FALSE;
    uint64 i = 0;

    if (ip_address == NULL_POINTER || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == FALSE)
        {
            continue;
        }
        if (clib__is_string_equal(g_ban_array[i].ip_address, ip_address) == TRUE)
        {
            is_banned = TRUE;
            break;
        }
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    return is_banned;
}

/**
 * @brief whether a public key is on the ban list. a ban records the target's identity at ban
 *        time, so the same identity is refused even when it returns from a different ip address.
 *        entries without a recorded identity never match
 *
 * @param char* public_key -> the connecting client's public key
 *
 * @return boole TRUE if the identity is in the ban list
 */
boole base__is_identity_banned(char* public_key)
{
    boole is_banned = FALSE;
    uint64 i = 0;

    if (public_key == NULL_POINTER || public_key[0] == 0 || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == FALSE || g_ban_array[i].identity[0] == 0)
        {
            continue;
        }
        if (clib__is_string_equal(g_ban_array[i].identity, public_key) == TRUE)
        {
            is_banned = TRUE;
            break;
        }
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    return is_banned;
}

/**
 * @brief whether a resolved country code is on the join block list. an empty or unresolved
 *        country (local addresses, lookup failures) is never blocked
 *
 * @param char* country_iso_code -> uppercase 2-letter code as the geoip db emits it
 *
 * @return boole TRUE when country blocking is on and the code is listed
 */
boole base__is_country_blocked(char* country_iso_code)
{
    uint64 i = 0;

    if (g_server_settings.is_country_blocking_active == FALSE || country_iso_code == NULL_POINTER || country_iso_code[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        if (clib__is_string_equal(&g_server_settings.blocked_countries[i][0], country_iso_code) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief adds a ban entry. ignored if the ip is already banned or the ban list is full
 *
 * @param char* ip_address -> the banned ip (matched at socket open)
 * @param char* country_iso_code -> the client's country at ban time (may be empty)
 * @param char* identity -> the client's identity / public key at ban time (matched at join; may be empty)
 * @param char* extra_data -> free-form extra data, e.g. a future fingerprint (may be empty)
 *
 * @attention caller must hold the bans write lock
 *
 * @return boole TRUE if a ban was added
 */
boole base__add_ban(char* ip_address, char* country_iso_code, char* identity, char* extra_data)
{
    uint64 i = 0;
    int64 free_index = -1;

    if (ip_address == NULL_POINTER || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == TRUE)
        {
            if (clib__is_string_equal(g_ban_array[i].ip_address, ip_address) == TRUE)
            {
                return FALSE; // already banned
            }
        }
        else if (free_index == -1)
        {
            free_index = (int64)i;
        }
    }

    if (free_index == -1)
    {
        log_info("%s", "base__add_ban: ban list is full");
        return FALSE;
    }

    clib__null_memory(&g_ban_array[free_index], sizeof(ban_entry_t));
    g_ban_array[free_index].is_existing = TRUE;
    g_ban_array[free_index].timestamp_banned = base__get_timestamp_ms();
    clib__copy_memory(ip_address, &g_ban_array[free_index].ip_address[0], clib__utf8_string_length(ip_address), BAN_IP_MAX_LENGTH - 1);
    if (country_iso_code != NULL_POINTER)
    {
        clib__copy_memory(country_iso_code, &g_ban_array[free_index].country_iso_code[0], clib__utf8_string_length(country_iso_code), COUNTRY_ISO_CODE_LENGTH - 1);
    }
    if (identity != NULL_POINTER)
    {
        clib__copy_memory(identity, &g_ban_array[free_index].identity[0], clib__utf8_string_length(identity), MAX_PUBLIC_KEY_LENGTH - 1);
    }
    if (extra_data != NULL_POINTER)
    {
        clib__copy_memory(extra_data, &g_ban_array[free_index].extra_data[0], clib__utf8_string_length(extra_data), BAN_EXTRA_DATA_MAX_LENGTH - 1);
    }

    return TRUE;
}

/**
 * @brief removes a ban entry matching an ip address
 *
 * @param char* ip_address -> the ip to unban
 *
 * @attention caller must hold the bans write lock
 *
 * @return boole TRUE if an entry was removed
 */
boole base__remove_ban_by_ip(char* ip_address)
{
    uint64 i = 0;

    if (ip_address == NULL_POINTER || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == TRUE && clib__is_string_equal(&g_ban_array[i].ip_address[0], ip_address) == TRUE)
        {
            clib__null_memory(&g_ban_array[i], sizeof(ban_entry_t));
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief writes the in-memory identity store (g_client_stored_data) into json_root under the "identities"
 *        key: one object per stored identity with its public-key hash and the tag ids it owns. entries with
 *        no hash or no tags are skipped. when identities are disabled the existing "identities" key is left
 *        untouched, so disabling then re-enabling does not wipe the stored data. guarded by the store mutex.
 *
 * @param cJSON* json_root -> the settings document being written
 *
 * @return void
 */
static void _base_internal__serialize_identities(cJSON* json_root)
{
    cJSON* json_identities = NULL_POINTER;
    cJSON* json_identity = NULL_POINTER;
    cJSON* json_identity_tag_ids = NULL_POINTER;
    uint64 i = 0;
    uint64 t = 0;

    if (g_server_settings.are_identities_enabled == FALSE)
    {
        DBG_IDENTITIES log_info("%s", "serialize_identities: identities disabled -> leaving the existing \"identities\" json key untouched \n");
        return;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "identities");
    json_identities = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "identities", json_identities);

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        // keep a slot that has tags, an avatar or an alias (any of them makes the identity worth storing)
        if (g_client_stored_data[i].public_key[0] == 0 || (g_client_stored_data[i].tag_id_count == 0 && g_client_stored_data[i].base64_avatar[0] == 0 && g_client_stored_data[i].alias[0] == 0))
        {
            continue;
        }

        DBG_IDENTITIES log_info("%s %llu %s %llu %s %s %s", "serialize_identities: writing store slot", i, "with", (uint64)g_client_stored_data[i].tag_id_count, "tag(s), hash [", &g_client_stored_data[i].public_key[0], "] \n");

        json_identity = cJSON_CreateObject();
        cJSON_AddStringToObject(json_identity, "public_key_hash", &g_client_stored_data[i].public_key[0]);
        cJSON_AddStringToObject(json_identity, "username", &g_client_stored_data[i].username[0]);
        if (g_client_stored_data[i].base64_avatar[0] != 0)
        {
            cJSON_AddStringToObject(json_identity, "base64_avatar", &g_client_stored_data[i].base64_avatar[0]);
        }
        if (g_client_stored_data[i].alias[0] != 0)
        {
            cJSON_AddStringToObject(json_identity, "alias", &g_client_stored_data[i].alias[0]);
        }
        // only ever written while the admin keeps last-seen enabled: switching the setting off stops
        // recording AND stops the recorded values from being carried into the next save
        if (g_server_settings.allow_last_seen == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            cJSON_AddNumberToObject(json_identity, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds);
        }
        // same rule for the raw public key: written only while offline messages are enabled, so
        // turning the feature off also stops carrying the keys into the next save
        if (g_server_settings.allow_offline_messages == TRUE && g_client_stored_data[i].raw_public_key[0] != 0)
        {
            cJSON_AddStringToObject(json_identity, "raw_public_key", &g_client_stored_data[i].raw_public_key[0]);
        }

        json_identity_tag_ids = cJSON_CreateArray();
        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            cJSON_AddItemToArray(json_identity_tag_ids, cJSON_CreateNumber((double)g_client_stored_data[i].tag_ids[t]));
        }
        cJSON_AddItemToObject(json_identity, "tag_ids", json_identity_tag_ids);

        cJSON_AddItemToArray(json_identities, json_identity);
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief saves server settings to a file
 *
 * @note caller holds the channels, icons, tags and bans locks for reading
 *
 * @return boole -> TRUE if the file was written
 */
boole base__save_server_settings_to_file(void)
{
    cJSON* json_root = NULL_POINTER;
    cJSON* json_channels = NULL_POINTER;
    cJSON* json_channel = NULL_POINTER;
    cJSON* json_icons = NULL_POINTER;
    cJSON* json_icon = NULL_POINTER;
    cJSON* json_tags = NULL_POINTER;
    cJSON* json_tag = NULL_POINTER;
    cJSON* json_bans = NULL_POINTER;
    cJSON* json_ban = NULL_POINTER;
    cJSON* json_blocked_countries = NULL_POINTER;
    char* json_text = NULL_POINTER;
    FILE* settings_file = NULL_POINTER;
    int64 file_length = 0;
    char* file_buffer = NULL_POINTER;
    uint64 bytes_read = 0;
    channel_t* channel_in_loop = NULL_POINTER;
    icon_t* icon_in_loop = NULL_POINTER;
    tag_t* tag_in_loop = NULL_POINTER;
    ban_entry_t* ban_in_loop = NULL_POINTER;
    uint64 i = 0;
    boole write_succeeded = FALSE;

    // read the existing settings file first so keys / ports / other settings stay untouched
    settings_file = fopen("server_settings.json", "rb");
    if (settings_file == NULL_POINTER)
    {
        log_info("%s", "save_server_settings: server_settings.json not found, refusing to overwrite");
        return FALSE;
    }

    fseek(settings_file, 0, SEEK_END);
    file_length = ftell(settings_file);
    fseek(settings_file, 0, SEEK_SET);
    if (file_length > 0)
    {
        file_buffer = (char* )malloc(file_length + 1);
        if (file_buffer != NULL_POINTER)
        {
            bytes_read = fread(file_buffer, 1, file_length, settings_file);
            file_buffer[bytes_read] = 0;
        }
    }
    fclose(settings_file);

    if (file_buffer == NULL_POINTER)
    {
        return FALSE;
    }

    json_root = cJSON_Parse(file_buffer);
    free(file_buffer);

    if (json_root == NULL_POINTER)
    {
        log_info("%s", "save_server_settings: existing server_settings.json could not be parsed, refusing to overwrite");
        return FALSE;
    }

    // update the general-settings toggles (delete-then-add so a value already present is replaced)
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_display_country_flags_active");
    cJSON_AddItemToObject(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "hide_admin_country_flag");
    cJSON_AddItemToObject(json_root, "hide_admin_country_flag", cJSON_CreateBool(g_server_settings.hide_admin_country_flag == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_voice_chat_active");
    cJSON_AddItemToObject(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_music_bot_audio_active");
    cJSON_AddItemToObject(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_hide_clients_in_password_protected_channels_active");
    cJSON_AddItemToObject(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_temp_channel_creation_allowed");
    cJSON_AddItemToObject(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));

    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "allow_typing_indicator");
    cJSON_AddItemToObject(json_root, "allow_typing_indicator", cJSON_CreateBool(g_server_settings.allow_typing_indicator == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "allow_client_renames");
    cJSON_AddItemToObject(json_root, "allow_client_renames", cJSON_CreateBool(g_server_settings.allow_client_renames == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_sending_text_to_idle_clients_allowed");
    cJSON_AddItemToObject(json_root, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "allow_private_messages");
    cJSON_AddItemToObject(json_root, "allow_private_messages", cJSON_CreateBool(g_server_settings.allow_private_messages == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_same_ip_address_allowed");
    cJSON_AddItemToObject(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_fast_reconnect_allowed");
    cJSON_AddItemToObject(json_root, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_identity_takeover_allowed");
    cJSON_AddItemToObject(json_root, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_websocket_ping_active");
    cJSON_AddItemToObject(json_root, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "webrtc_datachannel_cooldown_seconds");
    cJSON_AddNumberToObject(json_root, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "icon_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "show_music_bot_marquee_to_everyone");
    cJSON_AddItemToObject(json_root, "show_music_bot_marquee_to_everyone", cJSON_CreateBool(g_server_settings.show_music_bot_marquee_to_everyone == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "allow_file_uploads");
    cJSON_AddItemToObject(json_root, "allow_file_uploads", cJSON_CreateBool(g_server_settings.allow_file_uploads == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "file_upload_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "file_upload_max_size_bytes", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "chat_picture_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "chat_picture_max_size_bytes", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "allow_chat_pictures");
    cJSON_AddItemToObject(json_root, "allow_chat_pictures", cJSON_CreateBool(g_server_settings.allow_chat_pictures == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_country_blocking_active");
    cJSON_AddItemToObject(json_root, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "minimum_rsa_key_bits");
    cJSON_AddNumberToObject(json_root, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "announce_minimum_rsa_key_bits");
    cJSON_AddItemToObject(json_root, "announce_minimum_rsa_key_bits", cJSON_CreateBool(g_server_settings.announce_minimum_rsa_key_bits == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "blocked_countries");
    json_blocked_countries = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "blocked_countries", json_blocked_countries);
    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        cJSON_AddItemToArray(json_blocked_countries, cJSON_CreateString(&g_server_settings.blocked_countries[i][0]));
    }
    // only the log TOGGLES are persisted; the log lines themselves stay in ram, never in a file
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_client_joins");
    cJSON_AddItemToObject(json_root, "log_client_joins", cJSON_CreateBool(g_server_settings.log_client_joins == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_username_changes");
    cJSON_AddItemToObject(json_root, "log_username_changes", cJSON_CreateBool(g_server_settings.log_username_changes == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_tag_changes");
    cJSON_AddItemToObject(json_root, "log_tag_changes", cJSON_CreateBool(g_server_settings.log_tag_changes == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_server_settings_updates");
    cJSON_AddItemToObject(json_root, "log_server_settings_updates", cJSON_CreateBool(g_server_settings.log_server_settings_updates == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_kicks_and_bans");
    cJSON_AddItemToObject(json_root, "log_kicks_and_bans", cJSON_CreateBool(g_server_settings.log_kicks_and_bans == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_client_disconnects");
    cJSON_AddItemToObject(json_root, "log_client_disconnects", cJSON_CreateBool(g_server_settings.log_client_disconnects == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "log_failed_attempts");
    cJSON_AddItemToObject(json_root, "log_failed_attempts", cJSON_CreateBool(g_server_settings.log_failed_attempts == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_log_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "admin_log_max_size_bytes", (double)g_server_settings.admin_log_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_log_retention_days");
    cJSON_AddNumberToObject(json_root, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_password");
    cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_password_is_initial");
    cJSON_AddItemToObject(json_root, "admin_password_is_initial", cJSON_CreateBool(g_server_settings.admin_password_is_initial == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "are_identities_enabled");
    cJSON_AddItemToObject(json_root, "are_identities_enabled", cJSON_CreateBool(g_server_settings.are_identities_enabled == TRUE));

    // rebuild the channel layout (persistent fields only; runtime state like maintainer/occupants is skipped)
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "channels");
    json_channels = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "channels", json_channels);
    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        channel_in_loop = &g_channel_array[i];
        if (channel_in_loop->is_existing == FALSE)
        {
            continue;
        }

        // temp channels are disposable and never persisted
        if (channel_in_loop->is_temp_channel == TRUE)
        {
            continue;
        }

        json_channel = cJSON_CreateObject();
        cJSON_AddNumberToObject(json_channel, "channel_id", channel_in_loop->channel_id);
        cJSON_AddNumberToObject(json_channel, "parent_channel_id", (int64)channel_in_loop->parent_channel_id);
        cJSON_AddNumberToObject(json_channel, "type", channel_in_loop->type);
        cJSON_AddItemToObject(json_channel, "is_root_channel", cJSON_CreateBool(channel_in_loop->is_root_channel == TRUE));
        cJSON_AddItemToObject(json_channel, "is_using_password", cJSON_CreateBool(channel_in_loop->is_using_password == TRUE));
        cJSON_AddItemToObject(json_channel, "is_audio_enabled", cJSON_CreateBool(channel_in_loop->is_audio_enabled == TRUE));
        cJSON_AddItemToObject(json_channel, "is_client_limit_active", cJSON_CreateBool(channel_in_loop->is_client_limit_active == TRUE));
        cJSON_AddNumberToObject(json_channel, "max_client_count", (double)channel_in_loop->max_client_count);
        cJSON_AddItemToObject(json_channel, "has_channel_icon", cJSON_CreateBool(channel_in_loop->has_channel_icon == TRUE));
        cJSON_AddNumberToObject(json_channel, "channel_icon_id", (double)channel_in_loop->icon_id);
        cJSON_AddStringToObject(json_channel, "name", &channel_in_loop->name[0]);
        cJSON_AddStringToObject(json_channel, "password", &channel_in_loop->password[0]);
        cJSON_AddStringToObject(json_channel, "description", &channel_in_loop->description[0]);
        cJSON_AddItemToArray(json_channels, json_channel);
    }

    // rebuild icons (skip the admin icon id 0, which is re-seeded on every start)
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "icons");
    json_icons = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "icons", json_icons);
    for (i = 0; i < MAX_ICONS; i++)
    {
        icon_in_loop = &g_icons_array[i];
        if (icon_in_loop->is_existing == FALSE || icon_in_loop->id == 0)
        {
            continue;
        }

        json_icon = cJSON_CreateObject();
        cJSON_AddNumberToObject(json_icon, "id", icon_in_loop->id);
        cJSON_AddStringToObject(json_icon, "base64", (icon_in_loop->base64 != NULL_POINTER) ? icon_in_loop->base64 : "");
        cJSON_AddItemToArray(json_icons, json_icon);
    }

    // rebuild tags (skip the admin tag id 0, re-seeded on every start)
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "tags");
    json_tags = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "tags", json_tags);
    for (i = 0; i < MAX_TAGS; i++)
    {
        tag_in_loop = &g_tags_array[i];
        if (tag_in_loop->is_existing == FALSE || tag_in_loop->id == ADMIN_TAG_ID)
        {
            continue;
        }

        json_tag = cJSON_CreateObject();
        cJSON_AddNumberToObject(json_tag, "id", tag_in_loop->id);
        cJSON_AddNumberToObject(json_tag, "icon_id", tag_in_loop->icon_id);
        cJSON_AddItemToObject(json_tag, "has_icon", cJSON_CreateBool(tag_in_loop->has_icon == TRUE));
        cJSON_AddStringToObject(json_tag, "name", &tag_in_loop->name[0]);
        cJSON_AddItemToArray(json_tags, json_tag);
    }

    // the admin tag (id 0) itself is re-seeded on every start and skipped by the tags loop above, but its
    // icon IS runtime-editable, so persist just that link (icon id + has_icon) here and re-apply it after
    // the seed on load. without this the admin's chosen icon reverts to the default on every restart
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_tag_icon_id");
    cJSON_AddNumberToObject(json_root, "admin_tag_icon_id", (double)g_tags_array[ADMIN_TAG_ID].icon_id);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_tag_has_icon");
    cJSON_AddItemToObject(json_root, "admin_tag_has_icon", cJSON_CreateBool(g_tags_array[ADMIN_TAG_ID].has_icon == TRUE));

    // rebuild the ban list (matching is by ip at socket open and by identity at join; country/extra data are recorded for the admin)
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "bans");
    json_bans = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "bans", json_bans);
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

    // rebuild the identity store (public-key hash -> tag ids); left untouched when identities are disabled
    _base_internal__serialize_identities(json_root);

    json_text = cJSON_Print(json_root);
    if (json_text == NULL_POINTER)
    {
        cJSON_Delete(json_root);
        return FALSE;
    }

    write_succeeded = base__write_file_atomically("server_settings.json", json_text);
    if (write_succeeded == TRUE)
    {
        log_info("%s", "server settings saved to server_settings.json (general settings, channels, tags)");
    }
    else
    {
        log_info("%s", "save_server_settings: could not write server_settings.json");
    }

    cJSON_free(json_text);
    cJSON_Delete(json_root);

    return write_succeeded;
}

/**
 * @brief enforces per-client spam protection: rejects an action made inside the cooldown window, otherwise records its time and allows it
 *
 * @param uint64 client_index -> id of the client
 *
 * @attention uses write lock on clients_global_rwlock_guard
 *
 * @return boole
 */
boole base__is_request_allowed_based_on_spam_protection(uint64 client_index)
{
    uint64 timestamp_now = 0;
    boole skip = FALSE;

    clib__write_lock(&g_clients_global_rwlock_guard);
    timestamp_now = base__get_timestamp_ms();
    if (timestamp_now < (g_clients_array[client_index].timestamp_last_action + TIMESTAMP_LAST_ACTION_COOLDOWN_MS))
    {
        DBG_CLIENT_MESSAGE log_info("%s", "client_msg__process_change_client_username TIMESTAMP_LAST_ACTION_COOLDOWN_MS \n");
        skip = TRUE;
    }
    if (skip == FALSE)
    {
        g_clients_array[client_index].timestamp_last_action = timestamp_now;
    }

    clib__unlock(&g_clients_global_rwlock_guard);

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
 * @brief returns the current global chat message id (thread-safe)
 *
 * @return uint64 the current chat message id
 */
uint64 base__get_chat_message_id(void)
{
    uint64 to_return = 0;
    pthread_mutex_lock(&g_chat_message_id_mutex);

    to_return = g_chat_message_id;
    pthread_mutex_unlock(&g_chat_message_id_mutex);

    return to_return;
}

/**
 * @brief increments the global chat message id (thread-safe)
 *
 * @return void
 */
void base__increment_chat_message_id(void)
{
    pthread_mutex_lock(&g_chat_message_id_mutex);

    g_chat_message_id++;

    pthread_mutex_unlock(&g_chat_message_id_mutex);
}

/**
 * @brief returns TRUE if a tag with the given id exists
 *
 * @param uint64 tag_id -> id of the tag to check
 *
 * @return boole
 */
boole base__is_tag_id_real(uint64 tag_id)
{
    boole result = FALSE;
    uint64 i = 0;

    result = FALSE;

    for (i = 0; i < MAX_TAGS; i++)
    {
        if (g_tags_array[i].is_existing == FALSE)
        {
            continue;
        }

        if (g_tags_array[i].id == tag_id)
        {
            result = TRUE;
            break;
        }
    }

    return result;
}

/**
 * @brief returns TRUE if the client already has the given tag id
 *
 * @param uint64 client_id -> id of the client
 * @param uint64 this_tag_id -> id of the tag
 *
 * @return boole
 *
 * @attention this function assumes that client_id is correct and that this_tag_id is also correct
 */
boole base__is_client_already_assigned_this_tag_id(uint64 client_id, uint64 this_tag_id)
{
    uint64 cvector_loop_index = 0;
    client_t* client = NULL_POINTER;
    boole is_tag_found = FALSE;

    client = &g_clients_array[client_id];

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
 * @brief returns the index of the given tag id within the client's tag vector, or -1 if not found
 *
 * @param uint64 client_id -> id of the client
 * @param uint64 this_tag_id -> id of the tag
 *
 * @return int64 index of the tag id, or -1 if the client does not have it
 *
 * @attention this function assumes that client_id is correct and that this_tag_id is also correct
 */
int64 base__get_index_of_tag_id_of_client(uint64 client_id, uint64 this_tag_id)
{
    uint64 cvector_loop_index = 0;
    client_t* client = NULL_POINTER;
    int64 tag_index = -1;

    client = &g_clients_array[client_id];

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
 * @param uint64 client_index -> id of the client whose connection to close
 * @param boole use_readlock -> TRUE to acquire the read lock inside this function
 *
 * @return void
 *
 * @attention closing connection takes more lines of code, and this function was created to eliminate that.
 */
void base__close_websocket_connection(uint64 client_index, boole use_readlock)
{
    if (use_readlock == TRUE)
    {
        clib__read_lock(&g_clients_global_rwlock_guard);
    }
    if (g_clients_array[client_index].is_existing == TRUE)
    {
        if (g_clients_array[client_index].is_authenticated == TRUE)
        {
            DBG_CLOSE_CONNECTION log_info("%s %llu %s", "base__close_websocket_connection closing with authenticated client", client_index, "\n");
            ws_close_client(g_clients_array[client_index].p_ws_connection);
        }
        else
        {
            DBG_CLOSE_CONNECTION log_info("%s %llu %s", "base__close_websocket_connection client with not authenticated client", client_index, "\n");
            ws_close_client(g_clients_array[client_index].p_ws_connection);
        }
    }
    else
    {
        DBG_CLOSE_CONNECTION log_info("%s %llu %s", "base__close_websocket_connection client doesnt exist", client_index, "\n");
    }

    if (use_readlock == TRUE)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
    }
}

/**
 * @brief counts the authenticated, non-music-bot clients in the given channel
 *
 * @param uint64 channel_id -> id of the channel
 *
 * @return uint64 number of clients in the channel
 *
 */
uint64 base__get_client_count_for_channel(uint64 channel_id)
{
    uint64 x = 0;
    client_t* client = NULL_POINTER;
    uint64 result = 0;

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

        if (client->channel_id == channel_id)
        {
            result = result + 1;
        }
    }

    return result;
}

/**
 * @brief recursively marks the child channels of the given channel for deletion
 *
 * @param uint64 channel_id -> id of the channel to start marking from
 * @param uint64* out_current_index -> in/out count of marked channels, must not be NULL
 * @param uint64* out_channel_indices -> array receiving the marked channel ids, must not be NULL
 *
 * @return void
 */
void base__mark_channels_for_deletion(uint64 channel_id, uint64* out_current_index, uint64* out_channel_indices)
{
    uint64 i = 0;

    if (out_current_index == NULL_POINTER || out_channel_indices == NULL_POINTER)
    {
        return;
    }

    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        if (channel_id == g_channel_array[i].parent_channel_id)
        {
            out_channel_indices[*out_current_index] = i;
            *out_current_index += 1;
            DBG_CLIENT_MESSAGE log_info("%s %s %s", "mark_channels_for_deletion need to remove channel ", g_channel_array[i].name, " \n");
            base__mark_channels_for_deletion(g_channel_array[i].channel_id, out_current_index, out_channel_indices);
        }
    }
}

/**
 * @brief returns TRUE if the given public key is present in the stored client data
 *
 * @param char* public_key -> the public key to look for
 *
 * @return boole
 */
boole base__is_public_key_present_in_client_stored_data(char* public_key)
{
    boole result = FALSE;
    boole status = FALSE;
    uint64 i = 0;

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        status = clib__is_string_equal(g_client_stored_data[i].public_key, public_key);
        if (status == TRUE)
        {
            log_info("%s %s %s %s", "public key found in client stored data ", g_client_stored_data[i].public_key, public_key, "\n");
            result = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return result;
}

/**
 * @brief finds new index of maintainer of channel
 *
 * @param uint64* _out__new_index_of_maintainer -> receives the chosen maintainer's client id
 * @param uint64 channel_id -> id of the channel needing a new maintainer
 * @param uint64 index_of_client_that_left -> id of the client that just left
 * @param boole do_not_include_client_that_left_when_searching_for_new_maintainer -> TRUE to skip the client that left
 *
 * @note this is used function in situation where client leaves the channel for whatever reason and he happens to be the maintainer of it
 *
 * @return boole -> TRUE if a maintainer was found and written to _out__new_index_of_maintainer, FALSE if not
 *
 * @attention bad code
 */
boole base__find_new_maintainer_for_channel(uint64* _out__new_index_of_maintainer, uint64 channel_id, uint64 index_of_client_that_left, boole do_not_include_client_that_left_when_searching_for_new_maintainer)
{
    uint64 i = 0;
    client_t* client = NULL_POINTER;
    boole maintainer_found = FALSE;
    uint64* possible_new_maintainers = 0;
    uint64 possible_new_maintainers_count = 0;
    int random_index = 0;

    possible_new_maintainers = (uint64*)memorymanager__allocate(sizeof(uint64) * g_server_settings.max_client_count, MEMALLOC_FIND_MAINTAINER);
    // maintainer will be randomly chosen
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

        if (do_not_include_client_that_left_when_searching_for_new_maintainer == TRUE)
        {
            if (i == index_of_client_that_left)
            {
                continue;
            }
        }

        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__find_new_maintainer_for_channel client hopped on is: ", i, "\n");

        maintainer_found = TRUE;

        possible_new_maintainers[possible_new_maintainers_count] = i;
        possible_new_maintainers_count++;
    }

    if (maintainer_found == TRUE)
    {
        random_index = (int)(rand() % (possible_new_maintainers_count));
        *_out__new_index_of_maintainer = possible_new_maintainers[random_index];
        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__find_new_maintainer_for_channel random chosen maintainer index", *_out__new_index_of_maintainer, "\n");
    }
    else
    {
        DBG_CLIENT_DISCONNECT log_info("%s ", "base__find_new_maintainer_for_channel maintainer not found \n");
    }

    memorymanager__free((nuint)possible_new_maintainers);

    return maintainer_found;
}

/**
 * @brief finds the music bot client in the given channel, if one exists
 *
 * @param uint64 channel_id -> id of the channel to search
 *
 * @return client_t* the music bot client, or NULL_POINTER if the channel has none
 */
client_t* base__find_music_bot_in_channel(uint64 channel_id)
{
    uint64 i = 0;
    client_t* client_in_loop = NULL_POINTER;

    client_t* result = NULL_POINTER;

    // maintainer will be randomly chosen
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
 * @param uint64 client_index -> index of client that needs new username
 * @param cstring default_name -> default part of name string
 *
 * @return boole returns true if username has been assigned, false if not. username is assigned within this function, to avoid need allocating on heap (stack returning meh)
 *
 * @attention bad code
 */
boole base__assign_username_for_newly_joined_client(uint64 client_index, cstring default_name)
{
    boole status = FALSE;
    char* public_key = 0;
    char candidate_username[USERNAME_MAX_LENGTH];
    char index_suffix_buffer[16];
    uint64 username_suffix_number = 0;
    uint64 i = 0;
    boole result = FALSE;

    public_key = &g_clients_array[client_index].public_key[0];
    DBG_AUTHENTICATION log_info("%s %s %s", "newly joined client public key is -> ", public_key, "\n");

    status = base__is_public_key_present_in_client_stored_data(public_key);

    if (status == TRUE)
    {
        DBG_AUTHENTICATION log_info("%s", "public key present \n");
    }
    else
    {
        DBG_AUTHENTICATION log_info("%s", "public key not present \n");
        // two assumptions for safe operation
        // default name won't be too long
        // max number of clients will fit into 16 digits (easy), edge cases will have 3 digits, extremely edge cases 4

        // try "<default_name><n>" for n = 0, 1, 2, ... until a username no client is using is found
        for (username_suffix_number = 0; username_suffix_number < g_server_settings.max_client_count; username_suffix_number++)
        {
            uint64 default_name_length = 0;

            clib__null_memory(candidate_username, USERNAME_MAX_LENGTH);
            clib__copy_memory(g_server_settings.default_client_name, candidate_username, clib__utf8_string_length(g_server_settings.default_client_name), USERNAME_MAX_LENGTH);

            // as long as I know what I'm doing, shouldn't be dangerous
            clib__null_memory(index_suffix_buffer, 16);

            // itoa(username_suffix_number, index_suffix_buffer, 10); gcc on linux does not support itoa so I'm using sprintf
            sprintf(index_suffix_buffer, "%llu", username_suffix_number);

            default_name_length = clib__utf8_string_length(candidate_username);
            clib__copy_memory(&index_suffix_buffer[0], &candidate_username[default_name_length], clib__utf8_string_length(index_suffix_buffer), clib__utf8_string_length(g_server_settings.default_client_name));

            DBG_AUTHENTICATION log_info("%s %s %s", "index_suffix_buffer -> ", index_suffix_buffer, "\n");
            DBG_AUTHENTICATION log_info("%s %s %s", "candidate_username -> ", candidate_username, "\n");

            for (i = 0; i < g_server_settings.max_client_count; i++)
            {
                // is_existing needs to be guarded with mutex, while opening client, rwlock is not usable

                // log_info("%s %s %d %s", "trying username -> ", candidate_username , i, "\n");

                if (g_clients_array[i].is_existing == FALSE)
                { // client not is_existing, skip, this needs global lock
                    goto final_check;
                }

                if (g_clients_array[i].is_authenticated == FALSE)
                {
                    goto final_check;
                }

                if (i == client_index)
                { // skip current client
                    goto final_check;
                }

                status = clib__is_string_equal(g_clients_array[i].username, candidate_username);

                if (status == TRUE)
                { // username used by some of the clients, start another loop, with incremented numeric part of client's username
                    DBG_AUTHENTICATION log_info("%s %s %s", "username ", candidate_username, " it is not available \n");
                    break;
                }

final_check:

                if ((i + 1) == g_server_settings.max_client_count)
                { // if loop reached its end and username currently used in this loop was found to not be used by any of the clients
                    // go to end_loop where this newly found username will be assigned to client
                    DBG_AUTHENTICATION log_info("%s", "(i + 1) == g_server_settings.max_client_count goto end_loop \n");
                    result = TRUE;
                    goto end_loop;
                }

                // still some clients that need to be checked, not needed , included just for
            }
        }
    }

    return result; // either code jumps to end_loop or it returns false here

end_loop:
    clib__copy_memory(&candidate_username[0], &g_clients_array[client_index].username[0], clib__utf8_string_length(candidate_username), USERNAME_MAX_LENGTH);
    DBG_AUTHENTICATION log_info("%s %llu %s %s", "client: ", client_index, g_clients_array[client_index].username, "\n");

    return result;
}

/**
 * @brief finds the index of the first free client slot, or -1 if the server is full
 *
 * @return int64 free client index, or -1 if none available
 */
int64 base__get_new_index_for_client(void)
{
    int64 new_index = -1;
    uint64 i = 0;

    if ((g_server_settings.client_count + 1) < g_server_settings.max_client_count)
    {
        for (i = 0; i < g_server_settings.max_client_count; i++)
        {
            if (g_clients_array[i].timestamp_connected == 0)
            {
                new_index = i;
                break;
            }
        }
    }

    return new_index;
}

/**
 * @brief fills a buffer with cryptographically secure random bytes
 *
 * @param unsigned char* out_buffer -> destination buffer to fill
 * @param uint64 length -> number of random bytes to write (must not exceed the CTR_DRBG per-call max)
 *
 * @return boole -> TRUE on success, FALSE if the RNG could not be seeded or failed
 *
 * @note use this for all key material and authentication nonces; never use rand(), which is a
 *       predictable PRNG seeded from the clock (srand(time(0))) and is trivially reconstructable
 */
boole base__fill_secure_random_bytes(unsigned char* out_buffer, uint64 length)
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char* pers = "lemonchat_csprng";
    int ret = 0;
    boole result = TRUE;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers));
    if (ret != 0)
    {
        DBG_ENCRYPTION log_info("%s %d", "base__fill_secure_random_bytes: ctr_drbg_seed failed ", ret);
        result = FALSE;
    }

    if (result == TRUE)
    {
        ret = mbedtls_ctr_drbg_random(&ctr_drbg, out_buffer, length);
        if (ret != 0)
        {
            DBG_ENCRYPTION log_info("%s %d", "base__fill_secure_random_bytes: ctr_drbg_random failed ", ret);
            result = FALSE;
        }
    }

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return result;
}

/**
 * @brief fills a block of memory with cryptographically secure random printable ASCII characters
 *
 * @param char* block -> the buffer to fill
 * @param uint64 length -> number of bytes to fill
 *
 * @return boole -> TRUE on success, FALSE if the RNG could not be seeded or failed (block left unchanged)
 *
 * @note used for the RSA ownership challenge; the bytes must be unpredictable, so they come from a
 *       CSPRNG and not rand() - a predictable challenge would let an impostor answer it without the
 *       private key. Folding a uniform byte into the 94-symbol printable range adds a negligible modulo
 *       bias that does not affect unpredictability. The caller MUST treat FALSE as a hard failure.
 */
boole base__fill_block_of_data_with_ascii_characters(char* block, uint64 length)
{
    uint64 lowerbound = 33;
    uint64 upperbound = 126;
    uint64 i = 0;

    if (base__fill_secure_random_bytes((unsigned char*)block, length) == FALSE)
    {
        return FALSE;
    }

    for (i = 0; i < length; i++)
    {
        block[i] = (char)((unsigned char)block[i] % (upperbound - lowerbound + 1)) + lowerbound;
    }

    return TRUE;
}

/**
 * @brief frees some artifacts from working with json, used mostly in server_message.c
 *
 * @param cJSON* json_root_object1 -> the cJSON object to delete
 * @param char* json_root_object1_string -> the serialized string to free
 *
 * @return void
 *
 * @attention Function not really nessecary
 */
void base__free_json_message(cJSON* json_root_object1, char* json_root_object1_string)
{
    if (json_root_object1 != NULL_POINTER)
    {
        // log_info("base__free_json_message() cJSON_Delete(json_root_object1); \n");
        cJSON_Delete(json_root_object1);
    }

    if (json_root_object1_string != NULL_POINTER)
    {
        free(json_root_object1_string);
    }
}

/**
 * @brief returns timestamp in milliseconds
 *
 *        on windows this is GetTickCount64 (milliseconds since boot), everywhere else it is
 *        gettimeofday (milliseconds since the unix epoch). only differences between two readings
 *        are meaningful, the absolute value is not comparable across platforms.
 *
 * @attention should work on windows and linux
 *
 * @return uint64 -> the current timestamp in milliseconds
 */
uint64 base__get_timestamp_ms(void)
{
#ifdef WIN32
    uint64 timestamp_msec = GetTickCount64();
    return timestamp_msec;
#else
    // every non-Windows platform (Linux, macOS, BSD) has POSIX gettimeofday. this MUST have an
    // #else, not an #ifdef __linux__: on macOS __linux__ is undefined, so a linux-only branch left
    // the function with no return - it fell off the end and returned garbage, which made every
    // spam-protected request (channel create/join/delete, chat, etc.) compare against a bogus
    // "now" and get silently rejected while auth (no spam check) still worked
    struct timeval tv;
    uint64 timestamp_msec = 0;
    gettimeofday(&tv, NULL_POINTER);
    timestamp_msec = (uint64)tv.tv_sec * 1000 + (uint64)tv.tv_usec / 1000;
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

void base__sleep_for_milliseconds(uint64 milliseconds)
{ // cross-platform sleep function
#ifdef WIN32
    Sleep(milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL_POINTER);
#else
    if (milliseconds >= 1000)
    {
        sleep(milliseconds / 1000);
    }
    usleep((milliseconds % 1000) * 1000);
#endif
}

/**
 * @brief returns TRUE if an authenticated client with the same public key already exists
 *
 * @param cstring public_key -> the public key to look for
 *
 * @return boole
 *
 * @attention output of this function is stored on heap, and must be freed manually
 */
/**
 * @brief drops every OTHER connected client holding this public key, so a returning identity
 *        replaces its own stale session instead of being locked out by it
 *
 * @param uint64 client_index_to_keep -> the client that just proved it owns the key
 * @param cstring public_key -> the public key both sessions carry
 *
 * @return uint64 -> how many stale sessions were closed
 *
 * @attention call ONLY after the challenge response verified ownership: the public key is public,
 *            so acting on it any earlier would let anyone kick anyone off the server.
 *            the caller must hold the clients write lock
 */
uint64 base__disconnect_other_clients_with_same_public_key(uint64 client_index_to_keep, cstring public_key)
{
    uint64 disconnected_count = 0;
    uint64 i = 0;

    if (public_key == NULL_POINTER || public_key[0] == 0)
    {
        return 0;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_index_to_keep)
        {
            continue;
        }

        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == FALSE)
        {
            continue;
        }

        // the socket may be long dead (a drop the server never saw); closing it makes that
        // client's own reader thread run the normal disconnect teardown
        log_info("%s %llu %s %llu %s", "identity takeover: client", i, "is replaced by the returning client", client_index_to_keep, "\n");
        server_logs__client_disconnect_reason(&g_clients_array[i], "identity takeover");
        base__close_websocket_connection(i, FALSE);

        // torn down right here, not when the socket dies: the returning client's list and the
        // disconnect broadcast must both precede its join, so nobody ever sees both sessions
        clib__write_lock(&g_channels_global_rwlock_guard);
        base__process_client_disconnect(i);
        clib__unlock(&g_channels_global_rwlock_guard);
        disconnected_count++;
    }

    return disconnected_count;
}

/**
 * @brief fast reconnect: hands the new socket to this identity's still-open session, so the session keeps
 *        its id, channel, name, tags and roles. the old socket is closed without any broadcast
 *
 * @param uint64 new_client_index -> the entry the new socket got at onopen; emptied on success
 * @param cstring public_key -> the key the new socket just proved it owns
 *
 * @return int64 -> index of the adopted session, -1 when this key has no open session
 *
 * @attention the caller must hold the clients write lock. same rule as the takeover: only after the
 *            challenge response verified ownership, the public key alone proves nothing
 */
int64 base__adopt_socket_into_existing_session(uint64 new_client_index, cstring public_key)
{
    uint64 i = 0;
    uint64 timestamp_now = 0;
    client_t* old_session = NULL_POINTER;
    client_t* new_entry = NULL_POINTER;
    ws_cli_conn_t* old_ws_connection = NULL_POINTER;

    if (public_key == NULL_POINTER || public_key[0] == 0)
    {
        return -1;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == new_client_index || g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == TRUE)
        {
            old_session = &g_clients_array[i];
            break;
        }
    }

    if (old_session == NULL_POINTER)
    {
        return -1;
    }

    new_entry = &g_clients_array[new_client_index];
    old_ws_connection = old_session->p_ws_connection;
    timestamp_now = base__get_timestamp_ms();

    // the per-socket half moves over; the session half (id, channel, name, tags, roles, avatar) stays
    old_session->p_ws_connection = new_entry->p_ws_connection;
    old_session->is_dh_shared_secret_agreed_upon = new_entry->is_dh_shared_secret_agreed_upon;
    old_session->is_public_key_challenge_sent = new_entry->is_public_key_challenge_sent;
    clib__copy_memory(new_entry->dh_shared_secret, old_session->dh_shared_secret, SHARED_SECRET_LENGTH, SHARED_SECRET_LENGTH);
    clib__copy_memory(new_entry->challenge_string, old_session->challenge_string, CHALLENGE_STRING_LENGTH, CHALLENGE_STRING_LENGTH);
    clib__null_memory(old_session->ip_address, sizeof(old_session->ip_address));
    clib__copy_memory(new_entry->ip_address, old_session->ip_address, clib__utf8_string_length(new_entry->ip_address), sizeof(old_session->ip_address));
    clib__null_memory(old_session->country_iso_code, sizeof(old_session->country_iso_code));
    clib__copy_memory(new_entry->country_iso_code, old_session->country_iso_code, clib__utf8_string_length(new_entry->country_iso_code), sizeof(old_session->country_iso_code));
    old_session->timestamp_last_maintain_connection_message_received = timestamp_now;
    old_session->timestamp_last_action = timestamp_now;
    old_session->is_idle = FALSE;
    old_session->has_pending_maintainer_reset_vote = FALSE;

    // an upload that was in flight on the old socket died with it
    if (old_session->file_upload_extension.file_upload_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)old_session->file_upload_extension.file_upload_buffer);
    }
    clib__null_memory(&old_session->file_upload_extension, sizeof(old_session->file_upload_extension));

    // the throwaway entry owns nothing on the heap this early in the handshake; mirrored from the
    // disconnect path anyway, so a later change there cannot turn this into a leak
    if (new_entry->tag_ids != NULL_POINTER)
    {
        cvector_free(new_entry->tag_ids);
    }
    if (new_entry->base64_avatar != NULL_POINTER)
    {
        memorymanager__free((nuint)new_entry->base64_avatar);
    }
    if (new_entry->file_upload_extension.file_upload_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)new_entry->file_upload_extension.file_upload_buffer);
    }
    clib__null_memory(new_entry, sizeof(client_t));

    log_info("%s %llu %s %llu %s", "fast reconnect: session", i, "adopted the socket of entry", new_client_index, "\n");

    // closed like any other socket; its late onclose finds no entry holding this pointer
    ws_close_client(old_ws_connection);

    return (int64)i;
}

/**
 * @brief tells whether some OTHER authenticated client holds this public key (identity takeover off:
 *        such a newcomer is refused instead of replacing that session)
 *
 * @param uint64 client_index -> the newcomer, skipped
 * @param cstring public_key -> the key it just proved
 *
 * @return boole -> TRUE when another authenticated session carries the same key
 */
boole base__is_there_another_authenticated_client_with_same_public_key(uint64 client_index, cstring public_key)
{
    uint64 i = 0;

    if (public_key == NULL_POINTER || public_key[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_index || g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

boole base__is_there_a_client_with_same_public_key(cstring public_key)
{
    boole result = FALSE;
    uint64 i = 0;
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE)
        {
            continue;
        }

        if (g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key))
        {
            result = TRUE;
            break;
        }
    }
    return result;
}

/**
 * @brief the socket-open half of the same-ip rule: only one handshake at a time per ip, so a flood
 *        from one address costs one slot and not the whole table, while an established session never counts
 *
 * @param cstring ip_address -> ip address of the socket that just opened
 *
 * @return boole
 *
 * @attention the caller must hold the clients write lock
 */
boole base__is_there_an_unfinished_handshake_from_same_ip_address(cstring ip_address)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == TRUE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].ip_address, ip_address) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief the login half of the same-ip rule, judged only once the challenge proved the identity, so
 *        a returning client is refused by a second person on his ip and never by his own dropped session
 *
 * @param uint64 client_index -> the client being judged, never counted
 * @param cstring ip_address -> his ip address
 * @param cstring public_key -> his identity, older sessions holding it are the ghosts being disconnected and are skipped
 *
 * @return boole
 *
 * @attention the caller must hold the clients write lock. only authenticated sessions count: an unfinished
 *            handshake from the same ip is judged when it gets here itself, which lets exactly one of them in
 */
boole base__is_there_another_authenticated_client_with_same_ip_address(uint64 client_index, cstring ip_address, cstring public_key)
{
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (i == client_index || g_clients_array[i].is_existing == FALSE || g_clients_array[i].is_authenticated == FALSE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].public_key, public_key) == TRUE)
        {
            continue;
        }

        if (clib__is_string_equal(g_clients_array[i].ip_address, ip_address) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief derives an AES encryption key and an HMAC key from the already-established shared secret via HKDF-SHA256 (RFC 5869)
 *
 * @param char* dh_shared_secret -> the shared secret as a decimal string (HKDF input keying material)
 * @param unsigned char* out_enc_key -> receives the 32-byte AES-256 encryption key (OKM bytes 0..31)
 * @param unsigned char* out_mac_key -> receives the 32-byte HMAC-SHA256 key (OKM bytes 32..63)
 *
 * @return boole -> TRUE on success, FALSE if HKDF failed
 *
 * @note salt/info are fixed labels that MUST match the client's HKDF (js side), or decryption breaks
 */
static boole _base_internal__derive_keys_from_shared_secret(char* dh_shared_secret, unsigned char* out_enc_key, unsigned char* out_mac_key)
{
    const mbedtls_md_info_t* md = NULL_POINTER;
    unsigned char okm[64];
    int ret = 0;
    static const char* hkdf_salt = "lemonchat-hkdf-salt-v1";
    static const char* hkdf_info = "lemonchat-dh-keys-v1";

    clib__null_memory(okm, sizeof(okm));

    md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL_POINTER)
    {
        return FALSE;
    }

    ret = mbedtls_hkdf(md, (const unsigned char*)hkdf_salt, strlen(hkdf_salt), (const unsigned char*)dh_shared_secret, strlen(dh_shared_secret), (const unsigned char*)hkdf_info, strlen(hkdf_info), okm, sizeof(okm));
    if (ret != 0)
    {
        return FALSE;
    }

    clib__copy_memory(okm, out_enc_key, 32, 32);
    clib__copy_memory(okm + 32, out_mac_key, 32, 32);
    return TRUE;
}

/**
 * @brief computes the encrypt-then-MAC tag: HMAC-SHA256(mac_key, iv_base64 || data_base64)
 *
 * @param unsigned char* mac_key -> the 32-byte HMAC key from _base_internal__derive_keys_from_shared_secret
 * @param char* iv_base64 -> the base64 IV string (first part of the MAC input)
 * @param char* data_base64 -> the base64 ciphertext string (second part of the MAC input)
 * @param unsigned char* out_tag -> receives the 32-byte HMAC tag
 *
 * @return boole -> TRUE on success, FALSE if the HMAC failed
 */
static boole _base_internal__compute_metadata_tag(unsigned char* mac_key, char* iv_base64, char* data_base64, unsigned char* out_tag)
{
    const mbedtls_md_info_t* md = NULL_POINTER;
    mbedtls_md_context_t md_ctx;
    int ret = 0;

    md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL_POINTER)
    {
        return FALSE;
    }

    mbedtls_md_init(&md_ctx);
    ret = mbedtls_md_setup(&md_ctx, md, 1);
    if (ret == 0)
    {
        ret = mbedtls_md_hmac_starts(&md_ctx, mac_key, 32);
    }
    if (ret == 0)
    {
        ret = mbedtls_md_hmac_update(&md_ctx, (const unsigned char*)iv_base64, strlen(iv_base64));
    }
    if (ret == 0)
    {
        ret = mbedtls_md_hmac_update(&md_ctx, (const unsigned char*)data_base64, strlen(data_base64));
    }
    if (ret == 0)
    {
        ret = mbedtls_md_hmac_finish(&md_ctx, out_tag);
    }
    mbedtls_md_free(&md_ctx);

    return (ret == 0) ? TRUE : FALSE;
}

/**
 * @brief constant-time equality for two null-terminated strings (used to compare base64 auth tags)
 *
 * @param char* a -> first string
 * @param char* b -> second string
 *
 * @return boole -> TRUE if equal, FALSE otherwise
 *
 * @note length is compared first (lengths are not secret); the byte loop avoids early-out timing leaks
 */
static boole _base_internal__constant_time_str_equal(char* a, char* b)
{
    uint64 length_a = 0;
    uint64 length_b = 0;
    uint64 i = 0;
    unsigned char diff = 0;

    length_a = clib__utf8_string_length(a);
    length_b = clib__utf8_string_length(b);
    if (length_a != length_b)
    {
        return FALSE;
    }

    for (i = 0; i < length_a; i++)
    {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }

    return (diff == 0) ? TRUE : FALSE;
}

/**
 * @brief SHA256-hashes a password then base64-encodes it, so auth secrets can be stored without plaintext.
 *
 * @param char* plaintext -> the password to hash
 * @param char* out -> caller buffer for the base64(SHA256) text (44 chars + NUL)
 * @param int64 out_size -> size of out in bytes (must be >= 45)
 *
 * @return void
 */
void base__hash_password_to_base64(char* plaintext, char* out, int64 out_size)
{
    ITH_SHA256_CTX ctx;
    unsigned char digest[32];
    char encoded[BASE64_ENCODE_OUT_SIZE(32)];

    ith_sha256_init(&ctx);
    ith_sha256_update(&ctx, (unsigned char* )plaintext, strlen(plaintext));
    ith_sha256_final(&ctx, digest);

    clib__null_memory(encoded, sizeof(encoded));
    zchg_base64_encode(digest, 32, encoded);

    clib__null_memory(out, out_size);
    clib__copy_memory(encoded, out, clib__utf8_string_length(encoded), out_size - 1);
}

/**
 * @brief checks a submitted plaintext password against a stored base64(SHA256) hash.
 *
 * @param char* plaintext -> the submitted password
 * @param char* stored_base64_hash -> the stored base64(SHA256) to compare against
 *
 * @return boole -> TRUE when they match
 */
boole base__password_matches(char* plaintext, char* stored_base64_hash)
{
    char computed[BASE64_ENCODE_OUT_SIZE(32)];

    base__hash_password_to_base64(plaintext, computed, sizeof(computed));
    return clib__is_string_equal(computed, stored_base64_hash);
}

/**
 * @brief restores a re-authenticated client's tags from the persisted identity store. the store is keyed by
 *        base64(SHA256(public_key)) so the settings file never holds the raw public key. the admin tag
 *        (id 0) is restored like any other tag and re-grants admin. only meaningful when identities are
 *        enabled; the caller checks that.
 *
 * @param client_t* client -> the freshly authenticated client whose tags should be restored
 *
 * @note the caller must hold the clients write lock (client->tag_ids and is_admin are written) and the tags
 *       read lock (g_tags_array is read). the store itself is guarded by g_client_stored_data_mutex.
 *
 * @return void
 */
void base__restore_identity_tags(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 t = 0;
    uint64 tag_id = 0;
    boole match_found = FALSE;
    uint64 restored_count = 0;

    if (client == NULL_POINTER)
    {
        DBG_IDENTITIES log_info("%s", "restore_identity_tags: client is NULL, nothing to restore \n");
        return;
    }

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    DBG_IDENTITIES log_info("%s %llu %s %s %s", "restore_identity_tags: client_id", client->client_id, "connecting with identity hash [", identity_hash, "] \n");

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == FALSE)
        {
            DBG_IDENTITIES log_info("%s %llu %s %s %s", "restore_identity_tags: store slot", i, "holds a different identity [", g_client_stored_data[i].public_key, "] \n");
            continue;
        }

        match_found = TRUE;
        DBG_IDENTITIES log_info("%s %llu %s %llu %s", "restore_identity_tags: MATCH in store slot", i, "with", (uint64)g_client_stored_data[i].tag_id_count, "stored tag ids \n");

        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            tag_id = g_client_stored_data[i].tag_ids[t];

            // keep the admin tag (id 0) always; for every other tag, skip it if it no longer exists
            if (tag_id != ADMIN_TAG_ID && (tag_id >= MAX_TAGS || g_tags_array[tag_id].is_existing == FALSE))
            {
                DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_tags: skipping stored tag id", tag_id, "- it no longer exists on the server \n");
                continue;
            }

            cvector_push_back(client->tag_ids, (int)tag_id);
            restored_count++;
            DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_tags: restored tag id", tag_id, "\n");

            if (tag_id == ADMIN_TAG_ID)
            {
                client->is_admin = TRUE;
                DBG_IDENTITIES log_info("%s", "restore_identity_tags: admin tag restored -> client re-granted admin \n");
            }
        }

        break;
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    if (match_found == FALSE)
    {
        DBG_IDENTITIES log_info("%s %s %s", "restore_identity_tags: NO stored identity matched hash [", identity_hash, "] - client starts with no restored tags (wrong passphrase, or the identity was never saved) \n");
    }
    else
    {
        DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_tags: done, restored", restored_count, "tag(s) \n");
    }
}

/**
 * @brief restores a reconnecting identity's persisted avatar (matched by public-key hash) into the live
 *        client, so it can be served to others immediately without waiting for a re-upload. kept separate
 *        from tag restore so it can run when avatars are allowed even if tag-identities are disabled.
 *
 * @param client_t* client -> the just-authenticated client
 *
 * @note takes g_client_stored_data_mutex (a leaf lock). call with the clients lock held (it writes
 *       client->base64_avatar, a heap block freed on disconnect).
 *
 * @return void
 */
void base__restore_identity_avatar(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;
    uint64 avatar_len = 0;

    if (client == NULL_POINTER || client->public_key[0] == 0)
    {
        return;
    }

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            if (g_client_stored_data[i].base64_avatar[0] != 0)
            {
                avatar_len = clib__utf8_string_length(&g_client_stored_data[i].base64_avatar[0]);

                if (client->base64_avatar != NULL_POINTER)
                {
                    memorymanager__free((nuint)client->base64_avatar);
                    client->base64_avatar = NULL_POINTER;
                }

                client->base64_avatar = (char*)memorymanager__allocate(avatar_len + 1, MEMALLOC_AVATAR);
                if (client->base64_avatar != NULL_POINTER)
                {
                    clib__copy_memory(&g_client_stored_data[i].base64_avatar[0], client->base64_avatar, avatar_len, avatar_len);
                    DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_avatar: restored avatar for client_id", client->client_id, "\n");
                }
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief restores an identity's admin-registered alias (by public-key hash) into the live client.
 *        fixed-size copy, no allocation. does nothing when no alias is stored.
 *
 * @param client_t* client -> the just-authenticated client whose alias is being restored
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return void
 */
void base__restore_identity_alias(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 i = 0;

    if (client == NULL_POINTER || client->public_key[0] == 0)
    {
        return;
    }

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            if (g_client_stored_data[i].alias[0] != 0)
            {
                clib__null_memory(&client->alias[0], USERNAME_MAX_LENGTH);
                clib__copy_memory(&g_client_stored_data[i].alias[0], &client->alias[0], clib__utf8_string_length(&g_client_stored_data[i].alias[0]), USERNAME_MAX_LENGTH - 1);
                DBG_IDENTITIES log_info("%s %llu %s", "restore_identity_alias: restored alias for client_id", client->client_id, "\n");
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief snapshots every connected, authenticated client that currently owns at least one tag into the
 *        in-memory identity store, keyed by base64(SHA256(public_key)).
 *
 *        an existing entry for that hash is overwritten; otherwise the first free slot is used. called at
 *        "save server settings" time so the store mirrors the tags people currently wear, without
 *        persisting on every tag change. does nothing when identities are disabled. while offline
 *        messages are enabled it also captures the raw public key of every connected registered client,
 *        before the tagless skip, so keys are not missed for users who wear no tags.
 *
 * @note the caller must hold the clients read lock (g_clients_array and each client->tag_ids are read).
 *       the store itself is guarded by g_client_stored_data_mutex.
 *
 * @return void
 */
void base__snapshot_connected_clients_into_identity_store(void)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    client_t* client = NULL_POINTER;
    uint64 client_index = 0;
    uint64 tag_count = 0;
    uint64 t = 0;
    uint64 j = 0;
    int64 slot = 0;

    if (g_server_settings.are_identities_enabled == FALSE)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (client_index = 0; client_index < g_server_settings.max_client_count; client_index++)
    {
        client = &g_clients_array[client_index];

        if (client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
        {
            continue;
        }

        // an admin save is when the file is brought up to date, so collect the raw public keys of
        // everybody connected right now - BEFORE the tagless skip below. capturing only at
        // authentication missed every identity that was already connected when the feature was
        // switched on, and a registered user with no tags (the normal case) was skipped entirely,
        // so the key never reached the file at all
        if (g_server_settings.allow_offline_messages == TRUE && client->is_registered == TRUE && client->public_key[0] != 0)
        {
            char offline_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
            base__hash_password_to_base64(client->public_key, offline_identity_hash, sizeof(offline_identity_hash));

            // the store mutex is already held here; the by-hash helper takes it too, so write
            // through the slot directly instead of calling it (that would self-deadlock)
            for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
            {
                if (clib__is_string_equal(g_client_stored_data[j].public_key, offline_identity_hash) == TRUE)
                {
                    clib__null_memory(&g_client_stored_data[j].raw_public_key[0], MAX_PUBLIC_KEY_LENGTH);
                    clib__copy_memory(client->public_key, &g_client_stored_data[j].raw_public_key[0], clib__utf8_string_length(client->public_key), MAX_PUBLIC_KEY_LENGTH - 1);
                    break;
                }
            }
        }

        tag_count = cvector_size(client->tag_ids);
        if (tag_count == 0)
        {
            DBG_IDENTITIES log_info("%s %llu %s", "snapshot_identities: client_id", client->client_id, "has no tags -> not stored \n");
            continue;
        }

        base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

        // find an existing entry for this identity, else the first free slot
        slot = -1;
        for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
        {
            if (clib__is_string_equal(g_client_stored_data[j].public_key, identity_hash) == TRUE)
            {
                slot = (int64)j;
                break;
            }
        }
        if (slot == -1)
        {
            for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
            {
                if (g_client_stored_data[j].public_key[0] == 0)
                {
                    slot = (int64)j;
                    break;
                }
            }
        }
        if (slot == -1)
        {
            DBG_IDENTITIES log_info("%s %llu %s", "snapshot_identities: identity store FULL, cannot store client_id", client->client_id, "\n");
            continue; // store is full
        }

        // rewrite the identity fields but PRESERVE base64_avatar and alias - nulling the whole slot
        // would wipe them (sync_client_identity_in_store already preserves them the same way)
        clib__null_memory(&g_client_stored_data[slot].public_key[0], MAX_PUBLIC_KEY_LENGTH);
        clib__null_memory(&g_client_stored_data[slot].username[0], USERNAME_MAX_LENGTH);
        clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
        clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); // remember the last-seen username for the admin ui

        g_client_stored_data[slot].tag_id_count = 0;
        for (t = 0; t < tag_count && t < MAX_TAGS_FOR_SINGLE_CLIENT; t++)
        {
            g_client_stored_data[slot].tag_ids[g_client_stored_data[slot].tag_id_count] = (uint64)client->tag_ids[t];
            g_client_stored_data[slot].tag_id_count++;
        }

        DBG_IDENTITIES log_info("%s %llu %s %lld %s %llu %s %s %s", "snapshot_identities: stored client_id", client->client_id, "into slot", slot, "with", (uint64)g_client_stored_data[slot].tag_id_count, "tag(s), hash [", identity_hash, "] \n");
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief immediately mirrors ONE client's current tags into the in-memory identity store, so a
 *        reconnecting client gets its tags back within the same server run WITHOUT waiting for an
 *        admin "save server settings" (that save only adds disk persistence on top).
 *
 *        called right after any tag change on a client. if the client has tags they overwrite (or
 *        create) its store entry; if it now has zero tags its entry is cleared, so a fully-untagged
 *        identity is forgotten - unless the slot still holds an avatar or an alias, in which case only
 *        the tags are cleared. does nothing when identities are disabled.
 *
 * @param client_t* client -> the client whose tags just changed
 *
 * @note the caller must hold the clients lock (client->tag_ids is read). the store itself is guarded
 *       by g_client_stored_data_mutex, taken here.
 *
 * @return void
 */
void base__sync_client_identity_in_store(client_t* client)
{
    char identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
    uint64 tag_count = 0;
    uint64 t = 0;
    uint64 j = 0;
    int64 slot = 0;

    if (g_server_settings.are_identities_enabled == FALSE)
    {
        return;
    }

    if (client == NULL_POINTER || client->is_existing == FALSE || client->is_authenticated == FALSE || client->is_music_bot == TRUE)
    {
        return;
    }

    if (client->public_key[0] == 0)
    {
        return; // no key yet -> nothing to key the identity on
    }

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    tag_count = cvector_size(client->tag_ids);

    pthread_mutex_lock(&g_client_stored_data_mutex);

    // find this identity's existing slot (if any)
    slot = -1;
    for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
    {
        if (clib__is_string_equal(g_client_stored_data[j].public_key, identity_hash) == TRUE)
        {
            slot = (int64)j;
            break;
        }
    }

    if (tag_count == 0)
    {
        // the identity no longer wears any tag: drop its entry so it is not restored later - UNLESS it
        // still holds an avatar or an alias, in which case keep the slot and just clear the tags
        if (slot != -1)
        {
            if (g_client_stored_data[slot].base64_avatar[0] != 0 || g_client_stored_data[slot].alias[0] != 0)
            {
                g_client_stored_data[slot].tag_id_count = 0;
                clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
                DBG_IDENTITIES log_info("%s %llu %s %lld %s", "sync_identity: client_id", client->client_id, "has no tags left but keeps an avatar -> cleared tags on slot", slot, "\n");
            }
            else
            {
                clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
                DBG_IDENTITIES log_info("%s %llu %s %lld %s", "sync_identity: client_id", client->client_id, "has no tags left -> cleared store slot", slot, "\n");
            }
        }
        pthread_mutex_unlock(&g_client_stored_data_mutex);
        return;
    }

    // has tags but no entry yet: take the first free slot
    if (slot == -1)
    {
        for (j = 0; j < MAX_CLIENT_STORED_DATA; j++)
        {
            if (g_client_stored_data[j].public_key[0] == 0)
            {
                slot = (int64)j;
                break;
            }
        }
    }

    if (slot == -1)
    {
        DBG_IDENTITIES log_info("%s %llu %s", "sync_identity: identity store FULL, cannot store client_id", client->client_id, "\n");
        pthread_mutex_unlock(&g_client_stored_data_mutex);
        return;
    }

    // rewrite the identity fields but PRESERVE base64_avatar (nulling the whole slot would wipe a
    // stored avatar). a first-free slot is already fully zeroed, so this is correct for new entries too
    clib__null_memory(&g_client_stored_data[slot].public_key[0], MAX_PUBLIC_KEY_LENGTH);
    clib__null_memory(&g_client_stored_data[slot].username[0], USERNAME_MAX_LENGTH);
    clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
    clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); // remember the last-seen username for the admin ui

    g_client_stored_data[slot].tag_id_count = 0;
    for (t = 0; t < tag_count && t < MAX_TAGS_FOR_SINGLE_CLIENT; t++)
    {
        g_client_stored_data[slot].tag_ids[g_client_stored_data[slot].tag_id_count] = (uint64)client->tag_ids[t];
        g_client_stored_data[slot].tag_id_count++;
    }

    DBG_IDENTITIES log_info("%s %llu %s %lld %s %llu %s %s %s", "sync_identity: client_id", client->client_id, "mirrored into slot", slot, "with", (uint64)g_client_stored_data[slot].tag_id_count, "tag(s), hash [", identity_hash, "] \n");

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief removes one stored identity (by its base64(SHA256(public_key)) hash) from the in-memory
 *        store. the on-disk copy is untouched until the next "save server settings" re-serializes the
 *        store without it. stripping the tags off a currently-connected holder is the caller's job.
 *
 * @param char* identity_hash -> the base64 hash identifying the identity to drop
 *
 * @return boole TRUE if a matching entry was found and cleared
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole base__delete_identity_from_store_by_hash(char* identity_hash)
{
    uint64 i = 0;
    boole found = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            clib__null_memory(&g_client_stored_data[i], sizeof(client_stored_data_t));
            found = TRUE;
            DBG_IDENTITIES log_info("%s %llu %s %s %s", "delete_identity: cleared store slot", i, "for hash [", identity_hash, "] \n");
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found;
}

/**
 * @brief stores (or replaces) an identity's avatar in the in-memory store, keyed by its
 *        base64(SHA256(public_key)) hash. creates an avatar-only slot if the identity has no entry
 *        yet. pass an empty/NULL base64_avatar to clear it (and drop the slot if it then holds
 *        neither tags nor an alias). the on-disk copy follows on the next settings save.
 *
 * @param char* identity_hash -> base64(SHA256(public_key)) of the identity, the store's key
 * @param char* base64_avatar -> the base64 avatar to store, or NULL/empty to clear it
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return void
 */
void base__set_identity_avatar_by_hash(char* identity_hash, char* base64_avatar)
{
    uint64 i = 0;
    int64 slot = -1;
    boole clearing = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    clearing = (boole)(base64_avatar == NULL_POINTER || base64_avatar[0] == 0);

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            slot = (int64)i;
            break;
        }
    }

    if (slot == -1)
    {
        if (clearing == TRUE)
        {
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return; // nothing stored to clear
        }

        for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
        {
            if (g_client_stored_data[i].public_key[0] == 0)
            {
                slot = (int64)i;
                break;
            }
        }

        if (slot == -1)
        {
            DBG_IDENTITIES log_info("%s", "set_identity_avatar: identity store FULL, cannot store avatar \n");
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return;
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    }

    clib__null_memory(&g_client_stored_data[slot].base64_avatar[0], MAX_CLIENT_AVATAR_LENGTH);

    if (clearing == FALSE)
    {
        clib__copy_memory(base64_avatar, &g_client_stored_data[slot].base64_avatar[0], clib__utf8_string_length(base64_avatar), MAX_CLIENT_AVATAR_LENGTH - 1);
    }
    else if (g_client_stored_data[slot].tag_id_count == 0 && g_client_stored_data[slot].alias[0] == 0)
    {
        // cleared the avatar and the slot holds neither tags nor an alias -> drop it entirely
        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief compares two strings ignoring ascii case. used for alias clash detection so "Ada" cannot be
 *        registered next to "ada" and impersonate it in a contact list. non-ascii bytes compare as-is.
 *
 * @param char* string1 -> the first null-terminated string
 * @param char* string2 -> the second null-terminated string
 *
 * @return boole -> TRUE when the strings match case-insensitively, FALSE otherwise or when either is NULL
 */
static boole _base_internal__are_strings_equal_ignoring_ascii_case(char* string1, char* string2)
{
    uint64 i = 0;
    char character1 = 0;
    char character2 = 0;

    if (string1 == NULL_POINTER || string2 == NULL_POINTER)
    {
        return FALSE;
    }

    while (string1[i] != 0 && string2[i] != 0)
    {
        character1 = string1[i];
        character2 = string2[i];

        if (character1 >= 'A' && character1 <= 'Z') { character1 = (char)(character1 + 32); }
        if (character2 >= 'A' && character2 <= 'Z') { character2 = (char)(character2 + 32); }

        if (character1 != character2)
        {
            return FALSE;
        }

        i++;
    }

    return (boole)(string1[i] == 0 && string2[i] == 0);
}

/**
 * @brief tells whether an alias is already registered on a DIFFERENT identity. the alias is the only
 *        handle the stored-clients list exposes and the key clients pair offline entries by, so two
 *        identities must never share one. an identity re-registering its own alias is not a clash, and
 *        an empty alias (clearing) never clashes.
 *
 * @param char* alias -> the alias about to be registered
 * @param char* identity_hash -> the identity it would be registered on
 *
 * @return boole TRUE when another identity already holds this alias
 *
 * @note guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole base__is_alias_taken_by_another_identity(char* alias, char* identity_hash)
{
    uint64 i = 0;
    boole taken = FALSE;

    if (alias == NULL_POINTER || alias[0] == 0 || identity_hash == NULL_POINTER)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue;
        }

        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            continue; // the identity's own entry - renaming itself is allowed
        }

        if (_base_internal__are_strings_equal_ignoring_ascii_case(&g_client_stored_data[i].alias[0], alias) == TRUE)
        {
            taken = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return taken;
}

/**
 * @brief stamps "this identity was connected just now" onto its stored entry, as a unix-seconds
 *        timestamp.
 *
 *        only REGISTERED identities are tracked - an entry exists and carries an alias - so a random
 *        guest passing through is never timestamped. also requires identities to be enabled and the
 *        admin to have switched allow_last_seen on; with either off no timestamp is ever recorded or
 *        served. like every stored-data write, the value reaches disk on the next settings save.
 *
 * @param char* identity_hash -> base64 hash of the client's public key
 *
 * @return void
 */
void base__touch_identity_last_seen_by_hash(char* identity_hash)
{
    uint64 i = 0;

    if (g_server_settings.are_identities_enabled == FALSE || g_server_settings.allow_last_seen == FALSE)
    {
        return;
    }

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            if (g_client_stored_data[i].alias[0] != 0) // registered = carries an admin-granted alias
            {
                g_client_stored_data[i].last_seen_unix_seconds = (int64)time(NULL_POINTER);
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief remembers an identity's RAW rsa public key, so peers can encrypt to it while its owner is
 *        offline. only does anything while allow_offline_messages is on - with the feature off the
 *        server keeps nothing but the hash, exactly as before. creates the slot if the identity has
 *        none yet (a first-time visitor with no tags/avatar/alias).
 *
 * @param char* identity_hash -> base64 hash of the public key (the store's key)
 * @param char* raw_public_key -> the public key itself
 *
 * @return void
 */
void base__store_identity_raw_public_key(char* identity_hash, char* raw_public_key)
{
    uint64 i = 0;
    int64 slot = -1;

    if (g_server_settings.are_identities_enabled == FALSE || g_server_settings.allow_offline_messages == FALSE)
    {
        return;
    }

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0 || raw_public_key == NULL_POINTER || raw_public_key[0] == 0)
    {
        return;
    }

    if (clib__utf8_string_length(raw_public_key) >= MAX_PUBLIC_KEY_LENGTH)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            slot = (int64)i;
            break;
        }
    }

    if (slot == -1)
    {
        for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
        {
            if (g_client_stored_data[i].public_key[0] == 0)
            {
                slot = (int64)i;
                break;
            }
        }

        if (slot == -1)
        {
            DBG_IDENTITIES log_info("%s", "store_identity_raw_public_key: identity store FULL \n");
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return;
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    }

    clib__null_memory(&g_client_stored_data[slot].raw_public_key[0], MAX_PUBLIC_KEY_LENGTH);
    clib__copy_memory(raw_public_key, &g_client_stored_data[slot].raw_public_key[0], clib__utf8_string_length(raw_public_key), MAX_PUBLIC_KEY_LENGTH - 1);

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief forgets the stored raw public key of one identity. called when an admin takes an alias away:
 *        the identity stops being registered, so it can no longer be written to while offline and
 *        there is no reason to keep the key that would allow it.
 *
 * @param char* identity_hash -> hash identifying the stored identity
 *
 * @return void
 */
void base__clear_identity_raw_public_key(char* identity_hash)
{
    uint64 i = 0;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            clib__null_memory(&g_client_stored_data[i].raw_public_key[0], MAX_PUBLIC_KEY_LENGTH);
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief resolves a REGISTERED alias to the identity hash that currently owns it. aliases are unique
 *        across identities (the set-alias handler enforces that), so this is a 1:1 lookup.
 *
 * @param char* alias -> the alias to look up (compared ignoring ascii case, like the uniqueness check)
 * @param char* out_identity_hash -> receives the hash
 * @param uint64 out_buffer_size -> size of out_identity_hash
 *
 * @return boole TRUE when an identity carries this alias
 */
boole base__get_identity_hash_by_alias(char* alias, char* out_identity_hash, uint64 out_buffer_size)
{
    uint64 i = 0;
    boole found = FALSE;

    if (alias == NULL_POINTER || alias[0] == 0 || out_identity_hash == NULL_POINTER || out_buffer_size == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (g_client_stored_data[i].public_key[0] == 0 || g_client_stored_data[i].alias[0] == 0)
        {
            continue;
        }

        if (_base_internal__are_strings_equal_ignoring_ascii_case(&g_client_stored_data[i].alias[0], alias) == TRUE)
        {
            clib__null_memory(out_identity_hash, out_buffer_size);
            clib__copy_memory(&g_client_stored_data[i].public_key[0], out_identity_hash, clib__utf8_string_length(&g_client_stored_data[i].public_key[0]), out_buffer_size - 1);
            found = TRUE;
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found;
}

/**
 * @brief TRUE when this identity is REGISTERED on the server, meaning an admin gave it an alias.
 *
 * @param char* identity_hash -> base64 hash of the public key
 *
 * @return boole
 */
boole base__is_identity_registered_by_hash(char* identity_hash)
{
    uint64 i = 0;
    boole registered = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            registered = (boole)(g_client_stored_data[i].alias[0] != 0);
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return registered;
}

/**
 * @brief parks one text message for an identity that is not connected right now. the payload is
 *        copied onto the heap and is never inspected: it arrived encrypted with the RECIPIENT's
 *        public key, so the server cannot read it and neither can anybody else.
 *
 * @param char* recipient_identity_hash -> who it is for (the identity, not the alias)
 * @param char* sender_identity_hash -> who sent it
 * @param char* sender_alias -> the sender's registered name, for display on delivery
 * @param char* base64_encrypted_message -> opaque ciphertext
 *
 * @return boole TRUE when the message was queued
 *
 * @note takes g_offline_messages_mutex (leaf lock)
 */
boole base__queue_offline_message(char* recipient_identity_hash, char* sender_identity_hash, char* sender_alias, char* base64_encrypted_message)
{
    uint64 i = 0;
    uint64 message_length = 0;
    uint64 messages_for_recipient = 0;
    int64 free_slot = -1;
    int64 oldest_slot = -1;
    uint64 oldest_sequence_number = 0;
    char* message_copy = NULL_POINTER;

    if (g_offline_messages == NULL_POINTER || recipient_identity_hash == NULL_POINTER || base64_encrypted_message == NULL_POINTER)
    {
        return FALSE;
    }

    message_length = clib__utf8_string_length(base64_encrypted_message);
    if (message_length == 0 || message_length >= MAX_OFFLINE_MESSAGE_LENGTH)
    {
        DBG_IDENTITIES log_info("%s", "queue_offline_message: empty or oversize message dropped \n");
        return FALSE;
    }

    pthread_mutex_lock(&g_offline_messages_mutex);

    // one pass: count what this recipient already has waiting, find a free slot, and remember the
    // oldest entry of this recipient in case the per-identity limit is reached
    for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
    {
        if (g_offline_messages[i].is_used == FALSE)
        {
            if (free_slot == -1) { free_slot = (int64)i; }
            continue;
        }

        if (clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], recipient_identity_hash) == TRUE)
        {
            messages_for_recipient++;

            if (oldest_slot == -1 || g_offline_messages[i].sequence_number < oldest_sequence_number)
            {
                oldest_slot = (int64)i;
                oldest_sequence_number = g_offline_messages[i].sequence_number;
            }
        }
    }

    // recipient is at their personal limit: drop their oldest to make room, so a busy conversation
    // keeps the most recent messages instead of refusing new ones forever
    if (messages_for_recipient >= MAX_OFFLINE_MESSAGES_PER_IDENTITY && oldest_slot != -1)
    {
        if (g_offline_messages[oldest_slot].base64_encrypted_message != NULL_POINTER)
        {
            memorymanager__free((nuint)g_offline_messages[oldest_slot].base64_encrypted_message);
        }
        clib__null_memory(&g_offline_messages[oldest_slot], sizeof(offline_chat_message_t));
        free_slot = oldest_slot;
    }

    if (free_slot == -1)
    {
        DBG_IDENTITIES log_info("%s", "queue_offline_message: queue FULL, message refused \n");
        pthread_mutex_unlock(&g_offline_messages_mutex);
        return FALSE;
    }

    message_copy = (char*)memorymanager__allocate(message_length + 1, MEMALLOC_OFFLINE_MESSAGE);
    if (message_copy == NULL_POINTER)
    {
        pthread_mutex_unlock(&g_offline_messages_mutex);
        return FALSE;
    }

    clib__null_memory(&g_offline_messages[free_slot], sizeof(offline_chat_message_t));
    clib__copy_memory(base64_encrypted_message, message_copy, message_length, message_length);

    g_offline_messages[free_slot].is_used = TRUE;
    g_offline_messages[free_slot].base64_encrypted_message = message_copy;
    g_offline_messages[free_slot].message_length = message_length;
    g_offline_messages[free_slot].queued_unix_seconds = (int64)time(NULL_POINTER);
    g_offline_message_sequence_counter++;
    g_offline_messages[free_slot].sequence_number = g_offline_message_sequence_counter;

    clib__copy_memory(recipient_identity_hash, &g_offline_messages[free_slot].recipient_identity_hash[0], clib__utf8_string_length(recipient_identity_hash), IDENTITY_HASH_MAX_LENGTH - 1);

    if (sender_identity_hash != NULL_POINTER)
    {
        clib__copy_memory(sender_identity_hash, &g_offline_messages[free_slot].sender_identity_hash[0], clib__utf8_string_length(sender_identity_hash), IDENTITY_HASH_MAX_LENGTH - 1);
    }

    if (sender_alias != NULL_POINTER)
    {
        clib__copy_memory(sender_alias, &g_offline_messages[free_slot].sender_alias[0], clib__utf8_string_length(sender_alias), USERNAME_MAX_LENGTH - 1);
    }

    DBG_IDENTITIES log_info("%s %llu %s", "queue_offline_message: queued as sequence", g_offline_messages[free_slot].sequence_number, "\n");

    pthread_mutex_unlock(&g_offline_messages_mutex);

    return TRUE;
}

/**
 * @brief drops every queued message for one identity, freeing their payloads. used after delivery
 *        and when an identity is deleted from the store.
 *
 * @param char* identity_hash -> whose messages to drop
 *
 * @return void
 *
 * @note takes g_offline_messages_mutex (leaf lock)
 */
void base__free_offline_messages_for_identity(char* identity_hash)
{
    uint64 i = 0;

    if (g_offline_messages == NULL_POINTER || identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    pthread_mutex_lock(&g_offline_messages_mutex);

    for (i = 0; i < MAX_OFFLINE_MESSAGES; i++)
    {
        if (g_offline_messages[i].is_used == TRUE && clib__is_string_equal(&g_offline_messages[i].recipient_identity_hash[0], identity_hash) == TRUE)
        {
            if (g_offline_messages[i].base64_encrypted_message != NULL_POINTER)
            {
                memorymanager__free((nuint)g_offline_messages[i].base64_encrypted_message);
            }
            clib__null_memory(&g_offline_messages[i], sizeof(offline_chat_message_t));
        }
    }

    pthread_mutex_unlock(&g_offline_messages_mutex);
}

/**
 * @brief stores (or replaces) an identity's admin-registered alias in the in-memory store, keyed by
 *        its base64(SHA256(public_key)) hash. creates an alias-only slot if the identity has no entry
 *        yet. pass an empty/NULL alias to clear it (and drop the slot if it then holds neither tags
 *        nor an avatar). the on-disk copy follows on the next settings save.
 *
 * @param char* identity_hash -> base64(SHA256(public_key)) of the identity, the store's key
 * @param char* alias -> the alias to store, or NULL/empty to clear it
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return void
 */
void base__set_identity_alias_by_hash(char* identity_hash, char* alias)
{
    uint64 i = 0;
    int64 slot = -1;
    boole clearing = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return;
    }

    clearing = (boole)(alias == NULL_POINTER || alias[0] == 0);

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            slot = (int64)i;
            break;
        }
    }

    if (slot == -1)
    {
        if (clearing == TRUE)
        {
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return; // nothing stored to clear
        }

        for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
        {
            if (g_client_stored_data[i].public_key[0] == 0)
            {
                slot = (int64)i;
                break;
            }
        }

        if (slot == -1)
        {
            DBG_IDENTITIES log_info("%s", "set_identity_alias: identity store FULL, cannot store alias \n");
            pthread_mutex_unlock(&g_client_stored_data_mutex);
            return;
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    }

    clib__null_memory(&g_client_stored_data[slot].alias[0], USERNAME_MAX_LENGTH);

    if (clearing == FALSE)
    {
        clib__copy_memory(alias, &g_client_stored_data[slot].alias[0], clib__utf8_string_length(alias), USERNAME_MAX_LENGTH - 1);
    }
    else if (g_client_stored_data[slot].tag_id_count == 0 && g_client_stored_data[slot].base64_avatar[0] == 0)
    {
        // cleared the alias and the slot holds neither tags nor an avatar -> drop it entirely
        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief copies an identity's stored avatar (by hash) into out_buffer. out_buffer is always
 *        null-terminated, and the copy is truncated to out_buffer_size - 1 bytes.
 *
 * @param char* identity_hash -> base64(SHA256(public_key)) of the identity to look up
 * @param char* out_buffer -> receives the stored base64 avatar, emptied first
 * @param uint64 out_buffer_size -> size of out_buffer in bytes, including the null terminator
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
 *
 * @return boole -> TRUE only if the identity was found and held a non-empty avatar
 */
boole base__get_identity_avatar_by_hash(char* identity_hash, char* out_buffer, uint64 out_buffer_size)
{
    uint64 i = 0;
    boole found = FALSE;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0 || out_buffer == NULL_POINTER || out_buffer_size == 0)
    {
        return FALSE;
    }

    out_buffer[0] = 0;

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == TRUE)
        {
            if (g_client_stored_data[i].base64_avatar[0] != 0)
            {
                clib__copy_memory(&g_client_stored_data[i].base64_avatar[0], out_buffer, clib__utf8_string_length(&g_client_stored_data[i].base64_avatar[0]), out_buffer_size - 1);
                found = TRUE;
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found;
}

/**
 * @brief adds or removes one tag id on a stored identity (by hash), for admin tag management of
 *        identities that may be offline.
 *
 *        removing an identity's last tag clears the whole entry, matching how the store never holds
 *        tagless identities. disk persistence still waits for the next "save server settings".
 *        stripping/adding the tag on a currently-connected holder is the caller's job.
 *
 * @param char* identity_hash -> base64 hash of the identity to modify
 * @param uint64 tag_id -> the tag id to add or remove
 * @param boole add -> TRUE to add the tag, FALSE to remove it
 *
 * @return boole -> TRUE if the identity existed (whether or not the tag set actually changed)
 *
 * @note the store is guarded by g_client_stored_data_mutex, taken here (a leaf lock).
 */
boole base__modify_identity_tag_in_store(char* identity_hash, uint64 tag_id, boole add)
{
    uint64 i = 0;
    uint64 t = 0;
    boole found_identity = FALSE;
    boole has_tag = FALSE;
    uint64 tag_index = 0;

    if (identity_hash == NULL_POINTER || identity_hash[0] == 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&g_client_stored_data_mutex);

    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        if (clib__is_string_equal(g_client_stored_data[i].public_key, identity_hash) == FALSE)
        {
            continue;
        }

        found_identity = TRUE;

        for (t = 0; t < g_client_stored_data[i].tag_id_count; t++)
        {
            if (g_client_stored_data[i].tag_ids[t] == tag_id)
            {
                has_tag = TRUE;
                tag_index = t;
                break;
            }
        }

        if (add == TRUE)
        {
            if (has_tag == FALSE && g_client_stored_data[i].tag_id_count < MAX_TAGS_FOR_SINGLE_CLIENT)
            {
                g_client_stored_data[i].tag_ids[g_client_stored_data[i].tag_id_count] = tag_id;
                g_client_stored_data[i].tag_id_count++;
            }
        }
        else
        {
            if (has_tag == TRUE)
            {
                for (t = tag_index; (t + 1) < g_client_stored_data[i].tag_id_count; t++)
                {
                    g_client_stored_data[i].tag_ids[t] = g_client_stored_data[i].tag_ids[t + 1];
                }
                g_client_stored_data[i].tag_id_count--;

                // an identity with no tags left is dropped entirely, like everywhere else
                if (g_client_stored_data[i].tag_id_count == 0)
                {
                    clib__null_memory(&g_client_stored_data[i], sizeof(client_stored_data_t));
                }
            }
        }

        DBG_IDENTITIES log_info("%s %s %s %llu %s %s %s", "modify_identity_tag:", (add == TRUE) ? "added" : "removed", "tag", tag_id, (add == TRUE) ? "to" : "from", "identity [", identity_hash);

        break;
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);

    return found_identity;
}

/**
 * @brief encrypts string and converts it to base64 string.
 *
 * @param char* string_to_encrypt -> the null-terminated string to encrypt
 * @param int64* out_allocated_buffer_size -> receives the size of the returned buffer
 * @param char* dh_shared_secret -> optional DH shared secret to mix in, or 0
 *
 * @return char* encrypted string
 *
 * @attention the input string needs to be null-terminated. Its length is determined by position of null-terminator
 */
char* base__encrypt_cstring_and_convert_to_base64(char* string_to_encrypt, int64* out_allocated_buffer_size, char* dh_shared_secret)
{
    int64 encryption_buffer_size = 0;
    int64 base64_out_string_size = 0;
    unsigned char* encryption_buffer = 0;
    char* base64_out_string = 0;
    struct AES_ctx ctx;
    uint64 i = 0;
    unsigned char message_iv[16];
    char iv_base64[28];
    cJSON* envelope_object = 0;
    char* envelope_string = 0;
    char* printed_envelope = 0;
    int64 printed_envelope_length = 0;
    unsigned char enc_key[32];
    unsigned char mac_key[32];
    unsigned char metadata_tag[32];
    char tag_base64[48];
    boole has_shared_secret_layer = FALSE;
    int64 passed_string_length = 0;

    clib__null_memory(iv_base64, sizeof(iv_base64));
    clib__null_memory(enc_key, sizeof(enc_key));
    clib__null_memory(mac_key, sizeof(mac_key));
    clib__null_memory(metadata_tag, sizeof(metadata_tag));
    clib__null_memory(tag_base64, sizeof(tag_base64));

    passed_string_length = (int64)clib__utf8_string_length(string_to_encrypt);

    DBG_ENCRYPTION log_info("%s %lld %s", "base__encrypt_cstring_and_convert_to_base64() string length, ", passed_string_length, "\n");

    // why is this done this way?
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

    // check size
    if (base64_out_string_size > g_server_settings.websocket_message_max_length)
    {
        DBG_ENCRYPTION log_info("%s", "base__encrypt_cstring_and_convert_to_base64()  base64_out_string_size > g_server_settings.websocket_message_max_length) returning null \n");
        return 0;
    }

    // fresh random IV for this message; it seeds every metadata layer's AES-CTR counter and travels on the
    // wire as the plaintext "iv" field of the JSON envelope (an IV is not secret, only unique-per-message)
    if (base__fill_secure_random_bytes(message_iv, sizeof(message_iv)) == FALSE)
    {
        return 0;
    }

    DBG_ENCRYPTION log_info("%s %lld %s", "base__encrypt_cstring_and_convert_to_base64() encryption_buffer_size", encryption_buffer_size, "\n");
    DBG_ENCRYPTION log_info("%s %lld %s", "base__encrypt_cstring_and_convert_to_base64() passed_string_length", passed_string_length, "\n");
    DBG_ENCRYPTION log_info("%s %lld %s", "base64_out_string_size() base64_out_string_size", base64_out_string_size, "\n");

    encryption_buffer = (unsigned char*)memorymanager__allocate(encryption_buffer_size, MEMALLOC_TYPE_ENCRYPT);
    base64_out_string = (char*)memorymanager__allocate(base64_out_string_size, MEMALLOC_TYPE_ENCRYPT);

    clib__copy_memory(string_to_encrypt, encryption_buffer, passed_string_length, encryption_buffer_size);

    for (i = 0; i < g_server_settings.keys_count; i++)
    {
        AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, message_iv);
        AES_CTR_xcrypt_buffer(&ctx, encryption_buffer, encryption_buffer_size);
    }

    // if a shared secret has been established, add a layer encrypted with an HKDF-derived AES key (same
    // per-message IV), and keep the HKDF-derived MAC key for the authentication tag computed after base64 below
    if (dh_shared_secret != NULL_POINTER)
    {
        if (_base_internal__derive_keys_from_shared_secret(dh_shared_secret, enc_key, mac_key) == FALSE)
        {
            memorymanager__free((nuint)encryption_buffer);
            memorymanager__free((nuint)base64_out_string);
            return 0;
        }
        has_shared_secret_layer = TRUE;

        AES_init_ctx_iv(&ctx, enc_key, message_iv);
        AES_CTR_xcrypt_buffer(&ctx, encryption_buffer, encryption_buffer_size);
    }

    zchg_base64_encode(encryption_buffer, encryption_buffer_size, base64_out_string);
    zchg_base64_encode(message_iv, sizeof(message_iv), iv_base64);

    // encrypt-then-MAC: tag = HMAC-SHA256(mac_key, iv_base64 || data_base64). only present once a shared
    // secret has been established; the receiver then requires it, which blocks a tag-stripping downgrade.
    if (has_shared_secret_layer == TRUE)
    {
        if (_base_internal__compute_metadata_tag(mac_key, iv_base64, base64_out_string, metadata_tag) == FALSE)
        {
            memorymanager__free((nuint)encryption_buffer);
            memorymanager__free((nuint)base64_out_string);
            return 0;
        }
        zchg_base64_encode(metadata_tag, sizeof(metadata_tag), tag_base64);
    }

    if (encryption_buffer != NULL_POINTER)
    {
        memorymanager__free((nuint)encryption_buffer);
    }

    // wrap the ciphertext and its (public, per-message) IV in a JSON envelope { "iv":..., "data":... }.
    // cJSON_PrintUnformatted returns a malloc'd string, so copy it into a memorymanager buffer (the caller
    // frees the return with memorymanager__free) and free the cJSON string with free().
    envelope_object = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope_object, "iv", iv_base64);
    cJSON_AddStringToObject(envelope_object, "data", base64_out_string);
    if (has_shared_secret_layer == TRUE)
    {
        cJSON_AddStringToObject(envelope_object, "tag", tag_base64);
    }
    printed_envelope = cJSON_PrintUnformatted(envelope_object);
    cJSON_Delete(envelope_object);

    if (base64_out_string != NULL_POINTER)
    {
        memorymanager__free((nuint)base64_out_string);
    }

    if (printed_envelope == NULL_POINTER)
    {
        return 0;
    }

    printed_envelope_length = (int64)clib__utf8_string_length(printed_envelope);

    // the envelope (not the bare ciphertext) is what goes on the wire, so enforce the same size cap the
    // receiver applies in onmessage against the FULL envelope length, including the iv field + scaffolding
    if (printed_envelope_length > g_server_settings.websocket_message_max_length)
    {
        free(printed_envelope);
        return 0;
    }

    envelope_string = (char*)memorymanager__allocate(printed_envelope_length + 1, MEMALLOC_TYPE_ENCRYPT);
    clib__copy_memory(printed_envelope, envelope_string, printed_envelope_length, printed_envelope_length + 1);
    envelope_string[printed_envelope_length] = '\0';

    free(printed_envelope);

    *out_allocated_buffer_size = printed_envelope_length + 1;

    return envelope_string;
}

/**
 * @brief decrypts base64 string (used for decrypting metadata of message, not contents, contents are decrypted on clients end)
 *
 * @param uint64 client_id -> id of the client the data is from
 * @param char* base64_string -> the base64 input to decode and decrypt
 * @param unsigned char* out_buffer -> receives the decrypted bytes
 * @param int64 out_buffer_length -> size of out_buffer
 *
 * @attention this function is used within read lock on clients_array, result from this function must be manually freed
 *
 * @return void
 */
void base__get_data_from_base64_and_decrypt_it(uint64 client_id, char* base64_string, unsigned char* out_buffer, int64 out_buffer_length)
{
    int64 base64_decoded_size = 0; // 25 percent smaller
    int64 iv_decoded_size = 0;
    client_t* client = &g_clients_array[client_id];
    struct AES_ctx ctx;
    int64 i = 0;
    unsigned char message_iv[32];
    cJSON* envelope = 0;
    cJSON* iv_item = 0;
    cJSON* data_item = 0;
    cJSON* tag_item = 0;
    unsigned char enc_key[32];
    unsigned char mac_key[32];
    unsigned char expected_tag[32];
    char expected_tag_base64[48];

    clib__null_memory(message_iv, sizeof(message_iv));
    clib__null_memory(enc_key, sizeof(enc_key));
    clib__null_memory(mac_key, sizeof(mac_key));
    clib__null_memory(expected_tag, sizeof(expected_tag));
    clib__null_memory(expected_tag_base64, sizeof(expected_tag_base64));

    // the wire payload is a JSON envelope { "iv": <base64>, "data": <base64 ciphertext> }. the per-message
    // IV is public and seeds every metadata layer's AES-CTR counter, mirroring the encrypt side.
    envelope = cJSON_Parse(base64_string);
    if (envelope == NULL_POINTER)
    {
        DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: malformed envelope\n");
        return;
    }

    iv_item = cJSON_GetObjectItem(envelope, "iv");
    data_item = cJSON_GetObjectItem(envelope, "data");

    if (iv_item == NULL_POINTER || data_item == NULL_POINTER || iv_item->valuestring == NULL_POINTER || data_item->valuestring == NULL_POINTER)
    {
        DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: envelope missing iv/data\n");
        cJSON_Delete(envelope);
        return;
    }

    // the iv field is attacker-controlled: bound its length, then require it to decode to exactly 16 bytes,
    // so the fixed message_iv buffer can never overflow (16 bytes -> 24 base64 chars).
    if (strlen(iv_item->valuestring) > 24)
    {
        DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: iv field too long\n");
        cJSON_Delete(envelope);
        return;
    }

    iv_decoded_size = zchg_base64_decode(iv_item->valuestring, strlen(iv_item->valuestring), message_iv);
    if (iv_decoded_size != 16)
    {
        DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: iv is not 16 bytes\n");
        cJSON_Delete(envelope);
        return;
    }

    // once a shared secret has been agreed, this message MUST carry a valid HMAC tag (encrypt-then-MAC).
    // derive the enc + mac keys via HKDF, then verify the tag over iv_base64 || data_base64 BEFORE decoding
    // or decrypting. requiring the tag whenever a shared secret exists blocks an attacker stripping it (downgrade).
    if (client != NULL_POINTER && client->is_existing == TRUE && client->is_dh_shared_secret_agreed_upon == TRUE)
    {
        if (_base_internal__derive_keys_from_shared_secret(client->dh_shared_secret, enc_key, mac_key) == FALSE)
        {
            DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: HKDF failed\n");
            cJSON_Delete(envelope);
            return;
        }

        tag_item = cJSON_GetObjectItem(envelope, "tag");
        if (tag_item == NULL_POINTER || tag_item->valuestring == NULL_POINTER)
        {
            DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: missing auth tag while a shared secret is agreed\n");
            cJSON_Delete(envelope);
            return;
        }

        if (_base_internal__compute_metadata_tag(mac_key, iv_item->valuestring, data_item->valuestring, expected_tag) == FALSE)
        {
            cJSON_Delete(envelope);
            return;
        }
        zchg_base64_encode(expected_tag, sizeof(expected_tag), expected_tag_base64);

        if (_base_internal__constant_time_str_equal(expected_tag_base64, tag_item->valuestring) == FALSE)
        {
            DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: auth tag mismatch\n");
            cJSON_Delete(envelope);
            return;
        }
    }

    // the data field is attacker-controlled; guard its decode against out_buffer_length (the caller's buffer
    // size) so this function is self-defending regardless of caller, mirroring the iv guard above
    if ((int64)(strlen(data_item->valuestring) / 4 * 3) > out_buffer_length)
    {
        DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: data field too large for buffer\n");
        cJSON_Delete(envelope);
        return;
    }

    base64_decoded_size = zchg_base64_decode(data_item->valuestring, strlen(data_item->valuestring), out_buffer);

    // one per-message IV drives every layer; key order does not matter because layered CTR keystreams XOR
    if (client != NULL_POINTER && client->is_existing == TRUE && client->is_dh_shared_secret_agreed_upon == TRUE)
    {
        for (i = (g_server_settings.keys_count - 1); i >= 0; i--)
        {
            AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, message_iv);
            AES_CTR_xcrypt_buffer(&ctx, out_buffer, base64_decoded_size);
        }

        // enc_key was derived (and the tag already verified) in the validation block above
        AES_init_ctx_iv(&ctx, enc_key, message_iv);
        AES_CTR_xcrypt_buffer(&ctx, out_buffer, base64_decoded_size);
    }
    else
    {
        for (i = (g_server_settings.keys_count - 1); i >= 0; i--)
        {
            AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, message_iv);
            AES_CTR_xcrypt_buffer(&ctx, out_buffer, base64_decoded_size);
        }
    }

    cJSON_Delete(envelope);
}

/**
 * @brief encrypts string with public key
 *
 * @param char* public_key_modulus -> the RSA modulus as a base64 string
 * @param unsigned char* bytes -> the bytes to encrypt
 * @param uint64 buffer_length -> number of bytes to encrypt
 *
 * @return char* encrypted string
 *
 * @attention only modulus is needed, exponent is defined in code, base is defined in code
 * @note this function is used only once, when server sends out public key challenge to client to find out if client is owner of that public key
 */
/**
 * @brief bit length of a base64-encoded rsa public key modulus. leading zero bytes are
 *        skipped so a padded key cannot claim a bigger size than it has
 *
 * @param char* public_key_modulus -> the client's base64 public key string
 *
 * @return uint64 modulus bit length, 0 on any invalid input
 */
uint64 base__get_public_key_bit_length(char* public_key_modulus)
{
    unsigned char* modulus_binary = 0;
    uint64 modulus_length = 0;
    uint64 first_nonzero = 0;
    uint64 key_string_length = 0;
    uint64 bits = 0;
    unsigned char top_byte = 0;

    if (public_key_modulus == NULL_POINTER)
    {
        return 0;
    }

    // an 8192-bit modulus is 1368 base64 chars; anything longer is not a valid key
    key_string_length = (uint64)strlen(public_key_modulus);
    if (key_string_length == 0 || key_string_length > 1372)
    {
        return 0;
    }

    modulus_binary = (unsigned char*)memorymanager__allocate(2048, MEMALLOC_PUBLIC_KEY_ENCRYPT);
    modulus_length = zchg_base64_decode(public_key_modulus, key_string_length, modulus_binary);

    while (first_nonzero < modulus_length && modulus_binary[first_nonzero] == 0)
    {
        first_nonzero++;
    }

    if (first_nonzero < modulus_length)
    {
        top_byte = modulus_binary[first_nonzero];
        bits = (modulus_length - first_nonzero - 1) * 8;
        while (top_byte > 0)
        {
            bits++;
            top_byte >>= 1;
        }
    }

    memorymanager__free((nuint)modulus_binary);
    return bits;
}

char* base__encrypt_string_with_public_key(char* public_key_modulus, unsigned char* bytes, uint64 buffer_length)
{
    int status = 0;
    mbedtls_rsa_context rsa;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_mpi N;
    mbedtls_mpi E;
    const char* pers = "rsa_encrypt";
    char* public_key_modulus_binary = NULL_POINTER;
    uint64 buffer_modulus_bin_outsize = 0;
    int ret = 0;
    unsigned char* inputbuffer = NULL_POINTER;
    unsigned char* outbuffer = NULL_POINTER;
    uint64 base64_out_string_size = 0;
    void* base64_out_buffer = NULL_POINTER;

    DBG_ENCRYPTION log_info("%s %s %s", "public_key_modulus_base64 ", public_key_modulus, "\n");

    // an 8192-bit modulus is 1368 base64 chars / 1024 bytes; longer input is not a valid key
    if (strlen(public_key_modulus) > 1372)
    {
        base64_out_buffer = (void*)memorymanager__allocate(1, MEMALLOC_PUBLIC_KEY_ENCRYPT);
        ((char*)base64_out_buffer)[0] = 0;
        return base64_out_buffer;
    }

    public_key_modulus_binary = (char*)memorymanager__allocate(2048, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    buffer_modulus_bin_outsize = zchg_base64_decode(public_key_modulus, strlen(public_key_modulus), (unsigned char*)public_key_modulus_binary);

    DBG_ENCRYPTION log_info("%s %llu %s", "buffer_modulus_bin_outsize ", buffer_modulus_bin_outsize, "\n");

    mbedtls_rsa_init(&rsa);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers));
    if (ret != 0)
    {
        DBG_ENCRYPTION log_info("%s %d", "failed\n  ! mbedtls_ctr_drbg_seed returned ", ret);
    }

    // modulus and exponent; others are not needed for successful import
    mbedtls_mpi_init(&N);
    mbedtls_mpi_read_binary(&N, public_key_modulus_binary, buffer_modulus_bin_outsize);
    // status = mbedtls_mpi_read_string(&N, 64, public_key_modulus_base64);
    // N -> modulus
    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string N failed ", status, "\n");
    }

    mbedtls_mpi_init(&E);
    // E -> exponent

    // load exponent from string (3)
    status = mbedtls_mpi_read_string(&E, 10, "3");

    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string E failed ", status, "\n");
    }

    status = mbedtls_rsa_import(&rsa, &N, NULL_POINTER, NULL_POINTER, NULL_POINTER, &E);

    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " [FAIL] status ", status, "\n");
    }

    inputbuffer = (unsigned char*)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    clib__copy_memory(bytes, inputbuffer, buffer_length, 256);

    outbuffer = (unsigned char*)memorymanager__allocate(2048, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    status = mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg, buffer_length, inputbuffer, outbuffer);

    // status = mbedtls_rsa_public(&rsa, inputbuffer, outbuffer);
    // mbedtls_rsa_pkcs1_encrypt()
    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %X %s", "[!] base__encrypt_string_with_public_key failed ", status, " \n");
    }
    else
    {
        DBG_ENCRYPTION log_info("%s %d %s", "[!] base__encrypt_string_with_public_key succeeded ", status, " \n");
    }

    // the pkcs1 ciphertext is exactly as long as the modulus, not fixed 256 bytes:
    // the old hardcoded 256 silently truncated the challenge for every key over 2048 bits
    base64_out_string_size = ((4 * buffer_modulus_bin_outsize / 3) + 3) & ~3;

    DBG_ENCRYPTION log_info("%s %llu %s", "base64_out_string_size -> ", base64_out_string_size, "\n");

    base64_out_buffer = (void*)memorymanager__allocate(base64_out_string_size * 2, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    zchg_base64_encode(outbuffer, buffer_modulus_bin_outsize, base64_out_buffer);

    memorymanager__free((nuint)public_key_modulus_binary);
    memorymanager__free((nuint)outbuffer);
    memorymanager__free((nuint)inputbuffer);

    return base64_out_buffer;
}

/**
 * @brief destroys a temp channel: moves any remaining members to the root channel and frees the slot,
 *        then broadcasts the deletion. used when a temp channel's owner leaves it, disconnects, or
 *        deletes it. temp channels never have child channels or music bots, so no cascade is needed.
 *
 * @param uint64 temp_channel_id -> id of the temp channel to destroy
 *
 * @attention caller must hold the clients and channels write locks
 *
 * @return void
 */
void base__destroy_temp_channel(uint64 temp_channel_id)
{
    uint64 i = 0;
    uint64 index_of_new_maintainer = 0;
    boole status = FALSE;
    client_t* client_to_move = NULL_POINTER;

    // move any remaining members of the temp channel to the root channel
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        client_to_move = &g_clients_array[i];

        if (client_to_move->is_existing == FALSE)
        {
            continue;
        }
        if (client_to_move->is_authenticated == FALSE)
        {
            continue;
        }
        if (client_to_move->channel_id != temp_channel_id)
        {
            continue;
        }

        client_to_move->channel_id = ROOT_CHANNEL_ID;
        client_to_move->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel

        // keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this client
        // on the channel-mismatch check after the move to root
        audio_channel__process_client_channel_join(client_to_move);

        server_msg__send_channel_join_message_to_all_clients(client_to_move, &g_channel_array[ROOT_CHANNEL_ID]);

        if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == TRUE)
        {
            server_msg__send_maintainer_id_to_single_client(client_to_move, ROOT_CHANNEL_ID, g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
        }

        server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move->p_ws_connection, client_to_move->dh_shared_secret, ROOT_CHANNEL_ID);
    }

    // free the channel slot and tell everyone the channel is gone
    clib__null_memory(&g_channel_array[temp_channel_id], sizeof(channel_t));
    server_msg__send_channel_delete_message_to_all_clients(temp_channel_id, 0);

    // the root channel may have just gained members and have no maintainer; pick one
    if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
    {
        status = base__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE);
        if (status == TRUE)
        {
            g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present = TRUE;
            g_channel_array[ROOT_CHANNEL_ID].maintainer_id = index_of_new_maintainer;
            g_channel_array[ROOT_CHANNEL_ID].maintainer_generation++;
            server_msg__send_maintainer_id_to_clients_in_same_channel(ROOT_CHANNEL_ID, g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
        }
    }
}

/**
 * @brief moves a client into a channel server-side and tells everyone, the same way the delete path moves
 *        clients to the root channel: hand off the maintainer of the channel being left, set the new
 *        channel id, broadcast the channel join, sync the audio peer, then send the new maintainer id and
 *        the active microphone usage to the moved client
 *
 * @param uint64 client_id -> the client to move
 * @param uint64 destination_channel_id -> the channel to move the client into
 *
 * @attention caller must hold the clients and channels write locks
 *
 * @return void
 */
void base__move_client_into_channel(uint64 client_id, uint64 destination_channel_id)
{
    client_t* client = NULL_POINTER;
    channel_t* old_channel = NULL_POINTER;
    channel_t* new_channel = NULL_POINTER;
    uint64 new_maintainer_index = 0;
    boole status = FALSE;

    client = &g_clients_array[client_id];
    old_channel = &g_channel_array[client->channel_id];
    new_channel = &g_channel_array[destination_channel_id];

    // if the client was the maintainer of the channel they are leaving, hand it off to someone still there
    if (old_channel->is_channel_maintainer_present == TRUE && old_channel->maintainer_id == client->client_id)
    {
        status = base__find_new_maintainer_for_channel(&new_maintainer_index, old_channel->channel_id, client_id, TRUE);
        if (status == TRUE)
        {
            old_channel->is_channel_maintainer_present = TRUE;
            old_channel->maintainer_id = new_maintainer_index;
            old_channel->maintainer_generation++;
            server_msg__send_maintainer_id_to_clients_in_same_channel(old_channel->channel_id, old_channel->maintainer_id);
        }
        else
        {
            old_channel->is_channel_maintainer_present = FALSE;
            old_channel->maintainer_id = 0;
            old_channel->maintainer_generation++;
        }
    }

    // move the client and tell everyone (same message order as the delete -> move-to-root path)
    client->channel_id = destination_channel_id;
    client->has_pending_maintainer_reset_vote = FALSE; // channel changed - a pending reset vote belongs to the old channel
    server_msg__send_channel_join_message_to_all_clients(client, new_channel);
    audio_channel__process_client_channel_join(client);

    // if the client is now the only member of the channel, they become its maintainer
    if (base__get_client_count_for_channel(destination_channel_id) == 1)
    {
        new_channel->maintainer_id = client->client_id;
        new_channel->is_channel_maintainer_present = TRUE;
        new_channel->maintainer_generation++;
    }

    server_msg__send_maintainer_id_to_single_client(client, destination_channel_id, new_channel->maintainer_id);
    server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client->p_ws_connection, client->dh_shared_secret, destination_channel_id);
}

/**
 * @brief this function takes care of events that must happen when client disconnects
 *
 * @param uint64 client_index -> id of the disconnecting client
 *
 * @attention caller of this function must place this function in write lock
 *
 * @return void
 */
void base__process_client_disconnect(uint64 client_index)
{
    uint64 channel_id = 0;
    uint64 new_maintainer_index = 0;
    boole status = FALSE;
    boole is_client_also_channel_maintainer = FALSE;
    boole owns_temp_channel = FALSE;
    uint64 owned_temp_channel_id = 0;
    client_t* client = NULL_POINTER;

    DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__process_client_disconnect ", client_index, "\n");

    client = &g_clients_array[client_index];

    if (client->is_existing == TRUE)
    {
        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__process_client_disconnect is_existing TRUE ", client_index, "\n");

        // only ever-authenticated clients are recorded; the wrapper checks that itself
        server_logs__client_disconnected(client);

        // remember when this identity was last here, while the client struct still holds its key
        if (g_server_settings.allow_last_seen == TRUE && client->public_key[0] != 0 && client->is_music_bot == FALSE)
        {
            char last_seen_identity_hash[BASE64_ENCODE_OUT_SIZE(32)];
            base__hash_password_to_base64(client->public_key, last_seen_identity_hash, sizeof(last_seen_identity_hash));
            base__touch_identity_last_seen_by_hash(last_seen_identity_hash);
        }

        audio_channel__process_client_disconnect(client);

        channel_id = g_clients_array[client_index].channel_id;

        is_client_also_channel_maintainer = (boole)(g_channel_array[channel_id].maintainer_id == client_index);

        owns_temp_channel = client->is_temp_admin_channel;
        owned_temp_channel_id = client->temp_channel_id;

        DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__process_client_disconnect setting client struct to null ", client_index, "\n");

        if (client->tag_ids != NULL_POINTER)
        {
            cvector_free(client->tag_ids);
        }

        // tag_ids are the only thing stored in vector
        // if vector is NULL, that's okay

        // clear out file upload buffer
        if (client->file_upload_extension.file_upload_buffer != NULL_POINTER)
        {
            memorymanager__free((nuint)client->file_upload_extension.file_upload_buffer);
        }

        // free the heap-allocated live avatar before the struct is zeroed, or the pointer would leak
        if (client->base64_avatar != NULL_POINTER)
        {
            memorymanager__free((nuint)client->base64_avatar);
            client->base64_avatar = NULL_POINTER;
        }

        clib__null_memory(client, sizeof(client_t));

        server_msg__send_client_disconnect_message_to_all_clients(client_index);

        if (owns_temp_channel == TRUE)
        {
            // the client owned a temp channel (their current channel); destroy it instead of handing the
            // maintainer role off to someone else
            base__destroy_temp_channel(owned_temp_channel_id);
        }
        else if (is_client_also_channel_maintainer == TRUE)
        {
            DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__process_client_disconnect client was maintainer of channel ", client_index, "\n");

            status = base__find_new_maintainer_for_channel(&new_maintainer_index, channel_id, client_index, FALSE);

            if (status == TRUE)
            {
                DBG_CLIENT_DISCONNECT log_info("%s %llu %s", "base__process_client_disconnect new maintainer found ", new_maintainer_index, "\n");
                g_channel_array[channel_id].is_channel_maintainer_present = TRUE;
                g_channel_array[channel_id].maintainer_id = new_maintainer_index;
                g_channel_array[channel_id].maintainer_generation++;

                server_msg__send_maintainer_id_to_clients_in_same_channel(channel_id, new_maintainer_index);
            }
            else
            {
                g_channel_array[channel_id].is_channel_maintainer_present = FALSE;
                g_channel_array[channel_id].maintainer_id = 0;
                g_channel_array[channel_id].maintainer_generation++;
                DBG_CLIENT_DISCONNECT log_info("%s", "base__process_client_disconnect failed to find new maintainer \n");
            }
        }
    }
}

/**
 * @brief routes an authenticated client's decrypted message to the matching handler
 *
 * @param ws_cli_conn_t* websocket -> the client's websocket connection
 * @param uint64 client_index -> id of the client
 * @param char* decrypted_metadata_cstring -> the decrypted message json
 *
 * @return void
 */
void base__process_authenticated_client_message(ws_cli_conn_t* websocket, uint64 client_index, char* decrypted_metadata_cstring)
{
    cJSON* json_root = 0;
    boole status = FALSE;
    char* message_type = 0;
    client_t* client = NULL_POINTER;

    boole is_sender_idle = FALSE;

    // DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %s %s", "base__process_authenticated_client_message message : ", decrypted_metadata_cstring, "\n");
    json_root = cJSON_Parse(decrypted_metadata_cstring);

    if (json_root == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %llu %s", "client : ", client_index, " json_root is null \n");
        ws_close_client(websocket);
        // there is no json object to call cJSON_Delete on so just disconnect the client
        return;
    }

    status = client_msg__is_message_correct_at_first_sight_and_get_message_type(json_root, client_index, &message_type);

    if (status == FALSE)
    {
        DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s%llu%s", "client : ", client_index, "client_msg__is_message_correct_at_first_sight_and_get_message_type failed \n");
        ws_close_client(websocket);
        cJSON_Delete(json_root);
        return;
    }

    DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %llu %s %s %s", "client : ", client_index, " detected message type is ", message_type, "\n");

    clib__read_lock(&g_clients_global_rwlock_guard);
    is_sender_idle = g_clients_array[client_index].is_idle;
    clib__unlock(&g_clients_global_rwlock_guard);

    // todo, ignore audio related messages if audio is completely disabled by server

    // idle-exempt messages: the heartbeat plus the whole webrtc handshake - the server offers
    // to idle clients (create is allowed while idle), so their answers and candidates must pass too
    if (clib__is_string_equal(message_type, "client_connection_check"))
    {
        client_msg__process_client_connection_check(json_root, client_index);
    }
    else if (clib__is_string_equal(message_type, "create_new_webrtc_datachannel_connection"))
    {
        client_msg__process_create_new_webrtc_datachannel_connection(json_root, client_index);
    }
    else if (clib__is_string_equal(message_type, "sdp_answer"))
    {
        client_msg__process_sdp_answer(json_root, client_index);
    }
    else if (clib__is_string_equal(message_type, "ice_candidate"))
    {
        client_msg__process_ice_candidate(json_root, client_index);
    }
    else
    {
        // else block checks messages where it matters if client is in idle state or not
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
            else if (clib__is_string_equal(message_type, "offline_chat_message"))
            {
                client_msg__process_offline_chat_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "channel_chat_message"))
            {
                client_msg__process_channel_chat_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_chat_message_request"))
            {
                client_msg__process_delete_chat_message_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "edit_chat_message_request"))
            {
                client_msg__process_edit_chat_message_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "join_channel_request"))
            {
                client_msg__process_join_channel_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "reset_channel_maintainer"))
            {
                client_msg__process_reset_channel_maintainer_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_channel_request"))
            {
                client_msg__process_delete_channel_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "typing_indicator_request"))
            {
                client_msg__process_typing_indicator_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "poke_client"))
            {
                client_msg__process_poke_client_request(json_root, client_index);
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
            else if (clib__is_string_equal(message_type, "change_admin_password"))
            {
                client_msg__process_change_admin_password_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "add_tag_to_client"))
            {
                client_msg__process_add_tag_to_client_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "remove_tag_from_client"))
            {
                client_msg__process_remove_tag_from_client_message(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "set_alias_request"))
            {
                client_msg__process_set_alias_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "set_identity_alias_request"))
            {
                client_msg__process_set_identity_alias_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_stored_clients"))
            {
                client_msg__process_request_stored_clients(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_icon_upload"))
            {
                client_msg__process_set_server_settings_icon_upload(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_add_new_tag"))
            {
                client_msg__process_set_server_settings_add_new_tag(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_delete_tag"))
            {
                client_msg__process_set_server_settings_delete_tag(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_identity_list"))
            {
                client_msg__process_request_identity_list(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_identity"))
            {
                client_msg__process_delete_identity(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "modify_identity_tag"))
            {
                client_msg__process_modify_identity_tag(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_delete_icon"))
            {
                client_msg__process_set_server_settings_delete_icon(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "server_settings_set_tag_icon"))
            {
                client_msg__process_set_server_settings_set_tag_icon(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "set_channel_icon"))
            {
                client_msg__process_set_channel_icon(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "avatar_upload"))
            {
                client_msg__process_avatar_upload(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "delete_avatar"))
            {
                client_msg__process_delete_avatar(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_avatar_for_client"))
            {
                client_msg__process_request_avatar_for_client(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "request_avatars"))
            {
                client_msg__process_request_avatars_batch(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "save_server_settings"))
            {
                client_msg__process_save_server_settings_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "load_server_settings"))
            {
                client_msg__process_load_server_settings_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "admin_log_request"))
            {
                client_msg__process_admin_log_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "admin_log_clear"))
            {
                client_msg__process_admin_log_clear(json_root, client_index);
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
            else if (clib__is_string_equal(message_type, "get_client_info"))
            {
                client_msg__process_get_client_info_request(json_root, client_index);
            }
            else if (clib__is_string_equal(message_type, "remove_ban"))
            {
                client_msg__process_remove_ban_request(json_root, client_index);
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
                // client_msg__process_direct_chat_picture
                // and
                // client_msg__process_channel_chat_picture
                // gets called by client_msg__process_file_send_completed_request if needed

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
 * @brief routes a not-yet-authenticated client's message (key exchange / challenge) to the matching handler
 *
 * @param ws_cli_conn_t* websocket -> the client's websocket connection
 * @param uint64 index -> id of the client
 * @param char* decrypted_metadata_cstring -> the decrypted message json
 *
 * @return void
 */
void base__process_not_authenticated_client_message(ws_cli_conn_t* websocket, uint64 index, char* decrypted_metadata_cstring)
{
    cJSON* json_root = 0;
    boole status = FALSE;
    int64 checked_message_length = 0;
    char* message_type = 0;

    DBG_AUTHENTICATION log_info("%s %p %s", "[i] authenticating client ", websocket, "\n");
    DBG_AUTHENTICATION log_info("%s %s %s", "decrypted client verification message : ", decrypted_metadata_cstring, "\n");

    // drop oversized unauth messages. the cap must hold public_key_info: the 8192-bit DH public mix
    // (< SHARED_SECRET_LENGTH digits) + the RSA public key (< MAX_PUBLIC_KEY_LENGTH) + JSON scaffolding,
    // so it is derived from those constants rather than hardcoded, to survive future modulus/key bumps
    checked_message_length = clib__utf8_string_length_check_max_length(decrypted_metadata_cstring, UNAUTH_HANDSHAKE_MAX_LENGTH);

    if (checked_message_length == -1)
    {
        DBG_AUTHENTICATION log_info("%s %s %s", "base__process_not_authenticated_client_message message has more chars than allowed : ", decrypted_metadata_cstring, "\n");
        server_logs__join_refused("wrong key or malformed handshake", g_clients_array[index].ip_address);
        ws_close_client(websocket);
        return;
    }

    json_root = cJSON_Parse(decrypted_metadata_cstring);

    DBG_AUTHENTICATION log_info("%s", "base__process_not_authenticated_client_message decrypted_metadata_cstring \n");

    if (json_root == 0)
    {
        DBG_AUTHENTICATION log_info("%s %llu %s", "client : ", index, " json_root is null \n");
        // a wrong key decrypts the handshake into garbage, which is exactly what lands here
        server_logs__join_refused("wrong key or malformed handshake", g_clients_array[index].ip_address);
        ws_close_client(websocket);
        // there is no json object to cJSON_Delete, since it's 0
        return;
    }

    status = client_msg__is_message_correct_at_first_sight_and_get_message_type(json_root, index, &message_type);

    if (status == FALSE)
    {
        DBG_AUTHENTICATION log_info("%s%llu%s", "client : ", index, "client_msg__is_message_correct_at_first_sight_and_get_message_type failed \n");
        server_logs__join_refused("wrong key or malformed handshake", g_clients_array[index].ip_address);
        ws_close_client(websocket);
        cJSON_Delete(json_root);
        return;
    }

    DBG_AUTHENTICATION log_info("%s %llu %s %s %s", "client : ", index, " detected message type is ", message_type, "\n");

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

/**
 * @brief collects the ids of every valid client sitting in channel_id, except one, into
 *        out_receiving_client_ids.
 *
 *        used to build the recipient list when something has to be broadcast to a channel without
 *        echoing it back to the client that caused it. validity is util__is_client_valid, so
 *        free/unauthenticated slots are skipped. out_receiving_client_ids must have room for
 *        max_client_count entries - the function does no bounds checking of its own.
 *
 * @param int client_to_ignore -> index of the client to leave out, pass a value no slot can have to include everyone
 * @param uint64 channel_id -> id of the channel to collect clients from
 * @param int64* out_receiving_client_ids -> receives the matching client indexes
 *
 * @note the caller must hold the clients read lock (g_clients_array is read)
 *
 * @return uint64 -> how many client ids were written into out_receiving_client_ids
 */
uint64 base__get_other_clients_in_channel(int client_to_ignore, uint64 channel_id, int64* out_receiving_client_ids)
{
    uint64 count = 0;
    uint64 i = 0;

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        boole status = FALSE;

        status = util__is_client_valid(i);

        if (status == TRUE && i != client_to_ignore && channel_id == g_clients_array[i].channel_id)
        {
            out_receiving_client_ids[count] = i;
            count++;
        }
    }

    return count;
}

/**
 * @brief gives an icon slot its own heap copy of a base64 data url, replacing whatever it held
 *
 * @param icon_t* icon -> the slot
 * @param cstring base64 -> the data url, cut at ICON_MAX_LENGTH - 1 characters
 *
 * @return boole -> FALSE when the allocation failed (the slot is then empty)
 */
boole base__set_icon_base64(icon_t* icon, cstring base64)
{
    uint64 length = 0;

    base__free_icon_base64(icon);

    length = clib__utf8_string_length(base64);
    if (length > ICON_MAX_LENGTH - 1)
    {
        length = ICON_MAX_LENGTH - 1;
    }

    icon->base64 = (char*)memorymanager__allocate(length + 1, MEMALLOC_ICON); // zeroed, so the copy ends in a NUL
    if (icon->base64 == NULL_POINTER)
    {
        return FALSE;
    }

    clib__copy_memory((void*)base64, (void*)icon->base64, length, length + 1);

    return TRUE;
}

/**
 * @brief releases an icon slot's base64 buffer, if it has one
 *
 * @param icon_t* icon -> the slot
 *
 * @return void
 */
void base__free_icon_base64(icon_t* icon)
{
    if (icon->base64 != NULL_POINTER)
    {
        memorymanager__free((nuint)icon->base64);
        icon->base64 = NULL_POINTER;
    }
}