#include "identity_store.h"
#include "../crypto/ciphers.h"
#include "administration.h"
#include "emojis.h"
#include "../definitions.h"
#include "../crypto/crypto_metadata.h"

#include "../clib/clib.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../base.h"
#include "../../third-party/rxi-log/log.h"
#include "../httpserver/http_server.h"
#include "../util.h"
#include "../first_time_setup.h"
#include "server_settings.h"
#include "musicbot_store.h"
#include "permissions.h"
#include "channel.h"
#include "channel_groups.h"
#include "../memory_manager.h"
#include "../audio_websocket.h"
#include <string.h>
#ifdef WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// declarations
static boole _settings_internal__validate_channel_graph(cJSON *channels);
static void _settings_internal__build_and_push_client_config(int64 websocket_port, char plaintext_keys[][256], uint64 keys_count);
static boole _settings_internal__serialize_identities(cJSON *json_root);

/**
 * @brief Reads which attachment types clients may choose to block.
 * @param cJSON* message -> saved settings or an admin request, with optional boolean fields
 * @return void
 * @attention Runtime callers hold the clients write lock.
 */
void settings__read_attachment_receive_policy(cJSON *message)
{
    cJSON *field = NULL_POINTER;

    // read direct picture blocking
    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_direct_chat_pictures_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_direct_chat_pictures_active = cJSON_IsTrue(field);
    }

    // read direct file blocking
    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_direct_chat_files_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_direct_chat_files_active = cJSON_IsTrue(field);
    }

    // read channel picture blocking
    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_channel_chat_pictures_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_channel_chat_pictures_active = cJSON_IsTrue(field);
    }

    // read channel file blocking
    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_channel_chat_files_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_channel_chat_files_active = cJSON_IsTrue(field);
    }
}

/**
 * @brief seeds channel slot 0 as the root channel every client starts in: named "root", audio on,
 *        no parent and no maintainer yet.
 *
 * @return void
 */
void settings__init_channel_list(void)
{
    char channel_name[] = "root";
    char description[] = "this is default entry channel";
    channel_t *root_channel = &g_channel_array[0];

    // set the channel fields
    root_channel->channel_id = 0;
    root_channel->parent_channel_id = -1;
    root_channel->is_root_channel = TRUE;
    root_channel->is_existing = TRUE;
    root_channel->is_audio_enabled = TRUE;
    root_channel->is_video_stream_enabled = TRUE;

    // copy name and description
    clib__string_copy(root_channel->name, sizeof(root_channel->name), channel_name);
    clib__string_copy(root_channel->description, sizeof(root_channel->description), description);

    // set type and maintainer
    root_channel->type = 1;
    root_channel->maintainer_id = -1;
    root_channel->maintainer_generation = 0;
}

/**
 * @brief Restores saved channels, icons, groups, bans and identities into allocated arrays.
 *
 * @details Channels, icons and groups retain their saved array indices; runtime fields are reset. The root channel and seeded admin group and icon remain in place. The admin icon link is restored after loading icons. Bans and identities use sequential slots; identities without groups, an avatar or an offline key are skipped.
 *
 * @note must run after the arrays are allocated and before any client can connect.
 *
 * @return void
 */
void settings__load_persisted_state(void)
{
    FILE *settings_file = NULL_POINTER;
    int64 file_length = 0;
    char *file_buffer = NULL_POINTER;
    uint64 bytes_read = 0;
    cJSON *json_root = NULL_POINTER;
    cJSON *json_channels = NULL_POINTER;
    cJSON *json_channel = NULL_POINTER;
    cJSON *json_icons = NULL_POINTER;
    cJSON *json_icon = NULL_POINTER;
    cJSON *json_groups = NULL_POINTER;
    cJSON *json_group = NULL_POINTER;
    cJSON *json_bans = NULL_POINTER;
    cJSON *json_ban = NULL_POINTER;
    cJSON *json_identities = NULL_POINTER;
    cJSON *json_identity = NULL_POINTER;
    cJSON *json_identity_group_ids = NULL_POINTER;
    cJSON *json_identity_group_id = NULL_POINTER;
    cJSON *json_field = NULL_POINTER;
    channel_t *channel_in_loop = NULL_POINTER;
    icon_t *icon_in_loop = NULL_POINTER;
    group_t *group_in_loop = NULL_POINTER;
    ban_entry_t *ban_in_loop = NULL_POINTER;
    client_stored_data_t *identity_in_loop = NULL_POINTER;
    int64 channel_id = 0;
    int64 icon_id = 0;
    int64 group_id = 0;
    uint64 loaded_channels = 0;
    uint64 loaded_icons = 0;
    uint64 loaded_groups = 0;
    uint64 default_channel_power = 25;
    uint64 loaded_bans = 0;
    uint64 loaded_identities = 0;
    uint64 ban_slot = 0;
    uint64 identity_slot = 0;
    uint64 membership_index = 0;
    boole duplicate_membership = FALSE;

    // add the default emojis
    emojis__load(NULL_POINTER);

    // create default channel group
    channel_groups__init();

    // open the settings file
    settings_file = fopen("server_settings.json", "rb");
    if (settings_file == NULL_POINTER)
    {
        DBG_SETTINGS log_info("%s", "persisted state: server_settings.json not found, nothing to restore");
        return;
    }

    // read the file length
    fseek(settings_file, 0, SEEK_END);
    file_length = ftell(settings_file);
    fseek(settings_file, 0, SEEK_SET);
    DBG_SETTINGS log_info("%s %lld %s", "persisted state: reading", (int64)file_length, "bytes of server_settings.json");

    // read the file content
    if (file_length > 0 && (uint64)file_length <= SERVER_SETTINGS_MAX_FILE_BYTES)
    {
        file_buffer = (char *)malloc(file_length + 1);
        if (file_buffer != NULL_POINTER)
        {
            bytes_read = fread(file_buffer, 1, file_length, settings_file);
            file_buffer[bytes_read] = 0;
        }
    }
    fclose(settings_file);

    // check the file buffer
    if (file_buffer == NULL_POINTER)
    {
        log_error("Cannot read server_settings.json (empty, unreadable, over 64 MiB, or allocation failed).");
        exit(EXIT_FAILURE);
    }

    // parse the JSON text
    json_root = bytes_read == (uint64)file_length ? cJSON_ParseWithOpts(file_buffer, NULL_POINTER, TRUE) : NULL_POINTER;

    // clear the file buffer
    clib__null_memory(file_buffer, (uint64)file_length + 1);
    free(file_buffer);

    // check the JSON root
    if (!cJSON_IsObject(json_root))
    {
        log_error("server_settings.json must contain one complete JSON object; saved state was not restored.");
        util__json_delete_secure(json_root);
        exit(EXIT_FAILURE);
    }

    // check channels and groups
    if (!_settings_internal__validate_channel_graph(cJSON_GetObjectItemCaseSensitive(json_root, "channels")) || !channel_groups__load_catalog(cJSON_GetObjectItemCaseSensitive(json_root, "channel_group_definitions")))
    {
        log_error("Invalid saved channel layout or channel-group permissions; fix server_settings.json before starting.");
        util__json_delete_secure(json_root);
        exit(EXIT_FAILURE);
    }

    // channels
    json_channels = cJSON_GetObjectItemCaseSensitive(json_root, "channels");
    if (cJSON_IsArray(json_channels) == TRUE)
    {
        cJSON_ArrayForEach(json_channel, json_channels)
        {
            // read the channel id
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "channel_id");
            if (cJSON_IsNumber(json_field) == FALSE)
            {
                continue;
            }

            // check the id range
            channel_id = json_field->valueint;
            if (channel_id < 0 || channel_id >= (int64)g_server_settings.max_channel_count)
            {
                continue;
            }

            // write into the exact saved slot, never a first-free slot, so channel_id stays stable and
            // parent_channel_id references remain valid across the restart
            channel_in_loop = &g_channel_array[channel_id];
            channel_groups__clear(channel_in_loop);
            clib__null_memory(channel_in_loop, sizeof(channel_t));

            // set the start values
            channel_in_loop->channel_id = channel_id;
            channel_in_loop->is_existing = TRUE;
            channel_in_loop->maintainer_id = -1;
            channel_in_loop->is_channel_maintainer_present = FALSE;
            channel_in_loop->maintainer_generation = 0;
            channel_in_loop->is_music_bot_active_in_channel = FALSE;
            channel_in_loop->current_clients = 0;

            // read the required powers
            channel__read_required_powers(json_channel, channel_in_loop);

            // read the parent channel
            channel_in_loop->parent_channel_id = -1;

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "parent_channel_id");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                util__json_read_int64(json_field, -1, (int64)g_server_settings.max_channel_count - 1, &channel_in_loop->parent_channel_id);
            }

            // read the channel type
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "type");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                channel_in_loop->type = json_field->valueint;
            }

            // read the root flag
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_root_channel");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_root_channel = cJSON_IsTrue(json_field);
            }

            // read the password flag
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_using_password");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_using_password = cJSON_IsTrue(json_field);
            }

            // read the audio flag
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_audio_enabled");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_audio_enabled = cJSON_IsTrue(json_field);
            }

            // a settings file written before video streaming existed has no such key: those channels stream
            channel_in_loop->is_video_stream_enabled = TRUE;
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_video_stream_enabled");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_video_stream_enabled = cJSON_IsTrue(json_field);
            }

            // read the limit flag
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_client_limit_active");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_client_limit_active = cJSON_IsTrue(json_field);
            }

            // read the channel groups
            channel_groups__read(channel_in_loop, cJSON_GetObjectItemCaseSensitive(json_channel, "channel_groups"));

            // read the client limit
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "max_client_count");
            if (cJSON_IsNumber(json_field))
            {
                util__json_read_uint64(json_field, MAX_SERVER_SLOTS, &channel_in_loop->max_client_count);
            }

            // read the channel icon
            channel_in_loop->has_channel_icon = FALSE;
            channel_in_loop->icon_id = 0;
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "has_channel_icon");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->has_channel_icon = cJSON_IsTrue(json_field);
            }
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "channel_icon_id");
            if (cJSON_IsNumber(json_field))
            {
                util__json_read_uint64(json_field, MAX_ICONS - 1, &channel_in_loop->icon_id);
            }

            // copy the channel name
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__string_copy(channel_in_loop->name, sizeof(channel_in_loop->name), json_field->valuestring);
            }

            // copy the channel password
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "password");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__string_copy(channel_in_loop->password, sizeof(channel_in_loop->password), json_field->valuestring);
            }

            // copy the channel description
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "description");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__string_copy(channel_in_loop->description, sizeof(channel_in_loop->description), json_field->valuestring);
            }

            // count the loaded channel
            loaded_channels++;
        }
    }

    // icons (id is the array index; the admin icon id 0 is seeded separately, so skip it)
    json_icons = cJSON_GetObjectItemCaseSensitive(json_root, "icons");
    if (cJSON_IsArray(json_icons) == TRUE)
    {
        cJSON_ArrayForEach(json_icon, json_icons)
        {
            // read the icon id
            json_field = cJSON_GetObjectItemCaseSensitive(json_icon, "id");
            if (cJSON_IsNumber(json_field) == FALSE)
            {
                continue;
            }

            if (!util__json_read_int64(json_field, 1, MAX_ICONS - 1, &icon_id))
            {
                continue;
            }

            // reset the icon slot
            icon_in_loop = &g_icons_array[icon_id];
            server_admin__free_icon_base64(icon_in_loop);
            clib__null_memory(icon_in_loop, sizeof(icon_t));
            icon_in_loop->id = icon_id;
            icon_in_loop->is_existing = TRUE;

            // store the icon image
            json_field = cJSON_GetObjectItemCaseSensitive(json_icon, "base64");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                server_admin__set_icon_base64(icon_in_loop, json_field->valuestring);
            }

            // count the loaded icon
            loaded_icons++;
        }
    }

    // groups (id is the array index; the admin group id 0 is seeded separately, so skip it)
    json_groups = cJSON_GetObjectItemCaseSensitive(json_root, "groups");
    if (cJSON_IsArray(json_groups) == TRUE)
    {
        cJSON_ArrayForEach(json_group, json_groups)
        {
            // read the group id
            json_field = cJSON_GetObjectItemCaseSensitive(json_group, "id");
            if (cJSON_IsNumber(json_field) == FALSE)
            {
                continue;
            }

            // check the id range
            group_id = json_field->valueint;
            if (group_id <= 0 || group_id >= MAX_GROUPS || json_field->valuedouble != (double)group_id)
            {
                continue;
            }

            // reset the group slot
            group_in_loop = &g_groups_array[group_id];
            clib__null_memory(group_in_loop, sizeof(group_t));
            permissions__init_grants(&group_in_loop->permissions);
            group_in_loop->id = group_id;
            group_in_loop->is_existing = TRUE;

            // read the group type
            json_field = cJSON_GetObjectItemCaseSensitive(json_group, "group_type");
            if (cJSON_IsString(json_field) == TRUE)
            {
                permissions__parse_group_type(json_field->valuestring, &group_in_loop->group_type);
            }
            group_in_loop->is_default_guest = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json_group, "is_default_guest"));

            // read the icon id
            json_field = cJSON_GetObjectItemCaseSensitive(json_group, "icon_id");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                util__json_read_uint64(json_field, MAX_ICONS - 1, &group_in_loop->icon_id);
            }

            // read the icon flag
            json_field = cJSON_GetObjectItemCaseSensitive(json_group, "has_icon");
            if (cJSON_IsBool(json_field) == TRUE)
            {
                group_in_loop->has_icon = cJSON_IsTrue(json_field);
            }

            // settings written before has_icon existed had a mandatory icon, but icon id 0 is the seeded
            // admin icon - defaulting those to TRUE hands every iconless group the admin shield
            else
            {
                group_in_loop->has_icon = (boole)(group_in_loop->icon_id != 0);
            }

            // copy the group name
            json_field = cJSON_GetObjectItemCaseSensitive(json_group, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__string_copy(group_in_loop->name, sizeof(group_in_loop->name), json_field->valuestring);
            }

            // read the permissions
            json_field = cJSON_GetObjectItemCaseSensitive(json_group, "permissions");
            if (json_field != NULL_POINTER && permissions__read(json_field, &group_in_loop->permissions) == FALSE)
            {
                log_error("Group %llu has invalid saved permissions; fix server_settings.json before starting.", (uint64)group_in_loop->id);
                util__json_delete_secure(json_root);
                exit(EXIT_FAILURE);
            }

            // set the missing powers
            default_channel_power = group_in_loop->group_type == GROUP_TYPE_MODERATOR ? 50 : group_in_loop->group_type == GROUP_TYPE_ADMIN ? PERMISSION_POWER_MAX : 25;
            if (cJSON_GetObjectItemCaseSensitive(json_field, "talk_power") == NULL_POINTER)
            {
                group_in_loop->permissions.powers[PERMISSION_TALK_POWER] = default_channel_power;
            }
            if (cJSON_GetObjectItemCaseSensitive(json_field, "channel_message_power") == NULL_POINTER)
            {
                group_in_loop->permissions.powers[PERMISSION_CHANNEL_MESSAGE_POWER] = default_channel_power;
            }
            if (cJSON_GetObjectItemCaseSensitive(json_field, "channel_picture_power") == NULL_POINTER)
            {
                group_in_loop->permissions.powers[PERMISSION_CHANNEL_PICTURE_POWER] = default_channel_power;
            }
            if (cJSON_GetObjectItemCaseSensitive(json_field, "stream_power") == NULL_POINTER)
            {
                group_in_loop->permissions.powers[PERMISSION_STREAM_POWER] = default_channel_power;
            }

            // count the loaded group
            loaded_groups++;
        }
    }

    // load the ban list (filled sequentially; ip is required, the rest is optional metadata)
    json_bans = cJSON_GetObjectItemCaseSensitive(json_root, "bans");
    if (cJSON_IsArray(json_bans) == TRUE)
    {
        cJSON_ArrayForEach(json_ban, json_bans)
        {
            // stop at ban limit
            if (ban_slot >= MAX_BANS)
            {
                break;
            }

            // read the IP address
            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "ip_address");
            if (cJSON_IsString(json_field) == FALSE || json_field->valuestring == NULL_POINTER)
            {
                continue;
            }

            // create the ban entry
            ban_in_loop = &g_ban_array[ban_slot];
            clib__null_memory(ban_in_loop, sizeof(ban_entry_t));
            ban_in_loop->is_existing = TRUE;
            clib__string_copy(ban_in_loop->ip_address, sizeof(ban_in_loop->ip_address), json_field->valuestring);

            // copy the country code
            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "country_iso_code");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
            {
                clib__string_copy(ban_in_loop->country_iso_code, sizeof(ban_in_loop->country_iso_code), json_field->valuestring);
            }

            // copy the identity
            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "identity");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
            {
                clib__string_copy(ban_in_loop->identity, sizeof(ban_in_loop->identity), json_field->valuestring);
            }

            // copy the extra data
            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "extra_data");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
            {
                clib__string_copy(ban_in_loop->extra_data, sizeof(ban_in_loop->extra_data), json_field->valuestring);
            }

            // read the ban time
            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "timestamp_banned");
            if (cJSON_IsNumber(json_field))
            {
                util__json_read_uint64(json_field, 9007199254740991ULL, &ban_in_loop->timestamp_banned);
            }

            // count the loaded ban
            ban_slot++;
            loaded_bans++;
        }
    }

    // load the identity store (public-key hash -> group ids). only consulted on auth when identities are
    // enabled, but always loaded so the data survives a disable/re-enable cycle. entries with no groups are
    // dropped so they don't occupy a slot
    json_identities = cJSON_GetObjectItemCaseSensitive(json_root, "identities");
    if (cJSON_IsArray(json_identities) == TRUE)
    {
        cJSON_ArrayForEach(json_identity, json_identities)
        {
            // stop at identity limit
            if (identity_slot >= MAX_CLIENT_STORED_DATA)
            {
                break;
            }

            // read the key hash
            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "public_key_hash");
            if (cJSON_IsString(json_field) == FALSE || json_field->valuestring == NULL_POINTER)
            {
                continue;
            }

            // create the identity entry
            identity_in_loop = &g_client_stored_data[identity_slot];
            clib__null_memory(identity_in_loop, sizeof(client_stored_data_t));
            clib__string_copy(identity_in_loop->public_key, sizeof(identity_in_loop->public_key), json_field->valuestring);

            // copy the username
            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "username");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__string_copy(identity_in_loop->username, sizeof(identity_in_loop->username), json_field->valuestring);
            }

            // copy the avatar
            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "base64_avatar");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__string_copy(identity_in_loop->base64_avatar, sizeof(identity_in_loop->base64_avatar), json_field->valuestring);
            }

            // read last seen time
            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "last_seen");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                util__json_read_int64(json_field, 0, 9007199254740991LL, &identity_in_loop->last_seen_unix_seconds);
            }

            // the raw rsa public key, so peers can encrypt to this identity while it is offline.
            // only meaningful while offline messages are enabled
            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "raw_public_key");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__string_copy(identity_in_loop->raw_public_key, sizeof(identity_in_loop->raw_public_key), json_field->valuestring);
            }

            // read the group ids
            identity_in_loop->group_id_count = 0;
            json_identity_group_ids = cJSON_GetObjectItemCaseSensitive(json_identity, "group_ids");
            if (cJSON_IsArray(json_identity_group_ids) == TRUE)
            {
                cJSON_ArrayForEach(json_identity_group_id, json_identity_group_ids)
                {
                    // stop at group limit
                    if (identity_in_loop->group_id_count >= MAX_GROUPS_FOR_SINGLE_CLIENT)
                    {
                        break;
                    }

                    // skip invalid ids
                    if (cJSON_IsNumber(json_identity_group_id) == FALSE || json_identity_group_id->valuedouble != (double)json_identity_group_id->valueint || json_identity_group_id->valueint < 0 || json_identity_group_id->valueint >= MAX_GROUPS)
                    {
                        continue;
                    }

                    // check the group entry
                    group_id = json_identity_group_id->valueint;
                    if (g_groups_array[group_id].is_existing == FALSE || g_groups_array[group_id].is_default_guest == TRUE)
                    {
                        continue;
                    }

                    // skip duplicate ids
                    duplicate_membership = FALSE;
                    for (membership_index = 0; membership_index < identity_in_loop->group_id_count; membership_index++)
                    {
                        if (identity_in_loop->group_ids[membership_index] == (uint64)group_id)
                        {
                            duplicate_membership = TRUE;
                            break;
                        }
                    }
                    if (duplicate_membership == TRUE)
                    {
                        continue;
                    }

                    // add the group id
                    identity_in_loop->group_ids[identity_in_loop->group_id_count] = (uint64)json_identity_group_id->valueint;
                    identity_in_loop->group_id_count++;
                }
            }

            // skip empty identities
            if (identity_in_loop->group_id_count == 0 && identity_in_loop->base64_avatar[0] == 0 && identity_in_loop->raw_public_key[0] == 0)
            {
                DBG_IDENTITIES log_info("%s %s %s", "load_identities: dropping stored identity [", &identity_in_loop->public_key[0], "] - it has no groups, no avatar and no offline messaging key \n");
                clib__null_memory(identity_in_loop, sizeof(client_stored_data_t));
                continue;
            }

            // log the loaded identity
            DBG_IDENTITIES log_info("%s %llu %s %llu %s %s %s", "load_identities: loaded into store slot", identity_slot, "with", (uint64)identity_in_loop->group_id_count, "group(s), hash [", &identity_in_loop->public_key[0], "] \n");

            // count the loaded identity
            identity_slot++;
            loaded_identities++;
        }
    }

    // re-apply the admin group's saved icon link. the admin group was re-seeded with icon_id 0 before this
    // load ran; its runtime-chosen icon (and has_icon flag) is restored here, now that the icons it may
    // point at have been loaded above
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_group_icon_id");
    if (cJSON_IsNumber(json_field))
    {
        util__json_read_uint64(json_field, MAX_ICONS - 1, &g_groups_array[ADMIN_GROUP_ID].icon_id);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_group_has_icon");
    if (cJSON_IsBool(json_field))
    {
        g_groups_array[ADMIN_GROUP_ID].has_icon = cJSON_IsTrue(json_field);
    }

    // load emojis, music bots
    emojis__load(cJSON_GetObjectItemCaseSensitive(json_root, "emojis"));
    musicbot_store__load_reference(json_root);

    // free the JSON
    util__json_delete_secure(json_root);

    // print the loaded counts
    if (loaded_channels > 0 || loaded_icons > 0 || loaded_groups > 0 || loaded_bans > 0 || loaded_identities > 0)
    {
        printf("%s %s%llu%s%llu%s%llu%s%llu%s%llu%s\n", g_mark_ok, "restored from server_settings.json (", loaded_channels, " channels, ", loaded_icons, " icons, ", loaded_groups, " groups, ", loaded_bans, " bans, ", loaded_identities, " identities)");
    }
}

/**
 * @brief Creates the admin group with a grey shield icon. Saved icon choices are loaded afterwards.
 */
void settings__init_groups_and_icons(void)
{
    char group_name[] = "admin";
    cstring base64_icon = "data:image/svg+xml;base64,"
                          "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNiIgaGVpZ2h0PSIxNiIgdmlld0JveD0i"
                          "MCAwIDE2IDE2Ij48cGF0aCBkPSJNOCAxIDE0IDN2NGMwIDQtMyA2LTYgOC0zLTItNi00LTYtOFYzWiIgZmlsbD0iI2U1ZTdl"
                          "YiIgc3Ryb2tlPSIjZjhmYWZjIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lam9pbj0icm91bmQiLz48cGF0aCBkPSJN"
                          "OCAxIDE0IDN2NGMwIDQtMyA2LTYgOC0zLTItNi00LTYtOFYzWiIgZmlsbD0iI2NiZDBkNiIgc3Ryb2tlPSIjNDI0YjU3IiBz"
                          "dHJva2UtbGluZWpvaW49InJvdW5kIi8+PHBhdGggZD0ibTUgNyAyIDIgNC00IiBmaWxsPSJub25lIiBzdHJva2U9IiM0MjRi"
                          "NTciIHN0cm9rZS13aWR0aD0iMS41IiBzdHJva2UtbGluZWNhcD0icm91bmQiIHN0cm9rZS1saW5lam9pbj0icm91bmQiLz48"
                          "L3N2Zz4=";
    group_t *admin_group = NULL_POINTER;
    icon_t *admin_icon = NULL_POINTER;

    // create the admin group
    admin_group = &g_groups_array[ADMIN_GROUP_ID];
    admin_group->id = ADMIN_GROUP_ID;
    admin_group->icon_id = 0;
    admin_group->has_icon = TRUE;
    admin_group->is_existing = TRUE;
    admin_group->group_type = GROUP_TYPE_ADMIN;
    permissions__set_preset(GROUP_TYPE_ADMIN, &admin_group->permissions);
    clib__string_copy(admin_group->name, sizeof(admin_group->name), group_name);

    // create the admin icon
    admin_icon = &g_icons_array[0];
    admin_icon->id = 0;
    admin_icon->is_existing = TRUE;
    server_admin__set_icon_base64(admin_icon, base64_icon);
}

/**
 * @brief Restores the automatic Guest group or creates it in a free slot before clients connect.
 * @return FALSE when every group slot is already occupied and no Guest group was saved.
 */
boole settings__ensure_default_groups(void)
{
    uint64 i = 0;
    uint64 free_id = MAX_GROUPS;
    uint64 existing_count = 0;
    group_t *group = NULL_POINTER;

    // find the guest group
    g_guest_group_id = MAX_GROUPS;
    for (i = 1; i < MAX_GROUPS; i++)
    {
        // store first free slot
        group = &g_groups_array[i];
        if (group->is_existing == FALSE)
        {
            if (free_id == MAX_GROUPS)
            {
                free_id = i;
            }
            continue;
        }

        // count existing groups
        existing_count++;

        // keep one guest group
        if (group->is_default_guest == TRUE)
        {
            if (g_guest_group_id == MAX_GROUPS)
            {
                g_guest_group_id = i;
                group->group_type = GROUP_TYPE_GUEST;
            }
            else
            {
                group->is_default_guest = FALSE;
            }
        }
    }

    // guest group was saved
    if (g_guest_group_id < MAX_GROUPS)
    {
        return TRUE;
    }

    // no free slot
    if (free_id == MAX_GROUPS)
    {
        return FALSE;
    }

    // create the guest group
    group = &g_groups_array[free_id];
    clib__null_memory(group, sizeof(*group));
    group->id = free_id;
    group->is_existing = TRUE;
    group->is_default_guest = TRUE;
    group->group_type = GROUP_TYPE_GUEST;
    clib__string_copy(group->name, sizeof(group->name), "Guest");
    permissions__set_preset(GROUP_TYPE_GUEST, &group->permissions);
    g_guest_group_id = free_id;

    // a new server also starts with a Normal group that the admin can assign
    if (existing_count == 0)
    {
        // find a free slot
        for (i = 1; i < MAX_GROUPS; i++)
        {
            group = &g_groups_array[i];
            if (group->is_existing == FALSE)
            {
                // create the normal group
                clib__null_memory(group, sizeof(*group));
                group->id = i;
                group->is_existing = TRUE;
                group->group_type = GROUP_TYPE_NORMAL;
                clib__string_copy(group->name, sizeof(group->name), "Normal");
                permissions__set_preset(GROUP_TYPE_NORMAL, &group->permissions);
                break;
            }
        }
    }
    return TRUE;
}

/**
 * @brief Builds the client configuration script injected into the bundled web page.
 *
 * @details Connection addresses and plaintext keys are included only when client config embedding is enabled. A configured theme is included, and client policy flags are always included so the page can apply the server settings.
 *
 * @param int64 websocket_port -> the port clients should open the websocket on
 * @param char plaintext_keys[][256] -> the connect keys in cleartext, one per row
 * @param uint64 keys_count -> how many rows of plaintext_keys are filled
 *
 * @return void
 */
static void _settings_internal__build_and_push_client_config(int64 websocket_port, char plaintext_keys[][256], uint64 keys_count)
{
    cJSON *entry = NULL_POINTER;
    char *output = NULL_POINTER;
    const char *input = NULL_POINTER;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;
    cJSON *config_object = NULL_POINTER;
    cJSON *keys_array = NULL_POINTER;
    char *config_json = NULL_POINTER;
    char *config_script = NULL_POINTER;
    uint64 i = 0;

    // build the complete replacement before publishing it to concurrent HTTP requests
    config_object = cJSON_CreateObject();
    if (config_object == NULL_POINTER)
    {
        goto done;
    }

    // include connection details only when the operator enabled embedding
    if (g_server_settings.is_client_config_embedded == TRUE)
    {
        // add the websocket port
        if (cJSON_AddNumberToObject(config_object, "port", (double)websocket_port) == NULL_POINTER)
        {
            goto done;
        }

        // browsers need the public WSS port even when a tunnel provides it
        status0 = (boole)(g_server_settings.is_stunnel_enabled == TRUE || g_server_settings.is_external_wss_enabled == TRUE);
        status1 = (boole)(g_server_settings.wss_port > 0);
        status2 = (boole)(g_server_settings.wss_port <= 65535);
        if (status0 == TRUE && status1 == TRUE && status2 == TRUE)
        {
            if (cJSON_AddNumberToObject(config_object, "wss_port", (double)g_server_settings.wss_port) == NULL_POINTER)
            {
                goto done;
            }
        }

        // create the keys array
        keys_array = cJSON_AddArrayToObject(config_object, "keys");
        if (keys_array == NULL_POINTER)
        {
            goto done;
        }

        // add each connection key
        for (i = 0; i < keys_count; i++)
        {
            entry = cJSON_CreateObject();
            if (entry == NULL_POINTER || cJSON_AddStringToObject(entry, "type", ciphers__name(g_server_settings.keys[i].type)) == NULL_POINTER || cJSON_AddStringToObject(entry, "password", &plaintext_keys[i][0]) == NULL_POINTER)
            {
                goto done;
            }
            status0 = util__json_add_owned_array(keys_array, entry);
            entry = NULL_POINTER;
            if (status0 == FALSE)
            {
                goto done;
            }
        }
    }

    // add the default themes
    if (g_server_settings.default_theme[0] != 0)
    {
        if (cJSON_AddStringToObject(config_object, "theme", &g_server_settings.default_theme[0]) == NULL_POINTER)
        {
            goto done;
        }
    }
    if (g_server_settings.default_mobile_theme[0] != 0)
    {
        if (cJSON_AddStringToObject(config_object, "mobile_theme", &g_server_settings.default_mobile_theme[0]) == NULL_POINTER)
        {
            goto done;
        }
    }

    // page policy is included even when connection passwords are not embedded
    if (cJSON_AddBoolToObject(config_object, "persist_identity", g_server_settings.is_identity_localstorage_enabled == TRUE) == NULL_POINTER || cJSON_AddBoolToObject(config_object, "is_avatar_upload_allowed", g_server_settings.is_avatar_upload_allowed == TRUE) == NULL_POINTER)
    {
        goto done;
    }

    // the two start page buttons; tools/configure-client.py writes the same names
    if (cJSON_AddBoolToObject(config_object, "is_connect_button_shown", g_server_settings.is_start_page_connect_button_enabled == TRUE) == NULL_POINTER || cJSON_AddBoolToObject(config_object, "is_import_identity_button_shown", g_server_settings.is_start_page_import_identity_button_enabled == TRUE) == NULL_POINTER)
    {
        goto done;
    }

    // add the avatar limit
    if (g_server_settings.is_avatar_upload_allowed == TRUE)
    {
        if (cJSON_AddNumberToObject(config_object, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes) == NULL_POINTER)
        {
            goto done;
        }
    }

    // convert JSON to text
    config_json = cJSON_PrintUnformatted(config_object);
    if (config_json == NULL_POINTER)
    {
        goto done;
    }

    // create the script buffer
    config_script = calloc(strlen(config_json) * 6 + 64, 1);
    if (config_script == NULL_POINTER)
    {
        goto done;
    }

    // build the script text
    strcpy(config_script, "window.__SERVER_CONFIG__=");
    output = config_script + strlen(config_script);
    for (input = config_json; *input != 0; input++)
    {
        if (*input == '<')
        {
            memcpy(output, "\\u003c", 6);
            output += 6;
        }
        else
        {
            *output++ = *input;
        }
    }
    *output++ = ';';
    *output = 0;

done:
    // one publication on success; clear old secrets if any construction step failed
    http_server__set_client_config(config_script);

    // free the text buffers
    if (config_script != NULL_POINTER)
    {
        ciphers__clear(config_script, strlen(config_script));
        free(config_script);
    }
    if (config_json != NULL_POINTER)
    {
        clib__null_memory(config_json, strlen(config_json));
        cJSON_free(config_json);
    }

    // free the JSON objects
    util__json_delete_secure(entry);
    util__json_delete_secure(config_object);
}

/**
 * @brief reads the wireguard mode fields from a parsed server_settings.json: the switch, the
 *        router's public address and the forwarded udp range. called by settings__load and by the
 *        first time setup after the tunnel setup wrote the file.
 *
 * @param cJSON* json_root -> the parsed settings file
 *
 * @attention an unusable address or range switches the mode off
 *
 * @return void
 */
void settings__read_wireguard_fields(cJSON *json_root)
{
    cJSON *json_field = NULL_POINTER;

    // read the wireguard switch
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_wireguard_mode_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_wireguard_mode_active = cJSON_IsTrue(json_field);
    }

    // read the public address
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_public_address");
    if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
    {
        clib__null_memory(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address));
        clib__string_copy(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address), json_field->valuestring);
    }

    // read the port range
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_port_range_begin");
    if (cJSON_IsNumber(json_field))
    {
        util__json_read_int64(json_field, 1, 65535, &g_server_settings.webrtc_port_range_begin);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_port_range_end");
    if (cJSON_IsNumber(json_field))
    {
        util__json_read_int64(json_field, 1, 65535, &g_server_settings.webrtc_port_range_end);
    }

    // log the wireguard fields
    DBG_SETTINGS log_info("%s %s %s %s %s %lld %s %lld", "wireguard fields: mode", g_server_settings.is_wireguard_mode_active == TRUE ? "on" : "off", ", public address", g_server_settings.webrtc_public_address, ", port range", (int64)g_server_settings.webrtc_port_range_begin, "to", (int64)g_server_settings.webrtc_port_range_end);

    // an unusable address or range switches the mode off rather than starting with a broken voice path
    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        if (g_server_settings.webrtc_public_address[0] == 0 ||
            !(g_server_settings.webrtc_port_range_begin >= 1024) ||
            !(g_server_settings.webrtc_port_range_begin <= 65535) ||
            !(g_server_settings.webrtc_port_range_end >= g_server_settings.webrtc_port_range_begin) ||
            !(g_server_settings.webrtc_port_range_end <= 65535))
        {
            printf("%s %s\n", g_mark_warn, "wireguard mode: webrtc_public_address or the webrtc_port_range is not usable, mode switched off for this run");
            g_server_settings.is_wireguard_mode_active = FALSE;
        }
    }
}

/**
 * @brief Loads defaults and saved settings, or runs first-time setup when the file is absent.
 *
 * @details An unreadable or malformed existing file stops startup. Missing fields retain their defaults, including the array sizes. Plaintext connection keys stay in a local buffer for setup and client configuration, then the buffer is cleared.
 *
 * @return void
 */
void settings__load(void)
{
    cJSON *password = NULL_POINTER;
    cJSON *type = NULL_POINTER;
    int64 cipher = 0;
    char plaintext_keys[100][256] = { 0 };
    char default_client_name[30] = "user";
    FILE *settings_file = NULL_POINTER;
    int64 file_length = 0;
    char *file_buffer = NULL_POINTER;
    cJSON *json_root = NULL_POINTER;
    uint64 bytes_read = 0;
    cJSON *json_field = NULL_POINTER;
    cJSON *json_key = NULL_POINTER;
    int64 key_index = 0;
    uint64 banner_length = 0;

    // clear settings and keys
    clib__null_memory(&g_server_settings, sizeof(server_settings_t));
    clib__null_memory(plaintext_keys, sizeof(plaintext_keys));

    // set the publishing defaults
    g_server_settings.is_tailscale_funnel_enabled = FALSE;
    g_server_settings.is_server_uptime_published = TRUE;
    g_server_settings.is_server_slots_published = TRUE;
    g_server_settings.is_server_banner_published = TRUE;
    g_server_settings.is_server_features_published = TRUE;

    // set limits and cooldowns
    g_server_settings.websocket_message_max_length = 5000000;
    g_server_settings.websocket_chat_message_string_max_length = 8000;
    g_server_settings.chat_cooldown_milliseconds = 100;
    g_server_settings.join_channel_request_cooldown_milliseconds = 100;
    g_server_settings.create_channel_request_cooldown_milliseconds = 1000;
    g_server_settings.is_same_ip_address_allowed = TRUE;

    // set audio, video defaults
    g_server_settings.is_audio_over_websocket_allowed = TRUE; // both connections allowed: the client picks by its own preference
    g_server_settings.is_audio_over_webrtc_allowed = TRUE;
    g_server_settings.is_voice_chat_active = TRUE;
    g_server_settings.is_music_bot_audio_active = TRUE;
    g_server_settings.is_video_streaming_active = FALSE; // explicit opt-in by the admin in the settings tab

    // set the channel defaults
    g_server_settings.is_hide_clients_in_password_protected_channels_active = TRUE;
    g_server_settings.is_temp_channel_creation_allowed = FALSE;
    g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active = FALSE;

    // set country flag defaults
    g_server_settings.is_display_country_flags_active = FALSE;
    g_server_settings.is_admin_country_flag_hidden = FALSE;

    // set the permission defaults
    g_server_settings.is_permission_system_enabled = TRUE;
    g_server_settings.is_display_admin_group_active = TRUE;

    // set the session defaults
    g_server_settings.is_idle_mode_allowed = TRUE;
    g_server_settings.is_fast_reconnect_allowed = FALSE;
    g_server_settings.is_identity_takeover_allowed = FALSE;
    g_server_settings.is_websocket_ping_active = FALSE;
    g_server_settings.is_connection_check_enforced = TRUE;

    // set key reuse defaults
    g_server_settings.is_channel_key_reuse_enabled = TRUE;
    g_server_settings.is_channel_key_reuse_on_join_allowed = TRUE;
    g_server_settings.is_channel_key_reuse_on_leave_allowed = TRUE;
    g_server_settings.channel_key_reuse_join_min_clients = 50;
    g_server_settings.channel_key_reuse_leave_min_clients = 100;

    // set login, cooldown defaults
    g_server_settings.max_concurrent_logins = 4;
    g_server_settings.webrtc_datachannel_cooldown_seconds = 600;

    // set client feature defaults
    g_server_settings.is_music_bot_marquee_visible_to_everyone = FALSE;
    g_server_settings.is_sending_text_to_idle_clients_allowed = TRUE;
    g_server_settings.is_private_messaging_allowed = TRUE;
    g_server_settings.is_identity_storage_enabled = TRUE;
    g_server_settings.is_identity_localstorage_enabled = FALSE;
    g_server_settings.is_avatar_upload_allowed = FALSE;
    g_server_settings.is_stored_client_list_allowed = FALSE;
    g_server_settings.is_last_seen_enabled = FALSE;
    g_server_settings.is_offline_messaging_allowed = FALSE;
    g_server_settings.is_typing_indicator_allowed = FALSE;
    g_server_settings.is_client_rename_allowed = TRUE;
    g_server_settings.is_link_preview_allowed = FALSE;

    // set the size limits
    g_server_settings.avatar_max_size_bytes = 51200; // 50 KB raw image (~68 KB base64, fits MAX_CLIENT_AVATAR_LENGTH)
    g_server_settings.icon_max_size_bytes = 5000;
    g_server_settings.is_file_upload_allowed = FALSE;
    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_DEFAULT_SIZE_BYTES;
    g_server_settings.chat_picture_max_size_bytes = PICTURE_DEFAULT_SIZE_BYTES;
    g_server_settings.is_chat_picture_allowed = TRUE;

    // set country, RSA defaults
    g_server_settings.is_country_blocking_active = FALSE;
    g_server_settings.minimum_rsa_key_bits = 2048;
    g_server_settings.is_minimum_rsa_key_bits_announced = FALSE;
    g_server_settings.blocked_countries_count = 0;

    // set the logging defaults
    g_server_settings.is_logging_client_joins_enabled = FALSE;
    g_server_settings.is_logging_username_changes_enabled = FALSE;
    g_server_settings.is_logging_group_changes_enabled = FALSE;
    g_server_settings.is_logging_server_settings_updates_enabled = FALSE;
    g_server_settings.is_logging_kicks_and_bans_enabled = FALSE;
    g_server_settings.is_logging_client_disconnects_enabled = FALSE;
    g_server_settings.is_logging_socket_opens_and_closes_enabled = FALSE;
    g_server_settings.is_logging_failed_attempts_enabled = FALSE;
    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_DEFAULT_SIZE_BYTES;
    g_server_settings.admin_log_retention_days = ADMIN_LOG_DEFAULT_RETENTION_DAYS;

    // set the max client/channel counts here too; the JSON path below returns early, so without this the arrays would allocate at size 0
    g_server_settings.dh_exchange_count = 2;
    g_server_settings.session_encryption_layer_count = 2;
    g_server_settings.http_port = 80;
    g_server_settings.session_cipher = CRYPTO_KEY_AES_256_CTR;
    g_server_settings.is_random_cipher_layer_enabled = FALSE;
    g_server_settings.max_client_count = DEFAULT_SERVER_SLOTS;
    g_server_settings.max_channel_count = DEFAULT_CHANNEL_COUNT;
    g_server_settings.channel_count_limit = DEFAULT_CHANNEL_COUNT;

    // wireguard mode is off unless the setup wizard or selfhosting-tools/wireguard/ scripts switch it on; the range is the
    // default the scripts forward
    g_server_settings.is_wireguard_mode_active = FALSE;
    clib__null_memory(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address));
    g_server_settings.webrtc_port_range_begin = 20000;
    g_server_settings.webrtc_port_range_end = 20099;

    // set default client name
    clib__string_copy(g_server_settings.default_client_name, sizeof(g_server_settings.default_client_name), default_client_name);

    // optional non-interactive setup: if server_settings.json exists, load it and skip the prompts below; any omitted field keeps the default set above
    {
        // open the settings file
        settings_file = fopen("server_settings.json", "rb");
        if (settings_file != NULL_POINTER)
        {
            // read the file length
            fseek(settings_file, 0, SEEK_END);
            file_length = ftell(settings_file);
            fseek(settings_file, 0, SEEK_SET);

            // read the file content
            if (file_length > 0 && (uint64)file_length <= SERVER_SETTINGS_MAX_FILE_BYTES)
            {
                file_buffer = (char *)malloc(file_length + 1);
                if (file_buffer != NULL_POINTER)
                {
                    bytes_read = fread(file_buffer, 1, file_length, settings_file);
                    file_buffer[bytes_read] = 0;
                }
            }
            fclose(settings_file);

            // parse the JSON text
            if (file_buffer != NULL_POINTER)
            {
                json_root = bytes_read == (uint64)file_length ? cJSON_ParseWithOpts(file_buffer, NULL_POINTER, TRUE) : NULL_POINTER;
                clib__null_memory(file_buffer, (uint64)file_length + 1);
                free(file_buffer);
            }

            // read the saved settings
            if (cJSON_IsObject(json_root))
            {
                // read the server slots
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "server_slots");
                if (json_field != NULL_POINTER)
                {
                    if (cJSON_IsNumber(json_field) == FALSE || !(json_field->valuedouble >= 1 && json_field->valuedouble <= MAX_SERVER_SLOTS) || json_field->valuedouble != (double)(uint64)json_field->valuedouble)
                    {
                        fprintf(stderr, "server_slots must be an integer from 1 to %d; fix server_settings.json before starting.\n", MAX_SERVER_SLOTS);
                        util__json_delete_secure(json_root);
                        exit(EXIT_FAILURE);
                    }
                    g_server_settings.max_client_count = (uint64)json_field->valuedouble;
                }

                // read the channel limit
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "max_channel_count");
                if (json_field != NULL_POINTER)
                {
                    if (util__json_read_uint64(json_field, MAX_CHANNEL_COUNT, &g_server_settings.channel_count_limit) == FALSE || g_server_settings.channel_count_limit < 1)
                    {
                        fprintf(stderr, "max_channel_count must be an integer from 1 to %d; fix server_settings.json before starting.\n", MAX_CHANNEL_COUNT);
                        util__json_delete_secure(json_root);
                        exit(EXIT_FAILURE);
                    }
                }

                // check the saved channels
                g_server_settings.max_channel_count = g_server_settings.channel_count_limit;
                if (!_settings_internal__validate_channel_graph(cJSON_GetObjectItemCaseSensitive(json_root, "channels")))
                {
                    fprintf(stderr, "Saved channels do not fit max_channel_count or contain an invalid layout; delete or move those channels before lowering the count.\n");
                    util__json_delete_secure(json_root);
                    exit(EXIT_FAILURE);
                }

                // read the websocket port
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "websocket_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    util__json_read_int64(json_field, 1, 65535, &g_server_settings.websocket_port);
                }

                // read the reserved slots
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "reserved_slots");
                g_server_settings.reserved_slots = 0;
                if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 0 && json_field->valuedouble < g_server_settings.max_client_count && json_field->valuedouble == (double)(uint64)json_field->valuedouble)
                {
                    g_server_settings.reserved_slots = (uint64)json_field->valuedouble;
                }

                // read the wireguard fields
                settings__read_wireguard_fields(json_root);

                // read the admin password
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_password");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.admin_password, sizeof(g_server_settings.admin_password), json_field->valuestring);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_admin_password_initial");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_admin_password_initial = cJSON_IsTrue(json_field);
                }

                // read DH exchange count
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "dh_exchange_count");
                if (json_field != NULL_POINTER && (util__json_read_uint64(json_field, LEMON_MAX_DH_EXCHANGES, &g_server_settings.dh_exchange_count) == FALSE || g_server_settings.dh_exchange_count < 1))
                {
                    fprintf(stderr, "dh_exchange_count must be an integer from 1 to %d.\n", LEMON_MAX_DH_EXCHANGES);
                    exit(EXIT_FAILURE);
                }

                // read encryption layer count
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "session_encryption_layer_count");
                if (json_field != NULL_POINTER && (util__json_read_uint64(json_field, LEMON_MAX_SESSION_LAYERS, &g_server_settings.session_encryption_layer_count) == FALSE || g_server_settings.session_encryption_layer_count < 1))
                {
                    fprintf(stderr, "session_encryption_layer_count must be an integer from 1 to %d.\n", LEMON_MAX_SESSION_LAYERS);
                    exit(EXIT_FAILURE);
                }

                // read the session cipher
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "session_cipher");
                if (json_field != NULL_POINTER)
                {
                    if (!cJSON_IsString(json_field) || ciphers__type(json_field->valuestring) == 0)
                    {
                        fprintf(stderr, "Unsupported session_cipher in server_settings.json.\n");
                        exit(EXIT_FAILURE);
                    }
                    g_server_settings.session_cipher = ciphers__type(json_field->valuestring);
                }

                // read random layer switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_random_cipher_layer_enabled");
                if (json_field != NULL_POINTER)
                {
                    if (!cJSON_IsBool(json_field))
                    {
                        fprintf(stderr, "is_random_cipher_layer_enabled must be boolean.\n");
                        exit(EXIT_FAILURE);
                    }
                    g_server_settings.is_random_cipher_layer_enabled = cJSON_IsTrue(json_field);
                }

                // check the keys array
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "keys");
                if (json_field != NULL_POINTER && (!cJSON_IsArray(json_field) || cJSON_GetArraySize(json_field) > 100))
                {
                    fprintf(stderr, "keys must be an array of at most 100 connection passwords.\n");
                    exit(EXIT_FAILURE);
                }

                // read the connection keys
                cJSON_ArrayForEach(json_key, json_field)
                {
                    // read password and cipher
                    password = cJSON_IsString(json_key) ? json_key : cJSON_GetObjectItemCaseSensitive(json_key, "password");
                    type = cJSON_GetObjectItemCaseSensitive(json_key, "type");
                    cipher = cJSON_IsString(json_key) ? LEMON_AES256 : ciphers__type(cJSON_GetStringValue(type));

                    // check the key entry
                    if (!cJSON_IsString(password) || cipher == 0 || strlen(password->valuestring) >= sizeof(plaintext_keys[0]))
                    {
                        fprintf(stderr, "Each connection key must be a password string or a supported {type, password} object (password shorter than 256 bytes).\n");
                        exit(EXIT_FAILURE);
                    }

                    // create key from password
                    if (crypto_metadata__key_from_password(&g_server_settings.keys[key_index], cipher, password->valuestring) == 0)
                    {
                        fprintf(stderr, "Failed to derive a configured encryption key.\n");
                        exit(EXIT_FAILURE);
                    }

                    // store the plaintext password
                    clib__string_copy(plaintext_keys[key_index], sizeof(plaintext_keys[key_index]), password->valuestring);
                    key_index++;
                }

                // store the key count
                g_server_settings.keys_count = key_index;

                // read voice chat switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_voice_chat_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_voice_chat_active = cJSON_IsTrue(json_field);
                }

                // older Android server apps saved one mode string. honor it when the explicit
                // policy fields below are absent; those fields remain authoritative when present
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "audio_transport");
                if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
                {
                    if (clib__is_string_equal(json_field->valuestring, "webrtc") == TRUE)
                    {
                        g_server_settings.is_audio_over_websocket_allowed = FALSE;
                        g_server_settings.is_audio_over_webrtc_allowed = TRUE;
                    }
                    else if (clib__is_string_equal(json_field->valuestring, "websocket") == TRUE)
                    {
                        g_server_settings.is_audio_over_websocket_allowed = TRUE;
                        g_server_settings.is_audio_over_webrtc_allowed = FALSE;
                    }
                }

                // read audio transport switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_audio_over_websocket_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_audio_over_websocket_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_audio_over_webrtc_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_audio_over_webrtc_allowed = cJSON_IsTrue(json_field);
                }

                // read music, video switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_music_bot_audio_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_music_bot_audio_active = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_video_streaming_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_video_streaming_active = cJSON_IsTrue(json_field);
                }

                // read same IP switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_same_ip_address_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_same_ip_address_allowed = cJSON_IsTrue(json_field);
                }

                // read country flag switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_display_country_flags_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_display_country_flags_active = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_admin_country_flag_hidden");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_admin_country_flag_hidden = cJSON_IsTrue(json_field);
                }

                // read the channel switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_hide_clients_in_password_protected_channels_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_hide_clients_in_password_protected_channels_active = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_temp_channel_creation_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_temp_channel_creation_allowed = cJSON_IsTrue(json_field);
                }

                // read idle mode switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_idle_mode_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_idle_mode_allowed = cJSON_IsTrue(json_field);
                }

                // read reconnect, takeover switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_fast_reconnect_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_fast_reconnect_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_identity_takeover_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_identity_takeover_allowed = cJSON_IsTrue(json_field);
                }

                // read websocket ping switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_websocket_ping_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_websocket_ping_active = cJSON_IsTrue(json_field);
                }

                // read key reuse switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_channel_key_reuse_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_channel_key_reuse_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_channel_key_reuse_on_join_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_channel_key_reuse_on_join_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_channel_key_reuse_on_leave_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_channel_key_reuse_on_leave_allowed = cJSON_IsTrue(json_field);
                }

                // read key reuse limits
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "channel_key_reuse_join_min_clients");
                if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 30 && json_field->valuedouble <= 1000000)
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.channel_key_reuse_join_min_clients);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "channel_key_reuse_leave_min_clients");
                if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 30 && json_field->valuedouble <= 1000000)
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.channel_key_reuse_leave_min_clients);
                }

                // read the permission switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_permission_system_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_permission_system_enabled = cJSON_IsTrue(json_field);
                }

                // read connection check switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_connection_check_enforced");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_connection_check_enforced = cJSON_IsTrue(json_field);
                }

                // read attachment blocking
                settings__read_attachment_receive_policy(json_root);

                // read the login limit
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "max_concurrent_logins");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 1, 64, &g_server_settings.max_concurrent_logins);
                    if (g_server_settings.max_concurrent_logins < 1)
                    {
                        g_server_settings.max_concurrent_logins = 1;
                    }
                    if (g_server_settings.max_concurrent_logins > 64)
                    {
                        g_server_settings.max_concurrent_logins = 64;
                    }
                }

                // read the datachannel cooldown
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_datachannel_cooldown_seconds");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.webrtc_datachannel_cooldown_seconds);
                    if (g_server_settings.webrtc_datachannel_cooldown_seconds < 0)
                    {
                        g_server_settings.webrtc_datachannel_cooldown_seconds = 0;
                    }
                    if (g_server_settings.webrtc_datachannel_cooldown_seconds > 86400)
                    {
                        g_server_settings.webrtc_datachannel_cooldown_seconds = 86400;
                    }
                }

                // read the marquee switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_music_bot_marquee_visible_to_everyone");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_music_bot_marquee_visible_to_everyone = cJSON_IsTrue(json_field);
                }

                // read the restart switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_restart_on_crash_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_restart_on_crash_enabled = cJSON_IsTrue(json_field);
                }

                // read identity storage switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_identity_storage_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_identity_storage_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_identity_localstorage_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_identity_localstorage_enabled = cJSON_IsTrue(json_field);
                }

                // read avatar upload switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_avatar_upload_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_avatar_upload_allowed = cJSON_IsTrue(json_field);
                }

                // read stored client switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_stored_client_list_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_stored_client_list_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_last_seen_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_last_seen_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_offline_messaging_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_offline_messaging_allowed = cJSON_IsTrue(json_field);
                }

                // read client feature switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_typing_indicator_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_typing_indicator_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_client_rename_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_client_rename_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_link_preview_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_link_preview_allowed = cJSON_IsTrue(json_field);
                }

                // read the avatar size
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "avatar_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.avatar_max_size_bytes);
                }

                // read the icon size
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "icon_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.icon_max_size_bytes);
                    if (g_server_settings.icon_max_size_bytes < 1000)
                    {
                        g_server_settings.icon_max_size_bytes = 1000;
                    }
                    if (g_server_settings.icon_max_size_bytes > 30000)
                    {
                        g_server_settings.icon_max_size_bytes = 30000;
                    }
                }

                // read the Tailscale switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_tailscale_funnel_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_tailscale_funnel_enabled = cJSON_IsTrue(json_field);
                }

                // optional bundled-stunnel front-end (wss)
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_stunnel_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_stunnel_enabled = cJSON_IsTrue(json_field);
                }

                // read external WSS switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_external_wss_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_external_wss_enabled = cJSON_IsTrue(json_field);
                }

                // disable stunnel on conflict
                if (g_server_settings.is_tailscale_funnel_enabled == TRUE && g_server_settings.is_stunnel_enabled == TRUE)
                {
                    printf("%s %s\n", g_mark_warn, "Tailscale Funnel is enabled. Bundled stunnel is disabled for this run.");
                    g_server_settings.is_stunnel_enabled = FALSE;
                }
                if (g_server_settings.is_external_wss_enabled == TRUE)
                {
                    g_server_settings.is_stunnel_enabled = FALSE;
                }

                // read the WSS port
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "wss_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    util__json_read_int64(json_field, 1, 65535, &g_server_settings.wss_port);
                }

                // read the stunnel domain
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_domain");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain), json_field->valuestring);
                }

                // read the stunnel certificate
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_cert_fullchain");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain), json_field->valuestring);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_cert_privkey");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey), json_field->valuestring);
                }

                // read client HTML destination
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "client_html_dest");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.client_html_dest, sizeof(g_server_settings.client_html_dest), json_field->valuestring);
                }

                // optional bundled http server that serves the client (mainly for LAN testing)
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_client_http_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_client_http_enabled = cJSON_IsTrue(json_field);
                }

                // read blank page switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_client_http_blank_when_disabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_client_http_blank_when_disabled = cJSON_IsTrue(json_field);
                }

                // read the HTTP port
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    util__json_read_int64(json_field, 1, 65535, &g_server_settings.http_port);
                }

                // read the HTTPS settings
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_https_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_https_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "https_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    util__json_read_int64(json_field, 1, 65535, &g_server_settings.https_port);
                }

                // read the web root
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_webroot");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.http_webroot, sizeof(g_server_settings.http_webroot), json_field->valuestring);
                }

                // read the default themes
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "default_theme");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.default_theme, sizeof(g_server_settings.default_theme), json_field->valuestring);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "default_mobile_theme");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__string_copy(g_server_settings.default_mobile_theme, sizeof(g_server_settings.default_mobile_theme), json_field->valuestring);
                }

                // read the publishing switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_server_uptime_published");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_server_uptime_published = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_server_slots_published");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_server_slots_published = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_server_banner_published");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_server_banner_published = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_server_features_published");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_server_features_published = cJSON_IsTrue(json_field);
                }

                // read the server banner
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "server_banner");
                if (cJSON_IsString(json_field) && server_admin__is_server_banner_valid(json_field->valuestring) == TRUE)
                {
                    banner_length = clib__utf8_string_length(json_field->valuestring);
                    if (banner_length > 0)
                    {
                        g_server_settings.server_banner = (char *)memorymanager__allocate(banner_length + 1, MEMALLOC_SERVER_BANNER);
                        if (g_server_settings.server_banner != NULL_POINTER)
                        {
                            clib__copy_memory(json_field->valuestring, g_server_settings.server_banner, banner_length + 1, banner_length + 1);
                        }
                    }
                }

                // read config embedding switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_client_config_embedded");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_client_config_embedded = cJSON_IsTrue(json_field);
                }

                // read start page buttons
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_start_page_connect_button_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_start_page_connect_button_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_start_page_import_identity_button_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_start_page_import_identity_button_enabled = cJSON_IsTrue(json_field);
                }

                // read the messaging switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_sending_text_to_idle_clients_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_sending_text_to_idle_clients_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_private_messaging_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_private_messaging_allowed = cJSON_IsTrue(json_field);
                }

                // read file upload settings
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_file_upload_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_file_upload_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "file_upload_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.file_upload_max_size_bytes);
                }

                // read RSA key settings
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "minimum_rsa_key_bits");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.minimum_rsa_key_bits);
                    if (g_server_settings.minimum_rsa_key_bits < 2048)
                    {
                        g_server_settings.minimum_rsa_key_bits = 2048;
                    }
                    if (g_server_settings.minimum_rsa_key_bits > 8192)
                    {
                        g_server_settings.minimum_rsa_key_bits = 8192;
                    }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_minimum_rsa_key_bits_announced");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_minimum_rsa_key_bits_announced = cJSON_IsTrue(json_field);
                }

                // limit file upload size
                if (g_server_settings.file_upload_max_size_bytes < FILE_UPLOAD_MIN_SIZE_BYTES)
                {
                    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MIN_SIZE_BYTES;
                }
                if (g_server_settings.file_upload_max_size_bytes > FILE_UPLOAD_MAX_SIZE_BYTES)
                {
                    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MAX_SIZE_BYTES;
                }

                // read the picture size
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "chat_picture_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.chat_picture_max_size_bytes);
                }
                if (g_server_settings.chat_picture_max_size_bytes < PICTURE_MIN_SIZE_BYTES)
                {
                    g_server_settings.chat_picture_max_size_bytes = PICTURE_MIN_SIZE_BYTES;
                }
                if (g_server_settings.chat_picture_max_size_bytes > PICTURE_MAX_SIZE_BYTES)
                {
                    g_server_settings.chat_picture_max_size_bytes = PICTURE_MAX_SIZE_BYTES;
                }

                // read the picture switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_chat_picture_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_chat_picture_allowed = cJSON_IsTrue(json_field);
                }

                // read country blocking switch
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_country_blocking_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_country_blocking_active = cJSON_IsTrue(json_field);
                }

                // read the blocked countries
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "blocked_countries");
                if (cJSON_IsArray(json_field))
                {
                    g_server_settings.blocked_countries_count = 0;
                    cJSON_ArrayForEach(json_key, json_field)
                    {
                        if (g_server_settings.blocked_countries_count >= MAX_BLOCKED_COUNTRIES)
                        {
                            break;
                        }
                        if (util__normalize_country_code(json_key, &g_server_settings.blocked_countries[g_server_settings.blocked_countries_count][0]) == TRUE)
                        {
                            g_server_settings.blocked_countries_count++;
                        }
                    }
                }

                // read client logging switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_client_joins_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_client_joins_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_username_changes_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_username_changes_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_group_changes_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_group_changes_enabled = cJSON_IsTrue(json_field);
                }

                // read admin logging switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_server_settings_updates_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_server_settings_updates_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_kicks_and_bans_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_kicks_and_bans_enabled = cJSON_IsTrue(json_field);
                }

                // read connection log switches
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_client_disconnects_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_client_disconnects_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_socket_opens_and_closes_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_socket_opens_and_closes_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_failed_attempts_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_failed_attempts_enabled = cJSON_IsTrue(json_field);
                }

                // read admin log size
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_log_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.admin_log_max_size_bytes);
                }
                if (g_server_settings.admin_log_max_size_bytes < ADMIN_LOG_MIN_SIZE_BYTES)
                {
                    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MIN_SIZE_BYTES;
                }
                if (g_server_settings.admin_log_max_size_bytes > ADMIN_LOG_MAX_SIZE_BYTES)
                {
                    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MAX_SIZE_BYTES;
                }

                // read log retention days
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_log_retention_days");
                if (cJSON_IsNumber(json_field))
                {
                    util__json_read_int64(json_field, 0, INT64_MAX, &g_server_settings.admin_log_retention_days);
                }
                if (g_server_settings.admin_log_retention_days < 1)
                {
                    g_server_settings.admin_log_retention_days = 1;
                }
                if (g_server_settings.admin_log_retention_days > ADMIN_LOG_MAX_RETENTION_DAYS)
                {
                    g_server_settings.admin_log_retention_days = ADMIN_LOG_MAX_RETENTION_DAYS;
                }

                // update the client config
                memcpy(g_server_settings.plaintext_keys, plaintext_keys, sizeof(plaintext_keys));
                settings__refresh_client_config();

                // free the JSON
                util__json_delete_secure(json_root);

                // clear the local keys
                clib__null_memory(plaintext_keys, sizeof(plaintext_keys));
                return;
            }

            // report the load failure
            log_error("Cannot load server_settings.json: require a complete JSON object no larger than 64 MiB. Existing settings were left unchanged.");
            util__json_delete_secure(json_root);
            clib__null_memory(plaintext_keys, sizeof(plaintext_keys));
            exit(EXIT_FAILURE);
        }
    }

    // run first time setup
    first_time_setup__run(plaintext_keys);

    // update the client config
    memcpy(g_server_settings.plaintext_keys, plaintext_keys, sizeof(plaintext_keys));
    settings__refresh_client_config();

    // clear the local keys
    clib__null_memory(plaintext_keys, sizeof(plaintext_keys));
}

/**
 * @brief Writes a bounded settings document through a temporary file, flushes it, then atomically
 *        replaces the target. The previous version stays intact until replacement succeeds. POSIX
 *        temporary files are created exclusively with owner-only permissions.
 *
 * @param char* path -> the destination file path
 * @param char* contents -> the NUL-terminated text to write
 *
 * @return boole -> TRUE only if the file was fully written and atomically put in place
 */
boole settings__write_file_atomically(char *path, char *contents)
{
#ifndef WIN32
    int descriptor = 0; // native file descriptor used by fdopen, fchmod and close
#endif

#ifdef WIN32
    uint64 replace_error = 0;
    uint64 attempt = 0;
#endif

    /* A leaf mutex: callers may hold state locks, but no state lock is acquired
     * here. It also protects the Windows fixed temporary-file path. */
    static pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;
    char tmp_path[1024] = { 0 };
    FILE *file = NULL_POINTER;
    int64 contents_length = 0;
    boole succeeded = FALSE;
    boole io_failed = FALSE;
    int64 path_length = 0;

    // check arguments
    if (path == NULL_POINTER || contents == NULL_POINTER)
    {
        return FALSE;
    }

    // check the content length
    contents_length = clib__string_length(contents, SERVER_SETTINGS_MAX_FILE_BYTES + 1);
    if (contents_length < 0)
    {
        // never replace a readable file with one that every settings reader rejects
        return FALSE;
    }
#ifdef WIN32
    path_length = clib__string_format(tmp_path, sizeof(tmp_path), "%s.tmp", path);
#else
    path_length = clib__string_format(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
#endif

    // check the temporary path
    if (path_length < 0 || (uint64)path_length >= sizeof(tmp_path))
    {
        return FALSE;
    }

    // lock the write mutex
    pthread_mutex_lock(&write_mutex);
#ifdef WIN32
    // open the temporary file
    file = fopen(tmp_path, "wb");
#else
    // create the temporary file
    descriptor = mkstemp(tmp_path);
    if (descriptor < 0)
    {
        goto atomic_write_end;
    }

    // limit access to owner
    if (fchmod(descriptor, S_IRUSR | S_IWUSR) == 0)
    {
        file = fdopen(descriptor, "wb");
    }

    // delete file on failure
    if (file == NULL_POINTER)
    {
        close(descriptor);
        remove(tmp_path);
        goto atomic_write_end;
    }
#endif

    // check the opened file
    if (file == NULL_POINTER)
    {
        goto atomic_write_end;
    }

    // write the temporary file
    io_failed = (boole)(fwrite(contents, 1, (uint64)contents_length, file) != (uint64)contents_length);
    if (fflush(file) != 0)
    {
        io_failed = TRUE;
    }
#ifdef WIN32
    if (_commit(_fileno(file)) != 0)
    {
        io_failed = TRUE;
    }
#else
    if (fsync(fileno(file)) != 0)
    {
        io_failed = TRUE;
    }
#endif
    if (fclose(file) != 0)
    {
        io_failed = TRUE;
    }

    // replace the target file
    if (io_failed == FALSE)
    {
#ifdef WIN32
        replace_error = 0;

        // Windows scanners/editors may briefly hold a handle without delete
        // sharing. retry only those errors, with a bounded wait; never remove
        // the original file to make replacement succeed
        for (attempt = 0; attempt < 10; attempt++)
        {
            // try the replacement
            succeeded = (boole)(MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
            if (succeeded == TRUE)
            {
                break;
            }

            // stop on other errors
            replace_error = GetLastError();
            if (replace_error != ERROR_ACCESS_DENIED && replace_error != ERROR_SHARING_VIOLATION && replace_error != ERROR_LOCK_VIOLATION)
            {
                break;
            }

            // wait before next attempt
            if (attempt < 9)
            {
                Sleep(20);
            }
        }

        // report the failure
        if (succeeded == FALSE)
        {
            log_error("Atomic replacement of %s failed (Windows error %llu)", path, replace_error);
        }
#else
        succeeded = (boole)(rename(tmp_path, path) == 0);
#endif
    }

    // remove the temporary file
    if (succeeded == FALSE)
    {
        remove(tmp_path);
    }

atomic_write_end:
    // unlock the write mutex
    pthread_mutex_unlock(&write_mutex);
    return succeeded;
}

/**
 * @brief Serializes stored identities into the JSON identities array under the store mutex. When
 *        identity storage is disabled, the existing JSON identities are preserved.
 *
 * @details Entries need a hash and at least one group, avatar or offline messaging key.
 *
 * @param cJSON* json_root -> the settings document being written
 *
 * @return void
 */
static boole _settings_internal__serialize_identities(cJSON *json_root)
{
    cJSON *json_identities = NULL_POINTER;
    cJSON *json_identity = NULL_POINTER;
    cJSON *json_identity_group_ids = NULL_POINTER;
    uint64 i = 0;
    uint64 t = 0;

    // identity storage is off
    if (g_server_settings.is_identity_storage_enabled == FALSE)
    {
        DBG_IDENTITIES log_info("%s", "serialize_identities: identities disabled -> leaving the existing \"identities\" json key untouched \n");
        return TRUE;
    }

    // lock the identity store
    pthread_mutex_lock(&g_client_stored_data_mutex);

    // create the identities array
    util__json_delete_item_secure(json_root, "identities");
    json_identities = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "identities", json_identities))
    {
        goto json_failure;
    }

    // write each identity
    for (i = 0; i < MAX_CLIENT_STORED_DATA; i++)
    {
        // keep a slot that has groups, an avatar or an offline messaging key (any of them makes the identity worth storing)
        if (g_client_stored_data[i].public_key[0] == 0 || (g_client_stored_data[i].group_id_count == 0 && g_client_stored_data[i].base64_avatar[0] == 0 && g_client_stored_data[i].raw_public_key[0] == 0))
        {
            continue;
        }

        // log the identity slot
        DBG_IDENTITIES log_info("%s %llu %s %llu %s %s %s", "serialize_identities: writing store slot", i, "with", (uint64)g_client_stored_data[i].group_id_count, "group(s), hash [", &g_client_stored_data[i].public_key[0], "] \n");

        // create the identity object
        json_identity = cJSON_CreateObject();
        if (!util__json_add_owned_array(json_identities, json_identity))
        {
            goto json_failure;
        }

        // add hash and username
        if (!cJSON_AddStringToObject(json_identity, "public_key_hash", &g_client_stored_data[i].public_key[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_identity, "username", &g_client_stored_data[i].username[0]))
        {
            goto json_failure;
        }

        // add the avatar
        if (g_client_stored_data[i].base64_avatar[0] != 0)
        {
            if (!cJSON_AddStringToObject(json_identity, "base64_avatar", &g_client_stored_data[i].base64_avatar[0]))
            {
                goto json_failure;
            }
        }

        // only ever written while the admin keeps last-seen enabled: switching the setting off stops
        // recording AND stops the recorded values from being carried into the next save
        if (g_server_settings.is_last_seen_enabled == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            if (!cJSON_AddNumberToObject(json_identity, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds))
            {
                goto json_failure;
            }
        }

        // same rule for the raw public key: written only while offline messages are enabled, so
        // turning the feature off also stops carrying the keys into the next save
        if (g_server_settings.is_offline_messaging_allowed == TRUE && g_client_stored_data[i].raw_public_key[0] != 0)
        {
            if (!cJSON_AddStringToObject(json_identity, "raw_public_key", &g_client_stored_data[i].raw_public_key[0]))
            {
                goto json_failure;
            }
        }

        // add the group ids
        json_identity_group_ids = cJSON_CreateArray();
        if (!util__json_add_owned_object(json_identity, "group_ids", json_identity_group_ids))
        {
            goto json_failure;
        }
        for (t = 0; t < g_client_stored_data[i].group_id_count; t++)
        {
            if (!util__json_add_owned_array(json_identity_group_ids, cJSON_CreateNumber((double)g_client_stored_data[i].group_ids[t])))
            {
                goto json_failure;
            }
        }
    }

    // unlock the identity store
    pthread_mutex_unlock(&g_client_stored_data_mutex);
    return TRUE;

json_failure:
    // unlock after a failure
    pthread_mutex_unlock(&g_client_stored_data_mutex);
    return FALSE;
}

/**
 * @brief saves server settings to a file
 *
 * @note caller holds the channels, icons, groups and bans locks for reading
 *
 * @return boole -> TRUE if the file was written
 */
boole settings__save_server_settings_to_file(void)
{
    cJSON *connection_passwords = NULL_POINTER;
    uint64 key_index = 0;
    cJSON *entry = NULL_POINTER;
    cJSON *json_root = NULL_POINTER;
    cJSON *json_channels = NULL_POINTER;
    cJSON *json_channel = NULL_POINTER;
    cJSON *json_icons = NULL_POINTER;
    cJSON *json_icon = NULL_POINTER;
    cJSON *json_emojis = NULL_POINTER;
    cJSON *json_groups = NULL_POINTER;
    cJSON *json_group = NULL_POINTER;
    cJSON *json_bans = NULL_POINTER;
    cJSON *json_ban = NULL_POINTER;
    cJSON *json_blocked_countries = NULL_POINTER;
    char *json_text = NULL_POINTER;
    FILE *settings_file = NULL_POINTER;
    int64 file_length = 0;
    char *file_buffer = NULL_POINTER;
    uint64 bytes_read = 0;
    channel_t *channel_in_loop = NULL_POINTER;
    icon_t *icon_in_loop = NULL_POINTER;
    group_t *group_in_loop = NULL_POINTER;
    ban_entry_t *ban_in_loop = NULL_POINTER;
    uint64 i = 0;
    boole write_succeeded = FALSE;

    // read the existing file first to preserve settings that this editor does not change
    settings_file = fopen("server_settings.json", "rb");
    if (settings_file == NULL_POINTER)
    {
        log_info("%s", "save_server_settings: server_settings.json not found, refusing to overwrite");
        return FALSE;
    }

    // read the file length
    fseek(settings_file, 0, SEEK_END);
    file_length = ftell(settings_file);
    fseek(settings_file, 0, SEEK_SET);

    // read the file content
    if (file_length > 0 && (uint64)file_length <= SERVER_SETTINGS_MAX_FILE_BYTES)
    {
        file_buffer = (char *)malloc(file_length + 1);
        if (file_buffer != NULL_POINTER)
        {
            bytes_read = fread(file_buffer, 1, file_length, settings_file);
            file_buffer[bytes_read] = 0;
        }
    }
    fclose(settings_file);

    // check the file buffer
    if (file_buffer == NULL_POINTER)
    {
        return FALSE;
    }

    // parse the JSON text
    json_root = bytes_read == (uint64)file_length ? cJSON_ParseWithOpts(file_buffer, NULL_POINTER, TRUE) : NULL_POINTER;

    // clear the file buffer
    clib__null_memory(file_buffer, (uint64)file_length + 1);
    free(file_buffer);

    // check the JSON root
    if (!cJSON_IsObject(json_root))
    {
        util__json_delete_secure(json_root);
        log_info("%s", "save_server_settings: existing server_settings.json is not a complete object, refusing to overwrite");
        return FALSE;
    }

    // update the general-settings toggles (delete-then-add so a value already present is replaced)
    util__json_delete_item_secure(json_root, "server_banner");
    if (cJSON_AddStringToObject(json_root, "server_banner", g_server_settings.server_banner != NULL_POINTER ? g_server_settings.server_banner : "") == NULL_POINTER)
    {
        util__json_delete_secure(json_root);
        return FALSE;
    }
    util__json_delete_item_secure(json_root, "is_server_uptime_published");

    // write channel, slot limits
    util__json_delete_item_secure(json_root, "reserved_slots");
    util__json_delete_item_secure(json_root, "max_channel_count");
    if (!cJSON_AddNumberToObject(json_root, "max_channel_count", (double)g_server_settings.channel_count_limit))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "reserved_slots", (double)g_server_settings.reserved_slots))
    {
        goto json_failure;
    }

    // write channel group definitions
    util__json_delete_item_secure(json_root, "channel_group_definitions");
    if (!util__json_add_owned_object(json_root, "channel_group_definitions", channel_groups__write_catalog(TRUE)))
    {
        goto json_failure;
    }

    // write the publishing switches
    util__json_delete_item_secure(json_root, "is_server_slots_published");
    util__json_delete_item_secure(json_root, "is_server_banner_published");
    util__json_delete_item_secure(json_root, "is_server_features_published");
    if (cJSON_AddBoolToObject(json_root, "is_server_uptime_published", g_server_settings.is_server_uptime_published) == NULL_POINTER || cJSON_AddBoolToObject(json_root, "is_server_slots_published", g_server_settings.is_server_slots_published) == NULL_POINTER || cJSON_AddBoolToObject(json_root, "is_server_banner_published", g_server_settings.is_server_banner_published) == NULL_POINTER || cJSON_AddBoolToObject(json_root, "is_server_features_published", g_server_settings.is_server_features_published) == NULL_POINTER)
    {
        util__json_delete_secure(json_root);
        return FALSE;
    }

    // write country flag switches
    util__json_delete_item_secure(json_root, "is_display_country_flags_active");
    if (!util__json_add_owned_object(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_admin_country_flag_hidden");
    if (!util__json_add_owned_object(json_root, "is_admin_country_flag_hidden", cJSON_CreateBool(g_server_settings.is_admin_country_flag_hidden == TRUE)))
    {
        goto json_failure;
    }

    // write voice chat switch
    util__json_delete_item_secure(json_root, "is_voice_chat_active");
    if (!util__json_add_owned_object(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE)))
    {
        goto json_failure;
    }

    // write audio transport switches
    util__json_delete_item_secure(json_root, "is_audio_over_websocket_allowed");
    if (!util__json_add_owned_object(json_root, "is_audio_over_websocket_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_websocket_allowed == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_audio_over_webrtc_allowed");
    if (!util__json_add_owned_object(json_root, "is_audio_over_webrtc_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_webrtc_allowed == TRUE)))
    {
        goto json_failure;
    }

    // write music, video switches
    util__json_delete_item_secure(json_root, "is_music_bot_audio_active");
    if (!util__json_add_owned_object(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_video_streaming_active");
    if (!util__json_add_owned_object(json_root, "is_video_streaming_active", cJSON_CreateBool(g_server_settings.is_video_streaming_active == TRUE)))
    {
        goto json_failure;
    }

    // write the channel switches
    util__json_delete_item_secure(json_root, "is_hide_clients_in_password_protected_channels_active");
    if (!util__json_add_owned_object(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_temp_channel_creation_allowed");
    if (!util__json_add_owned_object(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE)))
    {
        goto json_failure;
    }

    // write client feature switches
    util__json_delete_item_secure(json_root, "is_typing_indicator_allowed");
    if (!util__json_add_owned_object(json_root, "is_typing_indicator_allowed", cJSON_CreateBool(g_server_settings.is_typing_indicator_allowed == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_client_rename_allowed");
    if (!util__json_add_owned_object(json_root, "is_client_rename_allowed", cJSON_CreateBool(g_server_settings.is_client_rename_allowed == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_link_preview_allowed");
    if (!cJSON_AddBoolToObject(json_root, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed))
    {
        goto json_failure;
    }

    // write the messaging switches
    util__json_delete_item_secure(json_root, "is_sending_text_to_idle_clients_allowed");
    if (!util__json_add_owned_object(json_root, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_private_messaging_allowed");
    if (!util__json_add_owned_object(json_root, "is_private_messaging_allowed", cJSON_CreateBool(g_server_settings.is_private_messaging_allowed == TRUE)))
    {
        goto json_failure;
    }

    // write same IP switch
    util__json_delete_item_secure(json_root, "is_same_ip_address_allowed");
    if (!util__json_add_owned_object(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE)))
    {
        goto json_failure;
    }

    // write reconnect, takeover switches
    util__json_delete_item_secure(json_root, "is_fast_reconnect_allowed");
    if (!util__json_add_owned_object(json_root, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_identity_takeover_allowed");
    if (!util__json_add_owned_object(json_root, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE)))
    {
        goto json_failure;
    }

    // write websocket ping switch
    util__json_delete_item_secure(json_root, "is_websocket_ping_active");
    if (!util__json_add_owned_object(json_root, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE)))
    {
        goto json_failure;
    }

    // write key reuse switches
    util__json_delete_item_secure(json_root, "is_channel_key_reuse_enabled");
    if (!cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_channel_key_reuse_on_join_allowed");
    if (!cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_channel_key_reuse_on_leave_allowed");
    if (!cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed))
    {
        goto json_failure;
    }

    // write key reuse limits
    util__json_delete_item_secure(json_root, "channel_key_reuse_join_min_clients");
    util__json_delete_item_secure(json_root, "channel_key_reuse_leave_min_clients");
    if (!cJSON_AddNumberToObject(json_root, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients))
    {
        goto json_failure;
    }

    // write permission, connection switches
    util__json_delete_item_secure(json_root, "is_permission_system_enabled");
    util__json_delete_item_secure(json_root, "is_connection_check_enforced");
    if (!cJSON_AddBoolToObject(json_root, "is_permission_system_enabled", g_server_settings.is_permission_system_enabled == TRUE))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced == TRUE))
    {
        goto json_failure;
    }

    // write the login limit
    util__json_delete_item_secure(json_root, "max_concurrent_logins");
    if (!cJSON_AddNumberToObject(json_root, "max_concurrent_logins", (double)g_server_settings.max_concurrent_logins))
    {
        goto json_failure;
    }

    // delete unused limit settings
    util__json_delete_item_secure(json_root, "outgoing_queue_limit_mb");
    util__json_delete_item_secure(json_root, "server_ram_limit_mb");

    // write attachment blocking switches
    util__json_delete_item_secure(json_root, "is_blocking_direct_chat_pictures_active");
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_pictures_active", g_server_settings.is_blocking_direct_chat_pictures_active))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_blocking_direct_chat_files_active");
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_files_active", g_server_settings.is_blocking_direct_chat_files_active))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_blocking_channel_chat_pictures_active");
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_pictures_active", g_server_settings.is_blocking_channel_chat_pictures_active))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_blocking_channel_chat_files_active");
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_files_active", g_server_settings.is_blocking_channel_chat_files_active))
    {
        goto json_failure;
    }

    // write the datachannel cooldown
    util__json_delete_item_secure(json_root, "webrtc_datachannel_cooldown_seconds");
    if (!cJSON_AddNumberToObject(json_root, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds))
    {
        goto json_failure;
    }

    // write the icon size
    util__json_delete_item_secure(json_root, "icon_max_size_bytes");
    if (!cJSON_AddNumberToObject(json_root, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes))
    {
        goto json_failure;
    }

    // write the marquee switch
    util__json_delete_item_secure(json_root, "is_music_bot_marquee_visible_to_everyone");
    if (!util__json_add_owned_object(json_root, "is_music_bot_marquee_visible_to_everyone", cJSON_CreateBool(g_server_settings.is_music_bot_marquee_visible_to_everyone == TRUE)))
    {
        goto json_failure;
    }

    // write file upload settings
    util__json_delete_item_secure(json_root, "is_file_upload_allowed");
    if (!util__json_add_owned_object(json_root, "is_file_upload_allowed", cJSON_CreateBool(g_server_settings.is_file_upload_allowed == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "file_upload_max_size_bytes");
    if (!cJSON_AddNumberToObject(json_root, "file_upload_max_size_bytes", (double)g_server_settings.file_upload_max_size_bytes))
    {
        goto json_failure;
    }

    // write avatar, picture settings
    util__json_delete_item_secure(json_root, "avatar_max_size_bytes");
    if (!cJSON_AddNumberToObject(json_root, "avatar_max_size_bytes", (double)g_server_settings.avatar_max_size_bytes))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "chat_picture_max_size_bytes");
    if (!cJSON_AddNumberToObject(json_root, "chat_picture_max_size_bytes", (double)g_server_settings.chat_picture_max_size_bytes))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_chat_picture_allowed");
    if (!util__json_add_owned_object(json_root, "is_chat_picture_allowed", cJSON_CreateBool(g_server_settings.is_chat_picture_allowed == TRUE)))
    {
        goto json_failure;
    }

    // write country blocking switch
    util__json_delete_item_secure(json_root, "is_country_blocking_active");
    if (!util__json_add_owned_object(json_root, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE)))
    {
        goto json_failure;
    }

    // write RSA key settings
    util__json_delete_item_secure(json_root, "minimum_rsa_key_bits");
    if (!cJSON_AddNumberToObject(json_root, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_minimum_rsa_key_bits_announced");
    if (!util__json_add_owned_object(json_root, "is_minimum_rsa_key_bits_announced", cJSON_CreateBool(g_server_settings.is_minimum_rsa_key_bits_announced == TRUE)))
    {
        goto json_failure;
    }

    // write the blocked countries
    util__json_delete_item_secure(json_root, "blocked_countries");
    json_blocked_countries = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "blocked_countries", json_blocked_countries))
    {
        goto json_failure;
    }
    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        if (!util__json_add_owned_array(json_blocked_countries, cJSON_CreateString(&g_server_settings.blocked_countries[i][0])))
        {
            goto json_failure;
        }
    }

    // only the log TOGGLES are persisted; the log lines themselves stay in ram, never in a file
    util__json_delete_item_secure(json_root, "is_logging_client_joins_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_client_joins_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_joins_enabled == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_logging_username_changes_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_username_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_username_changes_enabled == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_logging_group_changes_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_group_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_group_changes_enabled == TRUE)))
    {
        goto json_failure;
    }

    // write admin logging switches
    util__json_delete_item_secure(json_root, "is_logging_server_settings_updates_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_server_settings_updates_enabled", cJSON_CreateBool(g_server_settings.is_logging_server_settings_updates_enabled == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_logging_kicks_and_bans_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_kicks_and_bans_enabled", cJSON_CreateBool(g_server_settings.is_logging_kicks_and_bans_enabled == TRUE)))
    {
        goto json_failure;
    }

    // write connection log switches
    util__json_delete_item_secure(json_root, "is_logging_client_disconnects_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_client_disconnects_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_disconnects_enabled == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_logging_socket_opens_and_closes_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_socket_opens_and_closes_enabled", cJSON_CreateBool(g_server_settings.is_logging_socket_opens_and_closes_enabled == TRUE)))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_logging_failed_attempts_enabled");
    if (!util__json_add_owned_object(json_root, "is_logging_failed_attempts_enabled", cJSON_CreateBool(g_server_settings.is_logging_failed_attempts_enabled == TRUE)))
    {
        goto json_failure;
    }

    // write admin log limits
    util__json_delete_item_secure(json_root, "admin_log_max_size_bytes");
    if (!cJSON_AddNumberToObject(json_root, "admin_log_max_size_bytes", (double)g_server_settings.admin_log_max_size_bytes))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "admin_log_retention_days");
    if (!cJSON_AddNumberToObject(json_root, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days))
    {
        goto json_failure;
    }

    // write the admin password
    util__json_delete_item_secure(json_root, "admin_password");
    if (!cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "is_admin_password_initial");
    if (!util__json_add_owned_object(json_root, "is_admin_password_initial", cJSON_CreateBool(g_server_settings.is_admin_password_initial == TRUE)))
    {
        goto json_failure;
    }

    // write identity storage switch
    util__json_delete_item_secure(json_root, "is_identity_storage_enabled");
    if (!util__json_add_owned_object(json_root, "is_identity_storage_enabled", cJSON_CreateBool(g_server_settings.is_identity_storage_enabled == TRUE)))
    {
        goto json_failure;
    }

    // rebuild the channel layout (persistent fields only; runtime state like maintainer/occupants is skipped)
    util__json_delete_item_secure(json_root, "channels");
    json_channels = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "channels", json_channels))
    {
        goto json_failure;
    }
    for (i = 0; i < g_server_settings.max_channel_count; i++)
    {
        // skip empty slots
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

        // create the channel object
        json_channel = cJSON_CreateObject();
        if (!util__json_add_owned_array(json_channels, json_channel))
        {
            goto json_failure;
        }

        // add the channel groups
        if (!util__json_add_owned_object(json_channel, "channel_groups", channel_groups__write(channel_in_loop)))
        {
            goto json_failure;
        }

        // add ids and type
        if (!cJSON_AddNumberToObject(json_channel, "channel_id", channel_in_loop->channel_id))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(json_channel, "parent_channel_id", (int64)channel_in_loop->parent_channel_id))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(json_channel, "type", channel_in_loop->type))
        {
            goto json_failure;
        }

        // add the required powers
        if (!channel__write_required_powers(json_channel, channel_in_loop))
        {
            goto json_failure;
        }

        // add the channel switches
        if (!util__json_add_owned_object(json_channel, "is_root_channel", cJSON_CreateBool(channel_in_loop->is_root_channel == TRUE)))
        {
            goto json_failure;
        }
        if (!util__json_add_owned_object(json_channel, "is_using_password", cJSON_CreateBool(channel_in_loop->is_using_password == TRUE)))
        {
            goto json_failure;
        }
        if (!util__json_add_owned_object(json_channel, "is_audio_enabled", cJSON_CreateBool(channel_in_loop->is_audio_enabled == TRUE)))
        {
            goto json_failure;
        }
        if (!util__json_add_owned_object(json_channel, "is_video_stream_enabled", cJSON_CreateBool(channel_in_loop->is_video_stream_enabled == TRUE)))
        {
            goto json_failure;
        }
        if (!util__json_add_owned_object(json_channel, "is_client_limit_active", cJSON_CreateBool(channel_in_loop->is_client_limit_active == TRUE)))
        {
            goto json_failure;
        }

        // add the client limit
        if (!cJSON_AddNumberToObject(json_channel, "max_client_count", (double)channel_in_loop->max_client_count))
        {
            goto json_failure;
        }

        // add the channel icon
        if (!util__json_add_owned_object(json_channel, "has_channel_icon", cJSON_CreateBool(channel_in_loop->has_channel_icon == TRUE)))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(json_channel, "channel_icon_id", (double)channel_in_loop->icon_id))
        {
            goto json_failure;
        }

        // add name, password, description
        if (!cJSON_AddStringToObject(json_channel, "name", &channel_in_loop->name[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_channel, "password", &channel_in_loop->password[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_channel, "description", &channel_in_loop->description[0]))
        {
            goto json_failure;
        }
    }

    // write the emojis
    json_emojis = emojis__write();
    if (json_emojis == NULL_POINTER)
    {
        util__json_delete_secure(json_root);
        return FALSE;
    }
    util__json_delete_item_secure(json_root, "emojis");
    if (!cJSON_AddItemToObject(json_root, "emojis", json_emojis))
    {
        cJSON_Delete(json_emojis);
        util__json_delete_secure(json_root);
        return FALSE;
    }

    // rebuild icons (skip the admin icon id 0, which is re-seeded on every start)
    util__json_delete_item_secure(json_root, "icons");
    json_icons = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "icons", json_icons))
    {
        goto json_failure;
    }
    for (i = 0; i < MAX_ICONS; i++)
    {
        // skip empty, admin slots
        icon_in_loop = &g_icons_array[i];
        if (icon_in_loop->is_existing == FALSE || icon_in_loop->id == 0)
        {
            continue;
        }

        // create the icon object
        json_icon = cJSON_CreateObject();
        if (!util__json_add_owned_array(json_icons, json_icon))
        {
            goto json_failure;
        }

        // add id and image
        if (!cJSON_AddNumberToObject(json_icon, "id", icon_in_loop->id))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_icon, "base64", (icon_in_loop->base64 != NULL_POINTER) ? icon_in_loop->base64 : ""))
        {
            goto json_failure;
        }
    }

    // rebuild groups (skip the admin group id 0, re-seeded on every start)
    util__json_delete_item_secure(json_root, "groups");
    json_groups = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "groups", json_groups))
    {
        goto json_failure;
    }
    for (i = 0; i < MAX_GROUPS; i++)
    {
        // skip empty, admin slots
        group_in_loop = &g_groups_array[i];
        if (group_in_loop->is_existing == FALSE || group_in_loop->id == ADMIN_GROUP_ID)
        {
            continue;
        }

        // create the group object
        json_group = cJSON_CreateObject();
        if (!util__json_add_owned_array(json_groups, json_group))
        {
            goto json_failure;
        }

        // add id and icon
        if (!cJSON_AddNumberToObject(json_group, "id", group_in_loop->id))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(json_group, "icon_id", group_in_loop->icon_id))
        {
            goto json_failure;
        }
        if (!util__json_add_owned_object(json_group, "has_icon", cJSON_CreateBool(group_in_loop->has_icon == TRUE)))
        {
            goto json_failure;
        }

        // add name and type
        if (!cJSON_AddStringToObject(json_group, "name", &group_in_loop->name[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_group, "group_type", permissions__group_type_name(group_in_loop->group_type)))
        {
            goto json_failure;
        }
        if (!cJSON_AddBoolToObject(json_group, "is_default_guest", group_in_loop->is_default_guest))
        {
            goto json_failure;
        }

        // add the permissions
        if (!util__json_add_owned_object(json_group, "permissions", permissions__write(&group_in_loop->permissions)))
        {
            goto json_failure;
        }
    }

    // the admin group (id 0) itself is re-seeded on every start and skipped by the groups loop above, but its
    // icon IS runtime-editable, so persist just that link (icon id + has_icon) here and re-apply it after
    // the seed on load. without this the admin's chosen icon reverts to the default on every restart
    util__json_delete_item_secure(json_root, "admin_group_icon_id");
    if (!cJSON_AddNumberToObject(json_root, "admin_group_icon_id", (double)g_groups_array[ADMIN_GROUP_ID].icon_id))
    {
        goto json_failure;
    }
    util__json_delete_item_secure(json_root, "admin_group_has_icon");
    if (!util__json_add_owned_object(json_root, "admin_group_has_icon", cJSON_CreateBool(g_groups_array[ADMIN_GROUP_ID].has_icon == TRUE)))
    {
        goto json_failure;
    }

    // rebuild the ban list (matching is by ip at socket open and by identity at join; country/extra data are recorded for the admin)
    util__json_delete_item_secure(json_root, "bans");
    json_bans = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "bans", json_bans))
    {
        goto json_failure;
    }
    for (i = 0; i < MAX_BANS; i++)
    {
        // skip empty slots
        ban_in_loop = &g_ban_array[i];
        if (ban_in_loop->is_existing == FALSE)
        {
            continue;
        }

        // create the ban object
        json_ban = cJSON_CreateObject();
        if (!util__json_add_owned_array(json_bans, json_ban))
        {
            goto json_failure;
        }

        // add the ban fields
        if (!cJSON_AddStringToObject(json_ban, "ip_address", &ban_in_loop->ip_address[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_ban, "country_iso_code", &ban_in_loop->country_iso_code[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_ban, "identity", &ban_in_loop->identity[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddStringToObject(json_ban, "extra_data", &ban_in_loop->extra_data[0]))
        {
            goto json_failure;
        }
        if (!cJSON_AddNumberToObject(json_ban, "timestamp_banned", (double)ban_in_loop->timestamp_banned))
        {
            goto json_failure;
        }
    }

    // rebuild the identity store (public-key hash -> group ids); left untouched when identities are disabled
    if (!_settings_internal__serialize_identities(json_root))
    {
        goto json_failure;
    }

    // write music snapshot names
    util__json_delete_item_secure(json_root, "music_bot_snapshot");
    util__json_delete_item_secure(json_root, "music_bot_snapshot_previous");
    if (!cJSON_AddStringToObject(json_root, "music_bot_snapshot", g_server_settings.music_bot_snapshot) || !cJSON_AddStringToObject(json_root, "music_bot_snapshot_previous", g_server_settings.music_bot_snapshot_previous))
    {
        goto json_failure;
    }

    // write client page switches
    util__json_delete_item_secure(json_root, "is_client_http_enabled");
    util__json_delete_item_secure(json_root, "is_client_http_blank_when_disabled");
    util__json_delete_item_secure(json_root, "is_client_config_embedded");
    util__json_delete_item_secure(json_root, "is_start_page_connect_button_enabled");
    util__json_delete_item_secure(json_root, "is_start_page_import_identity_button_enabled");
    if (!cJSON_AddBoolToObject(json_root, "is_client_http_enabled", g_server_settings.is_client_http_enabled) || !cJSON_AddBoolToObject(json_root, "is_client_http_blank_when_disabled", g_server_settings.is_client_http_blank_when_disabled) || !cJSON_AddBoolToObject(json_root, "is_client_config_embedded", g_server_settings.is_client_config_embedded) || !cJSON_AddBoolToObject(json_root, "is_start_page_connect_button_enabled", g_server_settings.is_start_page_connect_button_enabled) || !cJSON_AddBoolToObject(json_root, "is_start_page_import_identity_button_enabled", g_server_settings.is_start_page_import_identity_button_enabled))
    {
        goto json_failure;
    }
    // the default themes of the served page can be changed in the server settings tab
    util__json_delete_item_secure(json_root, "default_theme");
    util__json_delete_item_secure(json_root, "default_mobile_theme");
    if (!cJSON_AddStringToObject(json_root, "default_theme", &g_server_settings.default_theme[0]) || !cJSON_AddStringToObject(json_root, "default_mobile_theme", &g_server_settings.default_mobile_theme[0]))
    {
        goto json_failure;
    }

    // write the encryption settings
    util__json_delete_item_secure(json_root, "dh_exchange_count");
    util__json_delete_item_secure(json_root, "session_encryption_layer_count");
    util__json_delete_item_secure(json_root, "session_cipher");
    util__json_delete_item_secure(json_root, "is_random_cipher_layer_enabled");
    if (!cJSON_AddNumberToObject(json_root, "dh_exchange_count", (double)g_server_settings.dh_exchange_count) || !cJSON_AddNumberToObject(json_root, "session_encryption_layer_count", (double)g_server_settings.session_encryption_layer_count) || !cJSON_AddStringToObject(json_root, "session_cipher", ciphers__name(g_server_settings.session_cipher)) || !cJSON_AddBoolToObject(json_root, "is_random_cipher_layer_enabled", g_server_settings.is_random_cipher_layer_enabled))
    {
        goto json_failure;
    }

    // write the connection keys
    util__json_delete_item_secure(json_root, "keys");
    connection_passwords = cJSON_AddArrayToObject(json_root, "keys");
    if (connection_passwords == NULL_POINTER)
    {
        goto json_failure;
    }
    for (key_index = 0; key_index < g_server_settings.keys_count; key_index++)
    {
        entry = cJSON_CreateObject();
        if (entry == NULL_POINTER || !cJSON_AddStringToObject(entry, "type", ciphers__name(g_server_settings.keys[key_index].type)) || !cJSON_AddStringToObject(entry, "password", g_server_settings.plaintext_keys[key_index]) || !cJSON_AddItemToArray(connection_passwords, entry))
        {
            util__json_delete_secure(entry);
            goto json_failure;
        }
    }

    // convert JSON to text
    json_text = cJSON_Print(json_root);
    if (json_text == NULL_POINTER)
    {
        util__json_delete_secure(json_root);
        return FALSE;
    }

    // write the settings file
    write_succeeded = settings__write_file_atomically("server_settings.json", json_text);
    if (write_succeeded == TRUE)
    {
        log_info("%s", "server settings saved to server_settings.json (general settings, channels, groups)");
    }
    else
    {
        log_info("%s", "save_server_settings: could not write server_settings.json");
    }

    // free text and JSON
    clib__null_memory(json_text, strlen(json_text));
    cJSON_free(json_text);
    util__json_delete_secure(json_root);

    return write_succeeded;

json_failure:
    // free the JSON
    util__json_delete_secure(json_root);
    return FALSE;
}

/**
 * @brief Saves setup settings and plaintext connection keys to server_settings.json. A failed file
 *        write is reported on stdout; setup continues.
 *
 * @param char plaintext_keys[][256] -> connection passwords as entered, one per row; the file keeps their text and type so startup can derive the same keys
 * @param uint64 keys_count -> number of valid entries in plaintext_keys
 *
 * @return void
 */
void settings__save_initial(char plaintext_keys[][256], uint64 keys_count)
{
    cJSON *entry = NULL_POINTER;
    cJSON *json_root = NULL_POINTER;
    cJSON *json_keys = NULL_POINTER;
    char *json_text = NULL_POINTER;
    uint64 i = 0;

    // create the JSON root
    json_root = cJSON_CreateObject();
    if (json_root == NULL_POINTER)
    {
        return;
    }

    // add the websocket port
    if (!cJSON_AddNumberToObject(json_root, "websocket_port", g_server_settings.websocket_port))
    {
        goto json_failure;
    }

    // add the wireguard fields
    if (!util__json_add_owned_object(json_root, "is_wireguard_mode_active", cJSON_CreateBool(g_server_settings.is_wireguard_mode_active == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddStringToObject(json_root, "webrtc_public_address", g_server_settings.webrtc_public_address))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "webrtc_port_range_begin", (double)g_server_settings.webrtc_port_range_begin))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "webrtc_port_range_end", (double)g_server_settings.webrtc_port_range_end))
    {
        goto json_failure;
    }

    // add audio transport switches
    if (!util__json_add_owned_object(json_root, "is_audio_over_websocket_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_websocket_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_audio_over_webrtc_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_webrtc_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add slot, channel limits
    if (!cJSON_AddNumberToObject(json_root, "server_slots", (double)g_server_settings.max_client_count))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "max_channel_count", (double)g_server_settings.channel_count_limit))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "reserved_slots", (double)g_server_settings.reserved_slots))
    {
        goto json_failure;
    }

    // add the admin password
    if (!cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_admin_password_initial", cJSON_CreateBool(g_server_settings.is_admin_password_initial == TRUE)))
    {
        goto json_failure;
    }

    // create the keys array
    json_keys = cJSON_CreateArray();
    if (!util__json_add_owned_object(json_root, "keys", json_keys))
    {
        goto json_failure;
    }

    // add each connection key
    for (i = 0; i < keys_count; i++)
    {
        entry = cJSON_CreateObject();
        if (entry == NULL_POINTER || cJSON_AddStringToObject(entry, "type", ciphers__name(g_server_settings.keys[i].type)) == NULL_POINTER || cJSON_AddStringToObject(entry, "password", &plaintext_keys[i][0]) == NULL_POINTER)
        {
            cJSON_Delete(entry);
            goto json_failure;
        }
        if (!util__json_add_owned_array(json_keys, entry))
        {
            goto json_failure;
        }
    }

    // add voice, music switches
    if (!util__json_add_owned_object(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE)))
    {
        goto json_failure;
    }

    // never a wizard question: off until the admin switches it on in the server settings tab
    if (!util__json_add_owned_object(json_root, "is_video_streaming_active", cJSON_CreateBool(g_server_settings.is_video_streaming_active == TRUE)))
    {
        goto json_failure;
    }

    // add same IP switch
    if (!util__json_add_owned_object(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add RSA key settings
    if (!cJSON_AddNumberToObject(json_root, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_minimum_rsa_key_bits_announced", cJSON_CreateBool(g_server_settings.is_minimum_rsa_key_bits_announced == TRUE)))
    {
        goto json_failure;
    }

    // add country flag switch
    if (!util__json_add_owned_object(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE)))
    {
        goto json_failure;
    }

    // add the channel switches
    if (!util__json_add_owned_object(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add idle mode switch
    if (!util__json_add_owned_object(json_root, "is_idle_mode_allowed", cJSON_CreateBool(g_server_settings.is_idle_mode_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add admin flag switch
    if (!util__json_add_owned_object(json_root, "is_admin_country_flag_hidden", cJSON_CreateBool(g_server_settings.is_admin_country_flag_hidden == TRUE)))
    {
        goto json_failure;
    }

    // add reconnect, takeover switches
    if (!util__json_add_owned_object(json_root, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add websocket ping switch
    if (!util__json_add_owned_object(json_root, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE)))
    {
        goto json_failure;
    }

    // add key reuse switches
    if (!cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed))
    {
        goto json_failure;
    }

    // add key reuse limits
    if (!cJSON_AddNumberToObject(json_root, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients))
    {
        goto json_failure;
    }

    // add permission, connection switches
    if (!cJSON_AddBoolToObject(json_root, "is_permission_system_enabled", g_server_settings.is_permission_system_enabled == TRUE))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced == TRUE))
    {
        goto json_failure;
    }

    // add the login limit
    if (!cJSON_AddNumberToObject(json_root, "max_concurrent_logins", (double)g_server_settings.max_concurrent_logins))
    {
        goto json_failure;
    }

    // add attachment blocking switches
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_pictures_active", g_server_settings.is_blocking_direct_chat_pictures_active))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_files_active", g_server_settings.is_blocking_direct_chat_files_active))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_pictures_active", g_server_settings.is_blocking_channel_chat_pictures_active))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_files_active", g_server_settings.is_blocking_channel_chat_files_active))
    {
        goto json_failure;
    }

    // add the datachannel cooldown
    if (!cJSON_AddNumberToObject(json_root, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds))
    {
        goto json_failure;
    }

    // add marquee, restart switches
    if (!util__json_add_owned_object(json_root, "is_music_bot_marquee_visible_to_everyone", cJSON_CreateBool(g_server_settings.is_music_bot_marquee_visible_to_everyone == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_restart_on_crash_enabled", cJSON_CreateBool(g_server_settings.is_restart_on_crash_enabled == TRUE)))
    {
        goto json_failure;
    }

    // add stunnel, WSS, Tailscale
    if (!util__json_add_owned_object(json_root, "is_stunnel_enabled", cJSON_CreateBool(g_server_settings.is_stunnel_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_external_wss_enabled", g_server_settings.is_external_wss_enabled == TRUE))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_tailscale_funnel_enabled", g_server_settings.is_tailscale_funnel_enabled == TRUE))
    {
        goto json_failure;
    }

    // add the WSS port
    if (!cJSON_AddNumberToObject(json_root, "wss_port", g_server_settings.wss_port))
    {
        goto json_failure;
    }

    // add domain and certificate
    if (!cJSON_AddStringToObject(json_root, "stunnel_domain", &g_server_settings.stunnel_domain[0]))
    {
        goto json_failure;
    }
    if (!cJSON_AddStringToObject(json_root, "stunnel_cert_fullchain", &g_server_settings.stunnel_cert_fullchain[0]))
    {
        goto json_failure;
    }
    if (!cJSON_AddStringToObject(json_root, "stunnel_cert_privkey", &g_server_settings.stunnel_cert_privkey[0]))
    {
        goto json_failure;
    }

    // add client HTML destination
    if (!cJSON_AddStringToObject(json_root, "client_html_dest", &g_server_settings.client_html_dest[0]))
    {
        goto json_failure;
    }

    // add the HTTP settings
    if (!util__json_add_owned_object(json_root, "is_client_http_enabled", cJSON_CreateBool(g_server_settings.is_client_http_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "http_port", g_server_settings.http_port))
    {
        goto json_failure;
    }
    if (!cJSON_AddStringToObject(json_root, "http_webroot", &g_server_settings.http_webroot[0]))
    {
        goto json_failure;
    }

    // add the HTTPS settings
    if (!util__json_add_owned_object(json_root, "is_https_enabled", cJSON_CreateBool(g_server_settings.is_https_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "https_port", g_server_settings.https_port))
    {
        goto json_failure;
    }

    // add the default themes
    if (!cJSON_AddStringToObject(json_root, "default_theme", &g_server_settings.default_theme[0]))
    {
        goto json_failure;
    }
    if (!cJSON_AddStringToObject(json_root, "default_mobile_theme", &g_server_settings.default_mobile_theme[0]))
    {
        goto json_failure;
    }

    // add client page switches
    if (!util__json_add_owned_object(json_root, "is_client_config_embedded", cJSON_CreateBool(g_server_settings.is_client_config_embedded == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_start_page_connect_button_enabled", cJSON_CreateBool(g_server_settings.is_start_page_connect_button_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_start_page_import_identity_button_enabled", cJSON_CreateBool(g_server_settings.is_start_page_import_identity_button_enabled == TRUE)))
    {
        goto json_failure;
    }

    // add identity storage switches
    if (!util__json_add_owned_object(json_root, "is_identity_localstorage_enabled", cJSON_CreateBool(g_server_settings.is_identity_localstorage_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_identity_storage_enabled", cJSON_CreateBool(g_server_settings.is_identity_storage_enabled == TRUE)))
    {
        goto json_failure;
    }

    // add the avatar settings
    if (!util__json_add_owned_object(json_root, "is_avatar_upload_allowed", cJSON_CreateBool(g_server_settings.is_avatar_upload_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "avatar_max_size_bytes", (double)g_server_settings.avatar_max_size_bytes))
    {
        goto json_failure;
    }

    // add the icon size
    if (!cJSON_AddNumberToObject(json_root, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes))
    {
        goto json_failure;
    }

    // add stored client switches
    if (!util__json_add_owned_object(json_root, "is_stored_client_list_allowed", cJSON_CreateBool(g_server_settings.is_stored_client_list_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_last_seen_enabled", cJSON_CreateBool(g_server_settings.is_last_seen_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_offline_messaging_allowed", cJSON_CreateBool(g_server_settings.is_offline_messaging_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add client feature switches
    if (!util__json_add_owned_object(json_root, "is_typing_indicator_allowed", cJSON_CreateBool(g_server_settings.is_typing_indicator_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_client_rename_allowed", cJSON_CreateBool(g_server_settings.is_client_rename_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddBoolToObject(json_root, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed))
    {
        goto json_failure;
    }

    // add the messaging switches
    if (!util__json_add_owned_object(json_root, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_private_messaging_allowed", cJSON_CreateBool(g_server_settings.is_private_messaging_allowed == TRUE)))
    {
        goto json_failure;
    }

    // add file upload settings
    if (!util__json_add_owned_object(json_root, "is_file_upload_allowed", cJSON_CreateBool(g_server_settings.is_file_upload_allowed == TRUE)))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "file_upload_max_size_bytes", (double)g_server_settings.file_upload_max_size_bytes))
    {
        goto json_failure;
    }

    // add the picture size
    if (!cJSON_AddNumberToObject(json_root, "chat_picture_max_size_bytes", (double)g_server_settings.chat_picture_max_size_bytes))
    {
        goto json_failure;
    }

    // pictures are assumed wanted: on by default and never a setup question, only a settings-tab toggle
    if (!util__json_add_owned_object(json_root, "is_chat_picture_allowed", cJSON_CreateBool(g_server_settings.is_chat_picture_allowed == TRUE)))
    {
        goto json_failure;
    }

    // country blocking is deliberately NOT a setup question; the admin sets it later in the settings tab
    if (!util__json_add_owned_object(json_root, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "blocked_countries", cJSON_CreateArray()))
    {
        goto json_failure;
    }

    // the admin log toggles are not setup questions either; all off until the admin turns them on
    if (!util__json_add_owned_object(json_root, "is_logging_client_joins_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_joins_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_logging_username_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_username_changes_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_logging_group_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_group_changes_enabled == TRUE)))
    {
        goto json_failure;
    }

    // add admin logging switches
    if (!util__json_add_owned_object(json_root, "is_logging_server_settings_updates_enabled", cJSON_CreateBool(g_server_settings.is_logging_server_settings_updates_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_logging_kicks_and_bans_enabled", cJSON_CreateBool(g_server_settings.is_logging_kicks_and_bans_enabled == TRUE)))
    {
        goto json_failure;
    }

    // add connection log switches
    if (!util__json_add_owned_object(json_root, "is_logging_client_disconnects_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_disconnects_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_logging_socket_opens_and_closes_enabled", cJSON_CreateBool(g_server_settings.is_logging_socket_opens_and_closes_enabled == TRUE)))
    {
        goto json_failure;
    }
    if (!util__json_add_owned_object(json_root, "is_logging_failed_attempts_enabled", cJSON_CreateBool(g_server_settings.is_logging_failed_attempts_enabled == TRUE)))
    {
        goto json_failure;
    }

    // add admin log limits
    if (!cJSON_AddNumberToObject(json_root, "admin_log_max_size_bytes", (double)g_server_settings.admin_log_max_size_bytes))
    {
        goto json_failure;
    }
    if (!cJSON_AddNumberToObject(json_root, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days))
    {
        goto json_failure;
    }

    // write music snapshot names
    util__json_delete_item_secure(json_root, "music_bot_snapshot");
    util__json_delete_item_secure(json_root, "music_bot_snapshot_previous");
    if (!cJSON_AddStringToObject(json_root, "music_bot_snapshot", g_server_settings.music_bot_snapshot) || !cJSON_AddStringToObject(json_root, "music_bot_snapshot_previous", g_server_settings.music_bot_snapshot_previous))
    {
        goto json_failure;
    }

    // write client page switches
    util__json_delete_item_secure(json_root, "is_client_http_enabled");
    util__json_delete_item_secure(json_root, "is_client_http_blank_when_disabled");
    util__json_delete_item_secure(json_root, "is_client_config_embedded");
    util__json_delete_item_secure(json_root, "is_start_page_connect_button_enabled");
    util__json_delete_item_secure(json_root, "is_start_page_import_identity_button_enabled");
    if (!cJSON_AddBoolToObject(json_root, "is_client_http_enabled", g_server_settings.is_client_http_enabled) || !cJSON_AddBoolToObject(json_root, "is_client_http_blank_when_disabled", g_server_settings.is_client_http_blank_when_disabled) || !cJSON_AddBoolToObject(json_root, "is_client_config_embedded", g_server_settings.is_client_config_embedded) || !cJSON_AddBoolToObject(json_root, "is_start_page_connect_button_enabled", g_server_settings.is_start_page_connect_button_enabled) || !cJSON_AddBoolToObject(json_root, "is_start_page_import_identity_button_enabled", g_server_settings.is_start_page_import_identity_button_enabled))
    {
        goto json_failure;
    }

    // write the encryption settings
    util__json_delete_item_secure(json_root, "dh_exchange_count");
    util__json_delete_item_secure(json_root, "session_encryption_layer_count");
    util__json_delete_item_secure(json_root, "session_cipher");
    util__json_delete_item_secure(json_root, "is_random_cipher_layer_enabled");
    if (!cJSON_AddNumberToObject(json_root, "dh_exchange_count", (double)g_server_settings.dh_exchange_count) || !cJSON_AddNumberToObject(json_root, "session_encryption_layer_count", (double)g_server_settings.session_encryption_layer_count) || !cJSON_AddStringToObject(json_root, "session_cipher", ciphers__name(g_server_settings.session_cipher)) || !cJSON_AddBoolToObject(json_root, "is_random_cipher_layer_enabled", g_server_settings.is_random_cipher_layer_enabled))
    {
        goto json_failure;
    }

    // write the settings file
    json_text = cJSON_Print(json_root);
    if (json_text != NULL_POINTER)
    {
        if (settings__write_file_atomically("server_settings.json", json_text) == TRUE)
        {
            printf("%s %s\n", g_mark_ok, "settings saved to server_settings.json (next start skips these prompts)");
        }
        else
        {
            printf("%s %s\n", g_mark_warn, "could not write server_settings.json (settings not persisted)");
        }
        clib__null_memory(json_text, strlen(json_text));
        cJSON_free(json_text);
    }

json_failure:
    // free the JSON
    util__json_delete_secure(json_root);
}

typedef struct saved_channel_graph_node_t
{
    int64 id;
    int64 parent_id;
    uint64 parent_index;
    ubyte state;
} saved_channel_graph_node_t;

// qsort requires an int return value for its comparison callback.
static int _settings_internal__compare_channel_ids(const void *left, const void *right)
{
    const saved_channel_graph_node_t *a = left;
    const saved_channel_graph_node_t *b = right;

    return (a->id > b->id) - (a->id < b->id);
}

/**
 * @brief Rejects duplicate slots, missing parents and cycles before restoring any saved channel.
 * @details Memory depends on saved entries, not configured capacity. Sorting and parent lookup take O(n log n) work; iterative cycle detection visits each node at most twice.
 */
static boole _settings_internal__validate_channel_graph(cJSON *channels)
{
    saved_channel_graph_node_t *node = NULL_POINTER;
    uint64 low = 0;
    uint64 high = 0;
    uint64 middle = 0;
    uint64 current = 0;
    cJSON *entry = NULL_POINTER;
    saved_channel_graph_node_t *nodes = NULL_POINTER;
    uint64 capacity = atomic_load(&g_server_settings.max_channel_count);
    uint64 count = 0;
    uint64 index = 0;
    boole valid = FALSE;

    // no channels saved
    if (channels == NULL_POINTER)
    {
        return TRUE;
    }

    // check array and capacity
    if (!cJSON_IsArray(channels) || capacity < 1 || capacity > MAX_CHANNEL_COUNT)
    {
        return FALSE;
    }

    // count the entries
    cJSON_ArrayForEach(entry, channels)
    {
        if (count >= capacity || count >= SIZE_MAX / sizeof(*nodes))
        {
            return FALSE;
        }
        count++;
    }

    // no entries to check
    if (count == 0)
    {
        return TRUE;
    }

    // create the node array
    nodes = calloc(count, sizeof(*nodes));
    if (nodes == NULL_POINTER)
    {
        return FALSE;
    }

    // read ids and parents
    cJSON_ArrayForEach(entry, channels)
    {
        node = &nodes[index++];
        if (!cJSON_IsObject(entry) || !util__json_read_int64(cJSON_GetObjectItemCaseSensitive(entry, "channel_id"), 0, (int64)capacity - 1, &node->id) || !util__json_read_int64(cJSON_GetObjectItemCaseSensitive(entry, "parent_channel_id"), -1, (int64)capacity - 1, &node->parent_id) || ((node->id == ROOT_CHANNEL_ID) != (node->parent_id == -1)))
        {
            goto done;
        }
    }

    // sort nodes by id
    clib__qsort(nodes, count, sizeof(*nodes), _settings_internal__compare_channel_ids);

    // link nodes to parents
    for (index = 0; index < count; index++)
    {
        // refuse duplicate ids
        if (index > 0 && nodes[index - 1].id == nodes[index].id)
        {
            goto done;
        }

        // skip nodes without parent
        nodes[index].parent_index = count;
        if (nodes[index].parent_id == -1)
        {
            continue;
        }

        // search for the parent
        low = 0;
        high = count;
        while (low < high)
        {
            middle = low + (high - low) / 2;
            if (nodes[middle].id < nodes[index].parent_id)
            {
                low = middle + 1;
            }
            else
            {
                high = middle;
            }
        }

        // refuse a missing parent
        if (low == count || nodes[low].id != nodes[index].parent_id)
        {
            goto done;
        }

        // store the parent index
        nodes[index].parent_index = low;
    }

    // search for cycles
    for (index = 0; index < count; index++)
    {
        // mark nodes on path
        current = index;
        while (current < count && nodes[current].state == 0)
        {
            nodes[current].state = 1;
            current = nodes[current].parent_index;
        }

        // refuse a cycle
        if (current < count && nodes[current].state == 1)
        {
            goto done;
        }

        // mark path as checked
        current = index;
        while (current < count && nodes[current].state == 1)
        {
            nodes[current].state = 2;
            current = nodes[current].parent_index;
        }
    }

    // channel layout is valid
    valid = TRUE;
done:
    // free the node array
    free(nodes);
    return valid;
}

// Validate first, then apply to a settings snapshot under the clients write lock.
// A missing password field keeps the list; an explicit empty list removes it.
boole settings__read_connection_encryption(cJSON *message, server_settings_t *destination)
{
    cJSON *type = NULL_POINTER;
    cJSON *entry_password = NULL_POINTER;
    cJSON *password = NULL_POINTER;
    cJSON *exchanges = cJSON_GetObjectItemCaseSensitive(message, "dh_exchange_count");
    cJSON *layers = cJSON_GetObjectItemCaseSensitive(message, "session_encryption_layer_count");
    cJSON *cipher = cJSON_GetObjectItemCaseSensitive(message, "session_cipher");
    cJSON *random = cJSON_GetObjectItemCaseSensitive(message, "is_random_cipher_layer_enabled");
    cJSON *passwords = cJSON_GetObjectItemCaseSensitive(message, "connection_passwords");
    cJSON *entry;
    int64 number;
    crypto_key_t keys[100] = { 0 };
    uint64 count = 0;
    boole success = FALSE;

    // check all fields
    if ((exchanges != NULL_POINTER && !util__json_read_int64(exchanges, 1, LEMON_MAX_DH_EXCHANGES, &number)) || (layers != NULL_POINTER && !util__json_read_int64(layers, 1, LEMON_MAX_SESSION_LAYERS, &number)) || (cipher != NULL_POINTER && (!cJSON_IsString(cipher) || ciphers__type(cipher->valuestring) == 0)) || (random != NULL_POINTER && !cJSON_IsBool(random)) || (passwords != NULL_POINTER && (!cJSON_IsArray(passwords) || cJSON_GetArraySize(passwords) > 100)))
    {
        goto done;
    }

    // create the new keys
    cJSON_ArrayForEach(entry, passwords)
    {
        type = cJSON_GetObjectItemCaseSensitive(entry, "type");
        entry_password = cJSON_GetObjectItemCaseSensitive(entry, "password");
        if (!cJSON_IsObject(entry) || !cJSON_IsString(type) || ciphers__type(type->valuestring) == 0 || !cJSON_IsString(entry_password) || strlen(entry_password->valuestring) >= 256 || !crypto_metadata__key_from_password(&keys[count], ciphers__type(type->valuestring), entry_password->valuestring))
        {
            goto done;
        }
        count++;
    }

    // store values in destination
    if (destination != NULL_POINTER)
    {
        // store exchange, layer counts
        if (exchanges != NULL_POINTER)
        {
            destination->dh_exchange_count = (uint64)exchanges->valuedouble;
        }
        if (layers != NULL_POINTER)
        {
            destination->session_encryption_layer_count = (uint64)layers->valuedouble;
        }

        // store the cipher settings
        if (cipher != NULL_POINTER)
        {
            destination->session_cipher = ciphers__type(cipher->valuestring);
        }
        if (random != NULL_POINTER)
        {
            destination->is_random_cipher_layer_enabled = cJSON_IsTrue(random);
        }

        // replace the connection keys
        if (passwords != NULL_POINTER)
        {
            ciphers__clear(destination->keys, sizeof(destination->keys));
            ciphers__clear(destination->plaintext_keys, sizeof(destination->plaintext_keys));
            memcpy(destination->keys, keys, count * sizeof(crypto_key_t));
            destination->keys_count = count;
            count = 0;
            cJSON_ArrayForEach(entry, passwords)
            {
                password = cJSON_GetObjectItemCaseSensitive(entry, "password");
                memcpy(destination->plaintext_keys[count++], password->valuestring, strlen(password->valuestring));
            }
        }
    }

    // all fields are valid
    success = TRUE;
done:
    // clear the local keys
    ciphers__clear(keys, sizeof(keys));
    return success;
}

void settings__refresh_client_config(void)
{
    _settings_internal__build_and_push_client_config(g_server_settings.websocket_port, g_server_settings.plaintext_keys, g_server_settings.keys_count);
}
