#include "definitions.h"

/* use forward slashes "/" when specifying paths, not backward slashes "\" linux environment has trouble finding files that way
   windows compiler will work with both */

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

/* guards the identity store (g_client_stored_data). taken as the innermost lock so restore-on-auth and the
   snapshot at save time never tear each other's reads/writes; the save handler only holds the tags read lock,
   which does not exclude a concurrent restore reader */
pthread_mutex_t g_client_stored_data_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64 g_chat_message_id;
client_t* g_clients_array;
channel_t* g_channel_array;
client_stored_data_t* g_client_stored_data;
icon_t* g_icons_array;
tag_t* g_tags_array;
ban_entry_t* g_ban_array;
server_settings_t g_server_settings;

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
    /* rename() on windows fails if the destination already exists, so use MoveFileEx with replace */
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
 * @brief adds a ban entry. ignored if the ip is already banned or the ban list is full
 *
 * @param char* ip_address -> the banned ip (matching is by this)
 * @param char* country_iso_code -> the client's country at ban time (may be empty)
 * @param char* identity -> the client's identity / public key at ban time (may be empty)
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
                return FALSE; /* already banned */
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
        /* keep a slot that has EITHER tags or an avatar (avatar-only identities are valid now) */
        if (g_client_stored_data[i].public_key[0] == 0 || (g_client_stored_data[i].tag_id_count == 0 && g_client_stored_data[i].base64_avatar[0] == 0))
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
 * @brief persists the runtime-editable server state into server_settings.json: the general-settings
 *        toggles, the channel layout, the tags/icons and the ban list. it READS the existing file first
 *        and only updates the keys it manages, so ports, operator keys and every other setting stay
 *        untouched. if the file is missing or cannot be parsed it aborts WITHOUT writing, so a transient
 *        error can never destroy a good settings file. the admin icon/tag (id 0) are re-seeded on every
 *        start and are not written. caller holds the channels, icons, tags and bans locks for reading.
 *
 * @return boole TRUE if the file was written
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

    /* read the existing settings file first so keys / ports / other settings stay untouched */
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

    /* update the general-settings toggles (delete-then-add so a value already present is replaced) */
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_display_country_flags_active");
    cJSON_AddItemToObject(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_voice_chat_active");
    cJSON_AddItemToObject(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_music_bot_audio_active");
    cJSON_AddItemToObject(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_hide_clients_in_password_protected_channels_active");
    cJSON_AddItemToObject(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_temp_channel_creation_allowed");
    cJSON_AddItemToObject(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_password");
    cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_password_is_initial");
    cJSON_AddItemToObject(json_root, "admin_password_is_initial", cJSON_CreateBool(g_server_settings.admin_password_is_initial == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "are_identities_enabled");
    cJSON_AddItemToObject(json_root, "are_identities_enabled", cJSON_CreateBool(g_server_settings.are_identities_enabled == TRUE));

    /* rebuild the channel layout (persistent fields only; runtime state like maintainer/occupants is skipped) */
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

        /* temp channels are disposable and never persisted */
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

    /* rebuild icons (skip the admin icon id 0, which is re-seeded on every start) */
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
        cJSON_AddStringToObject(json_icon, "base64", &icon_in_loop->base64[0]);
        cJSON_AddItemToArray(json_icons, json_icon);
    }

    /* rebuild tags (skip the admin tag id 0, re-seeded on every start) */
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

    /* the admin tag (id 0) itself is re-seeded on every start and skipped by the tags loop above, but its
       icon IS runtime-editable, so persist just that link (icon id + has_icon) here and re-apply it after
       the seed on load. without this the admin's chosen icon reverts to the default on every restart */
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_tag_icon_id");
    cJSON_AddNumberToObject(json_root, "admin_tag_icon_id", (double)g_tags_array[ADMIN_TAG_ID].icon_id);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_tag_has_icon");
    cJSON_AddItemToObject(json_root, "admin_tag_has_icon", cJSON_CreateBool(g_tags_array[ADMIN_TAG_ID].has_icon == TRUE));

    /* rebuild the ban list (matching is by ip; country/identity/extra data are recorded for the admin) */
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

    /* rebuild the identity store (public-key hash -> tag ids); left untouched when identities are disabled */
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
 * @return TRUE if maintainer is found, FALSE if not
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
    /* maintainer will be randomly chosen */
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

    /* maintainer will be randomly chosen */
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
        /* two assumptions for safe operation */
        /* default name won't be too long */
        /* max number of clients will fit into 16 digits (easy), edge cases will have 3 digits, extremely edge cases 4 */

        /* try "<default_name><n>" for n = 0, 1, 2, ... until a username no client is using is found */
        for (username_suffix_number = 0; username_suffix_number < g_server_settings.max_client_count; username_suffix_number++)
        {
            uint64 default_name_length = 0;

            clib__null_memory(candidate_username, USERNAME_MAX_LENGTH);
            clib__copy_memory(g_server_settings.default_client_name, candidate_username, clib__utf8_string_length(g_server_settings.default_client_name), USERNAME_MAX_LENGTH);

            /* as long as I know what I'm doing, shouldn't be dangerous */
            clib__null_memory(index_suffix_buffer, 16);

            /* itoa(username_suffix_number, index_suffix_buffer, 10); gcc on linux does not support itoa so I'm using sprintf */
            sprintf(index_suffix_buffer, "%llu", username_suffix_number);

            default_name_length = clib__utf8_string_length(candidate_username);
            clib__copy_memory(&index_suffix_buffer[0], &candidate_username[default_name_length], clib__utf8_string_length(index_suffix_buffer), clib__utf8_string_length(g_server_settings.default_client_name));

            DBG_AUTHENTICATION log_info("%s %s %s", "index_suffix_buffer -> ", index_suffix_buffer, "\n");
            DBG_AUTHENTICATION log_info("%s %s %s", "candidate_username -> ", candidate_username, "\n");

            for (i = 0; i < g_server_settings.max_client_count; i++)
            {
                /* is_existing needs to be guarded with mutex, while opening client, rwlock is not usable */

                /* log_info("%s %s %d %s", "trying username -> ", candidate_username , i, "\n"); */

                if (g_clients_array[i].is_existing == FALSE)
                { /* client not is_existing, skip, this needs global lock */
                    goto final_check;
                }

                if (g_clients_array[i].is_authenticated == FALSE)
                {
                    goto final_check;
                }

                if (i == client_index)
                { /* skip current client */
                    goto final_check;
                }

                status = clib__is_string_equal(g_clients_array[i].username, candidate_username);

                if (status == TRUE)
                { /* username used by some of the clients, start another loop, with incremented numeric part of client's username */
                    DBG_AUTHENTICATION log_info("%s %s %s", "username ", candidate_username, " it is not available \n");
                    break;
                }

final_check:

                if ((i + 1) == g_server_settings.max_client_count)
                { /* if loop reached its end and username currently used in this loop was found to not be used by any of the clients */
                    /* go to end_loop where this newly found username will be assigned to client */
                    DBG_AUTHENTICATION log_info("%s", "(i + 1) == g_server_settings.max_client_count goto end_loop \n");
                    result = TRUE;
                    goto end_loop;
                }

                /* still some clients that need to be checked, not needed , included just for */
            }
        }
    }

    return result; /* either code jumps to end_loop or it returns false here */

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
        /* log_info("base__free_json_message() cJSON_Delete(json_root_object1); \n"); */
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
#else
    /* every non-Windows platform (Linux, macOS, BSD) has POSIX gettimeofday. this MUST have an
       #else, not an #ifdef __linux__: on macOS __linux__ is undefined, so a linux-only branch left
       the function with no return - it fell off the end and returned garbage, which made every
       spam-protected request (channel create/join/delete, chat, etc.) compare against a bogus
       "now" and get silently rejected while auth (no spam check) still worked */
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
#include <time.h> /* for nanosleep */
#else
#include <unistd.h> /* for usleep */
#endif

void base__sleep_for_milliseconds(uint64 milliseconds)
{ /* cross-platform sleep function */
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
 * @brief part of authentication process.
 *
 * @param cstring ip_address -> ip address
 *
 * @return boole
 *
 * @attention output of this function is stored on heap, and must be freed manually
 */
boole base__is_there_a_client_with_same_ip_address(cstring ip_address)
{
    boole result = FALSE;
    uint64 i = 0;
    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].is_existing == FALSE)
        {
            continue;
        }

        /* doesn't have to be authenticated */
        if (clib__is_string_equal(g_clients_array[i].ip_address, ip_address))
        {
            result = TRUE;
            break;
        }
    }
    return result;
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

            /* keep the admin tag (id 0) always; for every other tag, skip it if it no longer exists */
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
 * @brief snapshots every connected, authenticated client that currently owns at least one tag into the
 *        in-memory identity store, keyed by base64(SHA256(public_key)). an existing entry for that hash is
 *        overwritten; otherwise the first free slot is used. called at "save server settings" time so the
 *        store mirrors the tags people currently wear, without persisting on every tag change. does nothing
 *        when identities are disabled.
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

        tag_count = cvector_size(client->tag_ids);
        if (tag_count == 0)
        {
            DBG_IDENTITIES log_info("%s %llu %s", "snapshot_identities: client_id", client->client_id, "has no tags -> not stored \n");
            continue;
        }

        base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

        /* find an existing entry for this identity, else the first free slot */
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
            continue; /* store is full */
        }

        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
        clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
        clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); /* remember the last-seen username for the admin ui */

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
 *        admin "save server settings" (that save only adds disk persistence on top). called right
 *        after any tag change on a client. if the client has tags they overwrite (or create) its
 *        store entry; if it now has zero tags its entry is cleared, so a fully-untagged identity is
 *        forgotten. does nothing when identities are disabled.
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
        return; /* no key yet -> nothing to key the identity on */
    }

    base__hash_password_to_base64(client->public_key, identity_hash, sizeof(identity_hash));

    tag_count = cvector_size(client->tag_ids);

    pthread_mutex_lock(&g_client_stored_data_mutex);

    /* find this identity's existing slot (if any) */
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
        /* the identity no longer wears any tag: drop its entry so it is not restored later - UNLESS it
           still holds an avatar, in which case keep the slot and just clear the tags */
        if (slot != -1)
        {
            if (g_client_stored_data[slot].base64_avatar[0] != 0)
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

    /* has tags but no entry yet: take the first free slot */
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

    /* rewrite the identity fields but PRESERVE base64_avatar (nulling the whole slot would wipe a
       stored avatar). a first-free slot is already fully zeroed, so this is correct for new entries too */
    clib__null_memory(&g_client_stored_data[slot].public_key[0], MAX_PUBLIC_KEY_LENGTH);
    clib__null_memory(&g_client_stored_data[slot].username[0], USERNAME_MAX_LENGTH);
    clib__null_memory(&g_client_stored_data[slot].tag_ids[0], sizeof(g_client_stored_data[slot].tag_ids));
    clib__copy_memory(identity_hash, &g_client_stored_data[slot].public_key[0], clib__utf8_string_length(identity_hash), MAX_PUBLIC_KEY_LENGTH - 1);
    clib__copy_memory(client->username, &g_client_stored_data[slot].username[0], clib__utf8_string_length(client->username), USERNAME_MAX_LENGTH - 1); /* remember the last-seen username for the admin ui */

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
 *        neither tags nor an avatar). the on-disk copy follows on the next settings save.
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
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
            return; /* nothing stored to clear */
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
    else if (g_client_stored_data[slot].tag_id_count == 0)
    {
        /* cleared the avatar and the slot holds no tags either -> drop it entirely */
        clib__null_memory(&g_client_stored_data[slot], sizeof(client_stored_data_t));
    }

    pthread_mutex_unlock(&g_client_stored_data_mutex);
}

/**
 * @brief copies an identity's stored avatar (by hash) into out_buffer. out_buffer is always
 *        null-terminated. returns TRUE only if a non-empty avatar was found.
 *
 * @note guarded by g_client_stored_data_mutex (a leaf lock).
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
 *        identities that may be offline. removing an identity's last tag clears the whole entry,
 *        matching how the store never holds tagless identities. disk persistence still waits for the
 *        next "save server settings". stripping/adding the tag on a currently-connected holder is the
 *        caller's job.
 *
 * @param char* identity_hash -> base64 hash of the identity to modify
 * @param uint64 tag_id -> the tag id to add or remove
 * @param boole add -> TRUE to add the tag, FALSE to remove it
 *
 * @return boole TRUE if the identity existed (whether or not the tag set actually changed)
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

                /* an identity with no tags left is dropped entirely, like everywhere else */
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

    /* why is this done this way? */
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

    /* check size */
    if (base64_out_string_size > g_server_settings.websocket_message_max_length)
    {
        DBG_ENCRYPTION log_info("%s", "base__encrypt_cstring_and_convert_to_base64()  base64_out_string_size > g_server_settings.websocket_message_max_length) returning null \n");
        return 0;
    }

    /* fresh random IV for this message; it seeds every metadata layer's AES-CTR counter and travels on the
       wire as the plaintext "iv" field of the JSON envelope (an IV is not secret, only unique-per-message) */
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

    /* if a shared secret has been established, add a layer encrypted with an HKDF-derived AES key (same
       per-message IV), and keep the HKDF-derived MAC key for the authentication tag computed after base64 below */
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

    /* encrypt-then-MAC: tag = HMAC-SHA256(mac_key, iv_base64 || data_base64). only present once a shared
       secret has been established; the receiver then requires it, which blocks a tag-stripping downgrade. */
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

    /* wrap the ciphertext and its (public, per-message) IV in a JSON envelope { "iv":..., "data":... }.
       cJSON_PrintUnformatted returns a malloc'd string, so copy it into a memorymanager buffer (the caller
       frees the return with memorymanager__free) and free the cJSON string with free(). */
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

    /* the envelope (not the bare ciphertext) is what goes on the wire, so enforce the same size cap the
       receiver applies in onmessage against the FULL envelope length, including the iv field + scaffolding */
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
    int64 base64_decoded_size = 0; /* 25 percent smaller */
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

    /* the wire payload is a JSON envelope { "iv": <base64>, "data": <base64 ciphertext> }. the per-message
       IV is public and seeds every metadata layer's AES-CTR counter, mirroring the encrypt side. */
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

    /* the iv field is attacker-controlled: bound its length, then require it to decode to exactly 16 bytes,
       so the fixed message_iv buffer can never overflow (16 bytes -> 24 base64 chars). */
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

    /* once a shared secret has been agreed, this message MUST carry a valid HMAC tag (encrypt-then-MAC).
       derive the enc + mac keys via HKDF, then verify the tag over iv_base64 || data_base64 BEFORE decoding
       or decrypting. requiring the tag whenever a shared secret exists blocks an attacker stripping it (downgrade). */
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

    /* the data field is attacker-controlled; guard its decode against out_buffer_length (the caller's buffer
       size) so this function is self-defending regardless of caller, mirroring the iv guard above */
    if ((int64)(strlen(data_item->valuestring) / 4 * 3) > out_buffer_length)
    {
        DBG_ENCRYPTION log_info("%s", "base__get_data_from_base64_and_decrypt_it: data field too large for buffer\n");
        cJSON_Delete(envelope);
        return;
    }

    base64_decoded_size = zchg_base64_decode(data_item->valuestring, strlen(data_item->valuestring), out_buffer);

    /* one per-message IV drives every layer; key order does not matter because layered CTR keystreams XOR */
    if (client != NULL_POINTER && client->is_existing == TRUE && client->is_dh_shared_secret_agreed_upon == TRUE)
    {
        for (i = (g_server_settings.keys_count - 1); i >= 0; i--)
        {
            AES_init_ctx_iv(&ctx, g_server_settings.keys[i].key_value, message_iv);
            AES_CTR_xcrypt_buffer(&ctx, out_buffer, base64_decoded_size);
        }

        /* enc_key was derived (and the tag already verified) in the validation block above */
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

    public_key_modulus_binary = (char*)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

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

    /* modulus and exponent; others are not needed for successful import */
    mbedtls_mpi_init(&N);
    mbedtls_mpi_read_binary(&N, public_key_modulus_binary, buffer_modulus_bin_outsize);
    /* status = mbedtls_mpi_read_string(&N, 64, public_key_modulus_base64); */
    /* N -> modulus */
    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string N failed ", status, "\n");
    }

    mbedtls_mpi_init(&E);
    /* E -> exponent */

    /* load exponent from string (3) */
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

    outbuffer = (unsigned char*)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    status = mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg, buffer_length, inputbuffer, outbuffer);

    /* status = mbedtls_rsa_public(&rsa, inputbuffer, outbuffer); */
    /* mbedtls_rsa_pkcs1_encrypt() */
    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %X %s", "[!] base__encrypt_string_with_public_key failed ", status, " \n");
    }
    else
    {
        DBG_ENCRYPTION log_info("%s %d %s", "[!] base__encrypt_string_with_public_key succeeded ", status, " \n");
    }

    base64_out_string_size = ((4 * 256 / 3) + 3) & ~3;

    DBG_ENCRYPTION log_info("%s %llu %s", "base64_out_string_size -> ", base64_out_string_size, "\n");

    base64_out_buffer = (void*)memorymanager__allocate(base64_out_string_size * 2, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    zchg_base64_encode(outbuffer, 256, base64_out_buffer);

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

    /* move any remaining members of the temp channel to the root channel */
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

        /* keep the webrtc peer's channel in sync, otherwise the audio relay keeps skipping this client
           on the channel-mismatch check after the move to root */
        audio_channel__process_client_channel_join(client_to_move);

        server_msg__send_channel_join_message_to_all_clients(client_to_move, &g_channel_array[ROOT_CHANNEL_ID]);

        if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == TRUE)
        {
            server_msg__send_maintainer_id_to_single_client(client_to_move, ROOT_CHANNEL_ID, g_channel_array[ROOT_CHANNEL_ID].maintainer_id);
        }

        server_msg__send_active_microphone_usage_for_current_channel_to_single_client(client_to_move->p_ws_connection, client_to_move->dh_shared_secret, ROOT_CHANNEL_ID);
    }

    /* free the channel slot and tell everyone the channel is gone */
    clib__null_memory(&g_channel_array[temp_channel_id], sizeof(channel_t));
    server_msg__send_channel_delete_message_to_all_clients(temp_channel_id, 0);

    /* the root channel may have just gained members and have no maintainer; pick one */
    if (g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present == FALSE)
    {
        status = base__find_new_maintainer_for_channel(&index_of_new_maintainer, ROOT_CHANNEL_ID, 0, FALSE);
        if (status == TRUE)
        {
            g_channel_array[ROOT_CHANNEL_ID].is_channel_maintainer_present = TRUE;
            g_channel_array[ROOT_CHANNEL_ID].maintainer_id = index_of_new_maintainer;
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

    /* if the client was the maintainer of the channel they are leaving, hand it off to someone still there */
    if (old_channel->is_channel_maintainer_present == TRUE && old_channel->maintainer_id == client->client_id)
    {
        status = base__find_new_maintainer_for_channel(&new_maintainer_index, old_channel->channel_id, client_id, TRUE);
        if (status == TRUE)
        {
            old_channel->is_channel_maintainer_present = TRUE;
            old_channel->maintainer_id = new_maintainer_index;
            server_msg__send_maintainer_id_to_clients_in_same_channel(old_channel->channel_id, old_channel->maintainer_id);
        }
        else
        {
            old_channel->is_channel_maintainer_present = FALSE;
            old_channel->maintainer_id = 0;
        }
    }

    /* move the client and tell everyone (same message order as the delete -> move-to-root path) */
    client->channel_id = destination_channel_id;
    server_msg__send_channel_join_message_to_all_clients(client, new_channel);
    audio_channel__process_client_channel_join(client);

    /* if the client is now the only member of the channel, they become its maintainer */
    if (base__get_client_count_for_channel(destination_channel_id) == 1)
    {
        new_channel->maintainer_id = client->client_id;
        new_channel->is_channel_maintainer_present = TRUE;
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

        /* tag_ids are the only thing stored in vector */
        /* if vector is NULL, that's okay */

        /* clear out file upload buffer */
        if (client->file_upload_extension.file_upload_buffer != NULL_POINTER)
        {
            memorymanager__free((nuint)client->file_upload_extension.file_upload_buffer);
        }

        /* free the heap-allocated live avatar before the struct is zeroed, or the pointer would leak */
        if (client->base64_avatar != NULL_POINTER)
        {
            memorymanager__free((nuint)client->base64_avatar);
            client->base64_avatar = NULL_POINTER;
        }

        clib__null_memory(client, sizeof(client_t));

        server_msg__send_client_disconnect_message_to_all_clients(client_index);

        if (owns_temp_channel == TRUE)
        {
            /* the client owned a temp channel (their current channel); destroy it instead of handing the
               maintainer role off to someone else */
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

                server_msg__send_maintainer_id_to_clients_in_same_channel(channel_id, new_maintainer_index);
            }
            else
            {
                g_channel_array[channel_id].is_channel_maintainer_present = FALSE;
                g_channel_array[channel_id].maintainer_id = 0;
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
 * */
void base__process_authenticated_client_message(ws_cli_conn_t* websocket, uint64 client_index, char* decrypted_metadata_cstring)
{
    cJSON* json_root = 0;
    boole status = FALSE;
    char* message_type = 0;
    client_t* client = NULL_POINTER;

    boole is_sender_idle = FALSE;

    /* DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %s %s", "base__process_authenticated_client_message message : ", decrypted_metadata_cstring, "\n"); */
    json_root = cJSON_Parse(decrypted_metadata_cstring);

    if (json_root == NULL_POINTER)
    {
        DBG_CLIENT_MESSAGE_MAIN_FUNCTION log_info("%s %llu %s", "client : ", client_index, " json_root is null \n");
        ws_close_client(websocket);
        /* there is no json object to call cJSON_Delete on so just disconnect the client */
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

    /* todo, ignore audio related messages if audio is completely disabled by server */

    /* first two messages are the ones where it doesn't matter if client is in idle state or not */
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
        /* else block checks messages where it matters if client is in idle state or not */
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
                /* client_msg__process_direct_chat_picture
                   and
                   client_msg__process_channel_chat_picture
                   gets called by client_msg__process_file_send_completed_request if needed */

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
 * */
void base__process_not_authenticated_client_message(ws_cli_conn_t* websocket, uint64 index, char* decrypted_metadata_cstring)
{
    cJSON* json_root = 0;
    boole status = FALSE;
    int64 checked_message_length = 0;
    char* message_type = 0;

    DBG_AUTHENTICATION log_info("%s %p %s", "[i] authenticating client ", websocket, "\n");
    DBG_AUTHENTICATION log_info("%s %s %s", "decrypted client verification message : ", decrypted_metadata_cstring, "\n");

    /* drop oversized unauth messages. the cap must hold public_key_info: the 8192-bit DH public mix
       (< SHARED_SECRET_LENGTH digits) + the RSA public key (< MAX_PUBLIC_KEY_LENGTH) + JSON scaffolding,
       so it is derived from those constants rather than hardcoded, to survive future modulus/key bumps */
    checked_message_length = clib__utf8_string_length_check_max_length(decrypted_metadata_cstring, UNAUTH_HANDSHAKE_MAX_LENGTH);

    if (checked_message_length == -1)
    {
        DBG_AUTHENTICATION log_info("%s %s %s", "base__process_not_authenticated_client_message message has more chars than allowed : ", decrypted_metadata_cstring, "\n");
        ws_close_client(websocket);
        return;
    }

    json_root = cJSON_Parse(decrypted_metadata_cstring);

    DBG_AUTHENTICATION log_info("%s", "base__process_not_authenticated_client_message decrypted_metadata_cstring \n");

    if (json_root == 0)
    {
        DBG_AUTHENTICATION log_info("%s %llu %s", "client : ", index, " json_root is null \n");
        ws_close_client(websocket);
        /* there is no json object to cJSON_Delete, since it's 0 */
        return;
    }

    status = client_msg__is_message_correct_at_first_sight_and_get_message_type(json_root, index, &message_type);

    if (status == FALSE)
    {
        DBG_AUTHENTICATION log_info("%s%llu%s", "client : ", index, "client_msg__is_message_correct_at_first_sight_and_get_message_type failed \n");
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