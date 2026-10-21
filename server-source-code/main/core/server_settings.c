#include "identity_store.h"
#include "administration.h"
#include "../definitions.h"
#include "../metadata_crypto.h"

#include "../clib/clib_string.h"
#include "../clib/clib_memory.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../base.h"
#include "../../third-party/rxi-log/log.h"
#include "../httpserver/http_server.h"
#include "../util.h"
#include "../first_time_setup.h"
#include "server_settings.h"
#include "../memory_manager.h"
#include "../audio_websocket.h"
#include <string.h>
#ifdef WIN32
#include <windows.h>
#include <io.h>
#endif

// declarations
static void _settings_internal__build_and_push_client_config(int64 websocket_port, char plaintext_keys[][256], uint64 keys_count);
static void _settings_internal__serialize_identities(cJSON* json_root);

/**
 * @brief Reads which attachment types clients may choose to block.
 * @param cJSON* message -> saved settings or an admin request, with optional boolean fields
 * @return void
 * @attention Runtime callers hold the clients write lock.
 */
void settings__read_attachment_receive_policy(cJSON* message)
{
    cJSON* field = NULL_POINTER;

    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_direct_chat_pictures_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_direct_chat_pictures_active = cJSON_IsTrue(field);
    }

    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_direct_chat_files_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_direct_chat_files_active = cJSON_IsTrue(field);
    }

    field = cJSON_GetObjectItemCaseSensitive(message, "is_blocking_channel_chat_pictures_active");
    if (cJSON_IsBool(field))
    {
        g_server_settings.is_blocking_channel_chat_pictures_active = cJSON_IsTrue(field);
    }

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

    channel_t* root_channel = &g_channel_array[0];

    root_channel->channel_id = 0;
    root_channel->parent_channel_id = -1;
    root_channel->is_root_channel = TRUE;
    root_channel->is_existing = TRUE;
    root_channel->is_audio_enabled = TRUE;
    root_channel->is_video_stream_enabled = TRUE;

    clib__copy_memory((void*)&channel_name, (void*)&root_channel->name, strlen(channel_name), CHANNEL_NAME_MAX_LENGTH);
    clib__copy_memory((void*)&description, (void*)&root_channel->description, strlen(description), CHANNEL_DESCRIPTION_MAX_LENGTH);
    root_channel->type = 1;
    root_channel->maintainer_id = -1;
    root_channel->maintainer_generation = 0;
}

/**
 * @brief rebuilds the saved channels, icons, tags, bans and identities from server_settings.json into the
 *        already allocated global arrays. a missing or unparseable file is a no-op.
 *
 *        each channel/icon/tag is written into the slot matching its saved id, so id references stay valid
 *        (ids are array indices, not opaque handles). runtime-only fields are reset to their empty defaults.
 *        the channel root and the admin tag/icon (id 0) are seeded separately before this runs and are left
 *        in place, so this only fills id >= 1 for tags and icons; the admin tag's saved icon link is
 *        re-applied at the end, once the icons it may point at are loaded. bans and identities are filled
 *        sequentially instead, and an identity with no tags, no avatar and no alias is dropped rather than
 *        occupying a slot.
 *
 * @note must run after the arrays are allocated and before any client can connect.
 *
 * @return void
 */
void settings__load_persisted_state(void)
{
    FILE* settings_file = NULL_POINTER;
    int64 file_length = 0;
    char* file_buffer = NULL_POINTER;
    uint64 bytes_read = 0;
    cJSON* json_root = NULL_POINTER;
    cJSON* json_channels = NULL_POINTER;
    cJSON* json_channel = NULL_POINTER;
    cJSON* json_icons = NULL_POINTER;
    cJSON* json_icon = NULL_POINTER;
    cJSON* json_tags = NULL_POINTER;
    cJSON* json_tag = NULL_POINTER;
    cJSON* json_bans = NULL_POINTER;
    cJSON* json_ban = NULL_POINTER;
    cJSON* json_identities = NULL_POINTER;
    cJSON* json_identity = NULL_POINTER;
    cJSON* json_identity_tag_ids = NULL_POINTER;
    cJSON* json_identity_tag_id = NULL_POINTER;
    cJSON* json_field = NULL_POINTER;
    channel_t* channel_in_loop = NULL_POINTER;
    icon_t* icon_in_loop = NULL_POINTER;
    tag_t* tag_in_loop = NULL_POINTER;
    ban_entry_t* ban_in_loop = NULL_POINTER;
    client_stored_data_t* identity_in_loop = NULL_POINTER;
    int64 channel_id = 0;
    int64 icon_id = 0;
    int64 tag_id = 0;
    uint64 loaded_channels = 0;
    uint64 loaded_icons = 0;
    uint64 loaded_tags = 0;
    uint64 loaded_bans = 0;
    uint64 loaded_identities = 0;
    uint64 ban_slot = 0;
    uint64 identity_slot = 0;

    settings_file = fopen("server_settings.json", "rb");
    if (settings_file == NULL_POINTER)
    {
        DBG_SETTINGS log_info("%s", "persisted state: server_settings.json not found, nothing to restore");
        return;
    }

    fseek(settings_file, 0, SEEK_END);
    file_length = ftell(settings_file);
    fseek(settings_file, 0, SEEK_SET);
    DBG_SETTINGS log_info("%s %lld %s", "persisted state: reading", (long long)file_length, "bytes of server_settings.json");
    if (file_length > 0)
    {
        file_buffer = (char*)malloc(file_length + 1);
        if (file_buffer != NULL_POINTER)
        {
            bytes_read = fread(file_buffer, 1, file_length, settings_file);
            file_buffer[bytes_read] = 0;
        }
    }
    fclose(settings_file);

    if (file_buffer == NULL_POINTER)
    {
        return;
    }

    json_root = cJSON_Parse(file_buffer);
    free(file_buffer);

    if (json_root == NULL_POINTER)
    {
        return;
    }

    // channels
    json_channels = cJSON_GetObjectItemCaseSensitive(json_root, "channels");
    if (cJSON_IsArray(json_channels) == TRUE)
    {
        cJSON_ArrayForEach(json_channel, json_channels)
        {
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "channel_id");
            if (cJSON_IsNumber(json_field) == FALSE)
            {
                continue;
            }

            channel_id = json_field->valueint;
            if (channel_id < 0 || channel_id >= (int64)g_server_settings.max_channel_count)
            {
                continue;
            }

            // write into the exact saved slot, never a first-free slot, so channel_id stays stable and
            // parent_channel_id references remain valid across the restart
            channel_in_loop = &g_channel_array[channel_id];
            clib__null_memory(channel_in_loop, sizeof(channel_t));

            channel_in_loop->channel_id = channel_id;
            channel_in_loop->is_existing = TRUE;
            channel_in_loop->maintainer_id = -1;
            channel_in_loop->is_channel_maintainer_present = FALSE;
            channel_in_loop->maintainer_generation = 0;
            channel_in_loop->is_music_bot_active_in_channel = FALSE;
            channel_in_loop->current_clients = 0;
            channel_in_loop->parent_channel_id = -1;

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "parent_channel_id");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                channel_in_loop->parent_channel_id = (int64)json_field->valuedouble;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "type");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                channel_in_loop->type = json_field->valueint;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_root_channel");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_root_channel = cJSON_IsTrue(json_field);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_using_password");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_using_password = cJSON_IsTrue(json_field);
            }

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

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_client_limit_active");
            if (cJSON_IsBool(json_field))
            {
                channel_in_loop->is_client_limit_active = cJSON_IsTrue(json_field);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "max_client_count");
            if (cJSON_IsNumber(json_field))
            {
                channel_in_loop->max_client_count = (uint64)json_field->valuedouble;
            }

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
                channel_in_loop->icon_id = (uint64)json_field->valuedouble;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__copy_memory(json_field->valuestring, &channel_in_loop->name[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_NAME_MAX_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "password");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__copy_memory(json_field->valuestring, &channel_in_loop->password[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_PASSWORD_MAX_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "description");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__copy_memory(json_field->valuestring, &channel_in_loop->description[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH - 1);
            }

            loaded_channels++;
        }
    }

    // icons (id is the array index; the admin icon id 0 is seeded separately, so skip it)
    json_icons = cJSON_GetObjectItemCaseSensitive(json_root, "icons");
    if (cJSON_IsArray(json_icons) == TRUE)
    {
        cJSON_ArrayForEach(json_icon, json_icons)
        {
            json_field = cJSON_GetObjectItemCaseSensitive(json_icon, "id");
            if (cJSON_IsNumber(json_field) == FALSE)
            {
                continue;
            }

            icon_id = json_field->valueint;
            if (icon_id <= 0 || icon_id >= MAX_ICONS)
            {
                continue;
            }

            icon_in_loop = &g_icons_array[icon_id];
            server_admin__free_icon_base64(icon_in_loop);
            clib__null_memory(icon_in_loop, sizeof(icon_t));
            icon_in_loop->id = icon_id;
            icon_in_loop->is_existing = TRUE;

            json_field = cJSON_GetObjectItemCaseSensitive(json_icon, "base64");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                server_admin__set_icon_base64(icon_in_loop, json_field->valuestring);
            }

            loaded_icons++;
        }
    }

    // tags (id is the array index; the admin tag id 0 is seeded separately, so skip it)
    json_tags = cJSON_GetObjectItemCaseSensitive(json_root, "tags");
    if (cJSON_IsArray(json_tags) == TRUE)
    {
        cJSON_ArrayForEach(json_tag, json_tags)
        {
            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "id");
            if (cJSON_IsNumber(json_field) == FALSE)
            {
                continue;
            }

            tag_id = json_field->valueint;
            if (tag_id <= 0 || tag_id >= MAX_TAGS)
            {
                continue;
            }

            tag_in_loop = &g_tags_array[tag_id];
            clib__null_memory(tag_in_loop, sizeof(tag_t));
            tag_in_loop->id = tag_id;
            tag_in_loop->is_existing = TRUE;

            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "icon_id");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                tag_in_loop->icon_id = (uint64)json_field->valueint;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "has_icon");
            if (cJSON_IsBool(json_field) == TRUE)
            {
                tag_in_loop->has_icon = cJSON_IsTrue(json_field);
            }
            // settings written before has_icon existed had a mandatory icon, but icon id 0 is the seeded
            // admin icon - defaulting those to TRUE hands every iconless tag the admin shield
            else
            {
                tag_in_loop->has_icon = (boole)(tag_in_loop->icon_id != 0);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
            {
                clib__copy_memory(json_field->valuestring, &tag_in_loop->name[0], clib__utf8_string_length(json_field->valuestring), TAG_MAX_NAME_LENGTH - 1);
            }

            loaded_tags++;
        }
    }

    // load the ban list (filled sequentially; ip is required, the rest is optional metadata)
    json_bans = cJSON_GetObjectItemCaseSensitive(json_root, "bans");
    if (cJSON_IsArray(json_bans) == TRUE)
    {
        cJSON_ArrayForEach(json_ban, json_bans)
        {
            if (ban_slot >= MAX_BANS)
            {
                break;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "ip_address");
            if (cJSON_IsString(json_field) == FALSE || json_field->valuestring == NULL_POINTER)
            {
                continue;
            }

            ban_in_loop = &g_ban_array[ban_slot];
            clib__null_memory(ban_in_loop, sizeof(ban_entry_t));
            ban_in_loop->is_existing = TRUE;
            clib__copy_memory(json_field->valuestring, &ban_in_loop->ip_address[0], clib__utf8_string_length(json_field->valuestring), BAN_IP_MAX_LENGTH - 1);

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "country_iso_code");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &ban_in_loop->country_iso_code[0], clib__utf8_string_length(json_field->valuestring), COUNTRY_ISO_CODE_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "identity");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &ban_in_loop->identity[0], clib__utf8_string_length(json_field->valuestring), MAX_PUBLIC_KEY_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "extra_data");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &ban_in_loop->extra_data[0], clib__utf8_string_length(json_field->valuestring), BAN_EXTRA_DATA_MAX_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "timestamp_banned");
            if (cJSON_IsNumber(json_field))
            {
                ban_in_loop->timestamp_banned = (uint64)json_field->valuedouble;
            }

            ban_slot++;
            loaded_bans++;
        }
    }

    // load the identity store (public-key hash -> tag ids). only consulted on auth when identities are
    // enabled, but always loaded so the data survives a disable/re-enable cycle. entries with no tags are
    // dropped so they don't occupy a slot
    json_identities = cJSON_GetObjectItemCaseSensitive(json_root, "identities");
    if (cJSON_IsArray(json_identities) == TRUE)
    {
        cJSON_ArrayForEach(json_identity, json_identities)
        {
            if (identity_slot >= MAX_CLIENT_STORED_DATA)
            {
                break;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "public_key_hash");
            if (cJSON_IsString(json_field) == FALSE || json_field->valuestring == NULL_POINTER)
            {
                continue;
            }

            identity_in_loop = &g_client_stored_data[identity_slot];
            clib__null_memory(identity_in_loop, sizeof(client_stored_data_t));
            clib__copy_memory(json_field->valuestring, &identity_in_loop->public_key[0], clib__utf8_string_length(json_field->valuestring), MAX_PUBLIC_KEY_LENGTH - 1);

            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "username");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &identity_in_loop->username[0], clib__utf8_string_length(json_field->valuestring), USERNAME_MAX_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "base64_avatar");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &identity_in_loop->base64_avatar[0], clib__utf8_string_length(json_field->valuestring), MAX_CLIENT_AVATAR_LENGTH - 1);
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "last_seen");
            if (cJSON_IsNumber(json_field) == TRUE)
            {
                identity_in_loop->last_seen_unix_seconds = (int64)json_field->valuedouble;
            }

            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "alias");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &identity_in_loop->alias[0], clib__utf8_string_length(json_field->valuestring), USERNAME_MAX_LENGTH - 1);
            }

            // the raw rsa public key, so peers can encrypt to this identity while it is offline.
            // only meaningful while offline messages are enabled
            json_field = cJSON_GetObjectItemCaseSensitive(json_identity, "raw_public_key");
            if (cJSON_IsString(json_field) == TRUE && json_field->valuestring != NULL_POINTER)
            {
                clib__copy_memory(json_field->valuestring, &identity_in_loop->raw_public_key[0], clib__utf8_string_length(json_field->valuestring), MAX_PUBLIC_KEY_LENGTH - 1);
            }

            identity_in_loop->tag_id_count = 0;
            json_identity_tag_ids = cJSON_GetObjectItemCaseSensitive(json_identity, "tag_ids");
            if (cJSON_IsArray(json_identity_tag_ids) == TRUE)
            {
                cJSON_ArrayForEach(json_identity_tag_id, json_identity_tag_ids)
                {
                    if (identity_in_loop->tag_id_count >= MAX_TAGS_FOR_SINGLE_CLIENT)
                    {
                        break;
                    }
                    if (cJSON_IsNumber(json_identity_tag_id) == FALSE)
                    {
                        continue;
                    }
                    identity_in_loop->tag_ids[identity_in_loop->tag_id_count] = (uint64)json_identity_tag_id->valueint;
                    identity_in_loop->tag_id_count++;
                }
            }

            if (identity_in_loop->tag_id_count == 0 && identity_in_loop->base64_avatar[0] == 0 && identity_in_loop->alias[0] == 0)
            {
                DBG_IDENTITIES log_info("%s %s %s", "load_identities: dropping stored identity [", &identity_in_loop->public_key[0], "] - it has no tags, no avatar and no alias \n");
                clib__null_memory(identity_in_loop, sizeof(client_stored_data_t));
                continue;
            }

            DBG_IDENTITIES log_info("%s %llu %s %llu %s %s %s", "load_identities: loaded into store slot", identity_slot, "with", (uint64)identity_in_loop->tag_id_count, "tag(s), hash [", &identity_in_loop->public_key[0], "] \n");

            identity_slot++;
            loaded_identities++;
        }
    }

    // re-apply the admin tag's saved icon link. the admin tag was re-seeded with icon_id 0 before this
    // load ran; its runtime-chosen icon (and has_icon flag) is restored here, now that the icons it may
    // point at have been loaded above
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_tag_icon_id");
    if (cJSON_IsNumber(json_field))
    {
        g_tags_array[ADMIN_TAG_ID].icon_id = (uint64)json_field->valuedouble;
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_tag_has_icon");
    if (cJSON_IsBool(json_field))
    {
        g_tags_array[ADMIN_TAG_ID].has_icon = cJSON_IsTrue(json_field);
    }

    cJSON_Delete(json_root);

    if (loaded_channels > 0 || loaded_icons > 0 || loaded_tags > 0 || loaded_bans > 0 || loaded_identities > 0)
    {
        printf("%s %s%llu%s%llu%s%llu%s%llu%s%llu%s\n", g_mark_ok, "restored from server_settings.json (", loaded_channels, " channels, ", loaded_icons, " icons, ", loaded_tags, " tags, ", loaded_bans, " bans, ", loaded_identities, " identities)");
    }
}

/**
 * @brief seeds the built-in "admin" tag in slot 0 and the shield icon it may be given, at icon id 0.
 *
 *        the icon exists but the tag ships with has_icon FALSE - an icon lives at id 0, so anything
 *        resolving tag_linked_icon_id without checking has_icon paints the shield on every tag.
 *
 * @return void
 */
void settings__init_tags_and_icons(void)
{
    char tag_name[] = "admin";
    char base64_icon[] = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAACXBIWXMAAAsSAAALEgHS3X78AAACU0lEQVQ4jX2TX0hTYRjGf0eOicqUydEES9L+iHQRnNafu0ASYayrXWiS0S4HFQQhCQMzDEUGQUzIGyOJbrr03LSI/hFY5DBLhyut3CkkN9eZ7sx5bHVx9LgD0nv1vc/3PA/Py/d+AmYJLV3+fC67BsBGLst2bRo5CmszpwPw8fkzAUAAhI5AMF/RcNJGnBn02vobj6MAVJWXAnDlWjsRRXEIHYHg393Eo6Nu8iX1OJ0i0Q+fCfQ84erYe/ZJEmoiQVlpOb2BSxRlM9p/xQDNxw7TP9DG3Ysu1ESCaHzJ4hcVinV1GtklAeB0injdIbzukHUvuyTGw+MATMbmARC3hQDGyyCX+85QWduI1x2i4UIvAF53H+GJIfSsztj9IF+4vpNgXV/D3+mjpeUcajxjG8ff6cPf6bNhajxj8a0ED15NWoTK2kYcVfUAdLsP2MSNTUeB1za+aOR0dHUaR1k5sksiLzjQUinCE0M28eL8AgtzM8guieWv73YMAA41nyB6r52bg11oKylWV5aZWzOXKp02X0ksLqa6uoazrQaPHoao840Rm3phGhw/cpCnW/PHPk2hpdOMDM/aEnT3nLaM1HiGvdovs/+d+GGRlha/AjAyPEvrbcVMkDNXeeiWl/6BNpLJpM24COC7GrcALZ0GQCrbA0BFSQli3gAgXsBbSZrLJADFssezAVA69xaA83fCJPQNAPStDwbwpr8DgGzTKQAiiuIQASOiKAJQIXs8GkDs2zxVNfst8fpqCiO7WigUtk2tQ+FYssfzZxeciKLUAT8LsX+oaO/ttIYBtAAAAABJRU5ErkJggg==";
    tag_t* admin_tag = NULL_POINTER;
    icon_t* admin_icon = NULL_POINTER;

    admin_tag = &g_tags_array[0];
    admin_tag->id = ADMIN_TAG_ID;
    admin_tag->icon_id = 0;
    // stated outright rather than left to zero-init: the admin tag ships WITHOUT an icon, even though
    // a real icon lives at id 0, and clients must not paint one until an admin assigns it
    admin_tag->has_icon = FALSE;
    admin_tag->is_existing = TRUE;
    clib__copy_memory((void*)&tag_name, (void*)&admin_tag->name, strlen(tag_name), TAG_MAX_NAME_LENGTH);

    admin_icon = &g_icons_array[0];
    admin_icon->id = 0;
    admin_icon->is_existing = TRUE;
    server_admin__set_icon_base64(admin_icon, &base64_icon[0]);
}

/**
 * @brief builds the window.__SERVER_CONFIG__ script and hands it to the bundled http server, which injects
 *        it into the client.html it serves so the served page can autoconnect.
 *
 *        the connection details (websocket port, the wss port when stunnel is on, and the plaintext connect
 *        keys) are only written into the page when the admin set is_client_config_embedded. the default theme is added when one
 *        is configured. the client policy flags - whether the identity passphrase may be persisted in
 *        localStorage, and whether avatars are allowed plus their accepted max raw size - are always written
 *        in so the client knows the server's stance; both default off when this config is absent because the
 *        page was loaded directly.
 *
 * @param int64 websocket_port -> the port clients should open the websocket on
 * @param char plaintext_keys[][256] -> the connect keys in cleartext, one per row
 * @param uint64 keys_count -> how many rows of plaintext_keys are filled
 *
 * @return void
 */
static void _settings_internal__build_and_push_client_config(int64 websocket_port, char plaintext_keys[][256], uint64 keys_count)
{
    cJSON* config_object = NULL_POINTER;
    cJSON* keys_array = NULL_POINTER;
    char* config_json = NULL_POINTER;
    char config_script[2048];
    uint64 i = 0;

    config_object = cJSON_CreateObject();
    if (config_object == NULL_POINTER)
    {
        return;
    }

    // only write the connection details (port + keys) into the served page if the admin opted in
    if (g_server_settings.is_client_config_embedded == TRUE)
    {
        cJSON_AddNumberToObject(config_object, "port", (double)websocket_port);
        // Browsers need the public WSS port even when a tunnel provides it.
        if ((g_server_settings.is_stunnel_enabled == TRUE || g_server_settings.is_external_wss_enabled == TRUE) && g_server_settings.wss_port > 0 && g_server_settings.wss_port <= 65535)
        {
            cJSON_AddNumberToObject(config_object, "wss_port", (double)g_server_settings.wss_port);
        }

        keys_array = cJSON_CreateArray();
        if (keys_array != NULL_POINTER)
        {
            for (i = 0; i < keys_count; i++)
            {
                cJSON_AddItemToArray(keys_array, cJSON_CreateString(&plaintext_keys[i][0]));
            }
            cJSON_AddItemToObject(config_object, "keys", keys_array);
        }
    }

    if (g_server_settings.default_theme[0] != 0)
    {
        cJSON_AddStringToObject(config_object, "theme", &g_server_settings.default_theme[0]);
    }

    // policy flags the client honours: whether to persist the identity passphrase in localStorage, and
    // whether avatars are allowed (with the accepted max raw image size). always written so the client
    // knows the server's stance; both default off when this config is absent (page loaded directly)
    cJSON_AddBoolToObject(config_object, "persist_identity", g_server_settings.is_identity_localstorage_enabled == TRUE);
    cJSON_AddBoolToObject(config_object, "is_avatar_upload_allowed", g_server_settings.is_avatar_upload_allowed == TRUE);
    if (g_server_settings.is_avatar_upload_allowed == TRUE)
    {
        cJSON_AddNumberToObject(config_object, "avatar_max_size", (double)g_server_settings.avatar_max_size_bytes);
    }

    config_json = cJSON_PrintUnformatted(config_object);
    if (config_json != NULL_POINTER)
    {
        clib__null_memory(config_script, sizeof(config_script));
        snprintf(config_script, sizeof(config_script), "window.__SERVER_CONFIG__=%s;", config_json);
        http_server__set_client_config(config_script);
        cJSON_free(config_json);
    }

    cJSON_Delete(config_object);
}

/**
 * @brief reads the wireguard mode fields from a parsed server_settings.json: the switch, the router's
 *        public address and the forwarded udp range. called by settings__load and by the first time
 *        setup after the tunnel setup wrote the file.
 *
 * @param cJSON* json_root -> the parsed settings file
 *
 * @attention an unusable address or range switches the mode off
 *
 * @return void
 */
void settings__read_wireguard_fields(cJSON* json_root)
{
    cJSON* json_field = NULL_POINTER;

    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_wireguard_mode_active");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_wireguard_mode_active = cJSON_IsTrue(json_field);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_public_address");
    if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER)
    {
        clib__null_memory(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address));
        clib__copy_memory(json_field->valuestring, g_server_settings.webrtc_public_address, clib__utf8_string_length(json_field->valuestring), sizeof(g_server_settings.webrtc_public_address) - 1);
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_port_range_begin");
    if (cJSON_IsNumber(json_field))
    {
        g_server_settings.webrtc_port_range_begin = json_field->valueint;
    }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_port_range_end");
    if (cJSON_IsNumber(json_field))
    {
        g_server_settings.webrtc_port_range_end = json_field->valueint;
    }

    DBG_SETTINGS log_info("%s %s %s %s %s %lld %s %lld", "wireguard fields: mode", g_server_settings.is_wireguard_mode_active == TRUE ? "on" : "off", ", public address", g_server_settings.webrtc_public_address, ", port range", (long long)g_server_settings.webrtc_port_range_begin, "to", (long long)g_server_settings.webrtc_port_range_end);

    // an unusable address or range switches the mode off rather than starting with a broken voice path
    if (g_server_settings.is_wireguard_mode_active == TRUE && (g_server_settings.webrtc_public_address[0] == 0 || g_server_settings.webrtc_port_range_begin < 1024 || g_server_settings.webrtc_port_range_begin > 65535 || g_server_settings.webrtc_port_range_end < g_server_settings.webrtc_port_range_begin || g_server_settings.webrtc_port_range_end > 65535))
    {
        printf("%s %s\n", g_mark_warn, "wireguard mode: webrtc_public_address or the webrtc_port_range is not usable, mode switched off for this run");
        g_server_settings.is_wireguard_mode_active = FALSE;
    }
}

/**
 * @brief fills g_server_settings with the built-in defaults, then overrides them from server_settings.json
 *        or, when there is no usable file, from the interactive first time setup.
 *
 *        the defaults include the max client and channel counts, because the json path returns early and
 *        the global arrays would otherwise allocate at size 0. any field the json omits keeps its default.
 *        connect keys are stored as sha256 hashes; the cleartext is kept only in a local buffer, so it can
 *        be handed to the embedded client config and to the first time setup. a file that exists but does
 *        not parse is reported and falls through to the interactive setup. either path ends by pushing the
 *        client config to the bundled http server.
 *
 * @return void
 */
void settings__load(void)
{
    char plaintext_keys[100][256];
    char default_client_name[30] = "user";

    FILE* settings_file = 0;
    int64 file_length = 0;
    char* file_buffer = NULL_POINTER;
    cJSON* json_root = NULL_POINTER;
    uint64 bytes_read = 0;
    cJSON* json_field = NULL_POINTER;
    cJSON* json_key = NULL_POINTER;
    int64 key_index = 0;
    uint64 banner_length = 0;

    clib__null_memory(&g_server_settings, sizeof(server_settings_t));
    clib__null_memory(plaintext_keys, sizeof(plaintext_keys));
    g_server_settings.is_server_uptime_published = TRUE;
    g_server_settings.is_server_slots_published = TRUE;
    g_server_settings.is_server_banner_published = TRUE;
    g_server_settings.is_server_features_published = TRUE;
    g_server_settings.websocket_message_max_length = 5000000;
    g_server_settings.websocket_chat_message_string_max_length = 8000;
    g_server_settings.chat_cooldown_milliseconds = 100;
    g_server_settings.join_channel_request_cooldown_milliseconds = 100;
    g_server_settings.create_channel_request_cooldown_milliseconds = 1000;
    g_server_settings.is_same_ip_address_allowed = TRUE;
    g_server_settings.is_audio_over_websocket_allowed = TRUE; // both connections allowed: the client picks by its own preference
    g_server_settings.is_audio_over_webrtc_allowed = TRUE;
    g_server_settings.is_voice_chat_active = TRUE;
    g_server_settings.is_music_bot_audio_active = TRUE;
    g_server_settings.is_video_streaming_active = FALSE; // explicit opt-in by the admin in the settings tab
    g_server_settings.is_hide_clients_in_password_protected_channels_active = TRUE;
    g_server_settings.is_temp_channel_creation_allowed = FALSE;
    g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active = FALSE;
    g_server_settings.is_display_country_flags_active = FALSE;
    g_server_settings.is_admin_country_flag_hidden = FALSE;
    g_server_settings.is_display_admin_tag_active = TRUE;
    g_server_settings.is_idle_mode_allowed = TRUE;
    g_server_settings.is_fast_reconnect_allowed = FALSE;
    g_server_settings.is_identity_takeover_allowed = FALSE;
    g_server_settings.is_websocket_ping_active = FALSE;
    g_server_settings.is_connection_check_enforced = TRUE;
    g_server_settings.is_channel_key_reuse_enabled = TRUE;
    g_server_settings.is_channel_key_reuse_on_join_allowed = TRUE;
    g_server_settings.is_channel_key_reuse_on_leave_allowed = TRUE;
    g_server_settings.channel_key_reuse_join_min_clients = 50;
    g_server_settings.channel_key_reuse_leave_min_clients = 100;
    g_server_settings.max_concurrent_logins = 4;
    g_server_settings.webrtc_datachannel_cooldown_seconds = 600;
    g_server_settings.is_music_bot_marquee_visible_to_everyone = FALSE;
    g_server_settings.is_sending_text_to_idle_clients_allowed = TRUE;
    g_server_settings.is_private_messaging_allowed = TRUE;
    g_server_settings.is_identity_storage_enabled = TRUE;
    g_server_settings.is_identity_localstorage_enabled = FALSE;
    g_server_settings.is_avatar_upload_allowed = FALSE;
    g_server_settings.is_alias_registration_allowed = FALSE;
    g_server_settings.is_stored_client_list_allowed = FALSE;
    g_server_settings.is_last_seen_enabled = FALSE;
    g_server_settings.is_offline_messaging_allowed = FALSE;
    g_server_settings.is_typing_indicator_allowed = FALSE;
    g_server_settings.is_client_rename_allowed = TRUE;
    g_server_settings.is_link_preview_allowed = FALSE;
    g_server_settings.avatar_max_size_bytes = 51200; // 50 KB raw image (~68 KB base64, fits MAX_CLIENT_AVATAR_LENGTH)
    g_server_settings.icon_max_size_bytes = 5000;
    g_server_settings.is_file_upload_allowed = FALSE;
    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_DEFAULT_SIZE_BYTES;
    g_server_settings.chat_picture_max_size_bytes = PICTURE_DEFAULT_SIZE_BYTES;
    g_server_settings.is_chat_picture_allowed = TRUE;
    g_server_settings.is_country_blocking_active = FALSE;
    g_server_settings.minimum_rsa_key_bits = 2048;
    g_server_settings.is_minimum_rsa_key_bits_announced = FALSE;
    g_server_settings.blocked_countries_count = 0;
    g_server_settings.is_logging_client_joins_enabled = FALSE;
    g_server_settings.is_logging_username_changes_enabled = FALSE;
    g_server_settings.is_logging_tag_changes_enabled = FALSE;
    g_server_settings.is_logging_server_settings_updates_enabled = FALSE;
    g_server_settings.is_logging_kicks_and_bans_enabled = FALSE;
    g_server_settings.is_logging_client_disconnects_enabled = FALSE;
    g_server_settings.is_logging_socket_opens_and_closes_enabled = FALSE;
    g_server_settings.is_logging_failed_attempts_enabled = FALSE;
    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_DEFAULT_SIZE_BYTES;
    g_server_settings.admin_log_retention_days = ADMIN_LOG_DEFAULT_RETENTION_DAYS;

    // set the max client/channel counts here too; the JSON path below returns early, so without this the arrays would allocate at size 0
    g_server_settings.max_client_count = DEFAULT_SERVER_SLOTS;
    g_server_settings.max_channel_count = MAX_CHANNELS;

    // wireguard mode is off unless the setup wizard or selfhosting-tools/wireguard/ scripts switch it on; the range is the
    // default the scripts forward
    g_server_settings.is_wireguard_mode_active = FALSE;
    clib__null_memory(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address));
    g_server_settings.webrtc_port_range_begin = 20000;
    g_server_settings.webrtc_port_range_end = 20099;

    clib__copy_memory(default_client_name, g_server_settings.default_client_name, strlen(default_client_name), 100);

    // optional non-interactive setup: if server_settings.json exists, load it and skip the prompts below; any omitted field keeps the default set above
    {
        settings_file = fopen("server_settings.json", "rb");
        if (settings_file != NULL_POINTER)
        {
            fseek(settings_file, 0, SEEK_END);
            file_length = ftell(settings_file);
            fseek(settings_file, 0, SEEK_SET);
            if (file_length > 0)
            {
                file_buffer = (char*)malloc(file_length + 1);
                if (file_buffer != NULL_POINTER)
                {
                    bytes_read = fread(file_buffer, 1, file_length, settings_file);
                    file_buffer[bytes_read] = 0;
                }
            }
            fclose(settings_file);

            if (file_buffer != NULL_POINTER)
            {
                json_root = cJSON_Parse(file_buffer);
                free(file_buffer);
            }

            if (json_root != NULL_POINTER)
            {
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "server_slots");
                if (json_field != NULL_POINTER)
                {
                    if (cJSON_IsNumber(json_field) == FALSE || !(json_field->valuedouble >= 1 && json_field->valuedouble <= MAX_SERVER_SLOTS) || json_field->valuedouble != (double)(uint64)json_field->valuedouble)
                    {
                        fprintf(stderr, "server_slots must be an integer from 1 to %d; fix server_settings.json before starting.\n", MAX_SERVER_SLOTS);
                        cJSON_Delete(json_root);
                        exit(EXIT_FAILURE);
                    }
                    g_server_settings.max_client_count = (uint64)json_field->valuedouble;
                }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "websocket_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    g_server_settings.websocket_port = json_field->valueint;
                }

                settings__read_wireguard_fields(json_root);

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_password");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.admin_password[0], clib__utf8_string_length(json_field->valuestring), 50);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_admin_password_initial");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_admin_password_initial = cJSON_IsTrue(json_field);
                }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "keys");
                if (cJSON_IsArray(json_field) == TRUE)
                {
                    cJSON_ArrayForEach(json_key, json_field)
                    {
                        if ((key_index < 100) && cJSON_IsString(json_key) && (json_key->valuestring != NULL_POINTER))
                        {
                            // The settings file stores these passwords as text.
                            // Turn each one into an AES key and store its type
                            // with its bytes, so later code knows how to use it.
                            if (!metadata_crypto__key_from_password(&g_server_settings.keys[key_index], CRYPTO_KEY_AES_256_CTR, json_key->valuestring))
                            {
                                fprintf(stderr, "Failed to derive a configured encryption key.\n");
                                exit(EXIT_FAILURE);
                            }
                            clib__copy_memory(json_key->valuestring, &plaintext_keys[key_index][0], clib__utf8_string_length(json_key->valuestring), 255);
                            key_index++;
                        }
                    }
                    g_server_settings.keys_count = key_index;
                }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_voice_chat_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_voice_chat_active = cJSON_IsTrue(json_field);
                }
                // Older Android server apps saved one mode string. Honor it when the explicit
                // policy fields below are absent; those fields remain authoritative when present.
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_same_ip_address_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_same_ip_address_allowed = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_idle_mode_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_idle_mode_allowed = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_websocket_ping_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_websocket_ping_active = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "channel_key_reuse_join_min_clients");
                if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 30 && json_field->valuedouble <= 1000000)
                {
                    g_server_settings.channel_key_reuse_join_min_clients = (int64)json_field->valuedouble;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "channel_key_reuse_leave_min_clients");
                if (cJSON_IsNumber(json_field) && json_field->valuedouble >= 30 && json_field->valuedouble <= 1000000)
                {
                    g_server_settings.channel_key_reuse_leave_min_clients = (int64)json_field->valuedouble;
                }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_connection_check_enforced");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_connection_check_enforced = cJSON_IsTrue(json_field);
                }
                settings__read_attachment_receive_policy(json_root);
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "max_concurrent_logins");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.max_concurrent_logins = json_field->valueint;
                    if (g_server_settings.max_concurrent_logins < 1)
                    {
                        g_server_settings.max_concurrent_logins = 1;
                    }
                    if (g_server_settings.max_concurrent_logins > 64)
                    {
                        g_server_settings.max_concurrent_logins = 64;
                    }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_datachannel_cooldown_seconds");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.webrtc_datachannel_cooldown_seconds = (int64)json_field->valuedouble;
                    if (g_server_settings.webrtc_datachannel_cooldown_seconds < 0)
                    {
                        g_server_settings.webrtc_datachannel_cooldown_seconds = 0;
                    }
                    if (g_server_settings.webrtc_datachannel_cooldown_seconds > 86400)
                    {
                        g_server_settings.webrtc_datachannel_cooldown_seconds = 86400;
                    }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_music_bot_marquee_visible_to_everyone");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_music_bot_marquee_visible_to_everyone = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_restart_on_crash_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_restart_on_crash_enabled = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_avatar_upload_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_avatar_upload_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_alias_registration_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_alias_registration_allowed = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "avatar_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.avatar_max_size_bytes = (int64)json_field->valuedouble;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "icon_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.icon_max_size_bytes = (int64)json_field->valuedouble;
                    if (g_server_settings.icon_max_size_bytes < 1000)
                    {
                        g_server_settings.icon_max_size_bytes = 1000;
                    }
                    if (g_server_settings.icon_max_size_bytes > 30000)
                    {
                        g_server_settings.icon_max_size_bytes = 30000;
                    }
                }

                // optional bundled-stunnel front-end (wss)
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_stunnel_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_stunnel_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_external_wss_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_external_wss_enabled = cJSON_IsTrue(json_field);
                }
                if (g_server_settings.is_external_wss_enabled == TRUE)
                {
                    g_server_settings.is_stunnel_enabled = FALSE;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "wss_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    g_server_settings.wss_port = json_field->valueint;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_domain");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.stunnel_domain[0], clib__utf8_string_length(json_field->valuestring), 255);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_cert_fullchain");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.stunnel_cert_fullchain[0], clib__utf8_string_length(json_field->valuestring), 511);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_cert_privkey");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.stunnel_cert_privkey[0], clib__utf8_string_length(json_field->valuestring), 511);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "client_html_dest");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.client_html_dest[0], clib__utf8_string_length(json_field->valuestring), 511);
                }

                // optional bundled http server that serves the client (mainly for LAN testing)
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_client_http_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_client_http_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    g_server_settings.http_port = json_field->valueint;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_https_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_https_enabled = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "https_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    g_server_settings.https_port = json_field->valueint;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_webroot");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.http_webroot[0], clib__utf8_string_length(json_field->valuestring), 511);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "default_theme");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.default_theme[0], clib__utf8_string_length(json_field->valuestring), 31);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "server_banner");
                if (cJSON_IsString(json_field) && server_admin__is_server_banner_valid(json_field->valuestring) == TRUE)
                {
                    banner_length = clib__utf8_string_length(json_field->valuestring);
                    if (banner_length > 0)
                    {
                        g_server_settings.server_banner = (char*)memorymanager__allocate(banner_length + 1, MEMALLOC_SERVER_BANNER);
                        if (g_server_settings.server_banner != NULL_POINTER)
                        {
                            clib__copy_memory(json_field->valuestring, g_server_settings.server_banner, banner_length + 1, banner_length + 1);
                        }
                    }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_client_config_embedded");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_client_config_embedded = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_file_upload_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_file_upload_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "file_upload_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.file_upload_max_size_bytes = (int64)json_field->valuedouble;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "minimum_rsa_key_bits");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.minimum_rsa_key_bits = (int64)json_field->valuedouble;
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
                if (g_server_settings.file_upload_max_size_bytes < FILE_UPLOAD_MIN_SIZE_BYTES)
                {
                    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MIN_SIZE_BYTES;
                }
                if (g_server_settings.file_upload_max_size_bytes > FILE_UPLOAD_MAX_SIZE_BYTES)
                {
                    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MAX_SIZE_BYTES;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "chat_picture_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.chat_picture_max_size_bytes = (int64)json_field->valuedouble;
                }
                if (g_server_settings.chat_picture_max_size_bytes < PICTURE_MIN_SIZE_BYTES)
                {
                    g_server_settings.chat_picture_max_size_bytes = PICTURE_MIN_SIZE_BYTES;
                }
                if (g_server_settings.chat_picture_max_size_bytes > PICTURE_MAX_SIZE_BYTES)
                {
                    g_server_settings.chat_picture_max_size_bytes = PICTURE_MAX_SIZE_BYTES;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_chat_picture_allowed");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_chat_picture_allowed = cJSON_IsTrue(json_field);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_country_blocking_active");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_country_blocking_active = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_logging_tag_changes_enabled");
                if (cJSON_IsBool(json_field))
                {
                    g_server_settings.is_logging_tag_changes_enabled = cJSON_IsTrue(json_field);
                }
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_log_max_size_bytes");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.admin_log_max_size_bytes = (int64)json_field->valuedouble;
                }
                if (g_server_settings.admin_log_max_size_bytes < ADMIN_LOG_MIN_SIZE_BYTES)
                {
                    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MIN_SIZE_BYTES;
                }
                if (g_server_settings.admin_log_max_size_bytes > ADMIN_LOG_MAX_SIZE_BYTES)
                {
                    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MAX_SIZE_BYTES;
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_log_retention_days");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.admin_log_retention_days = (int64)json_field->valuedouble;
                }
                if (g_server_settings.admin_log_retention_days < 1)
                {
                    g_server_settings.admin_log_retention_days = 1;
                }
                if (g_server_settings.admin_log_retention_days > ADMIN_LOG_MAX_RETENTION_DAYS)
                {
                    g_server_settings.admin_log_retention_days = ADMIN_LOG_MAX_RETENTION_DAYS;
                }

                _settings_internal__build_and_push_client_config(g_server_settings.websocket_port, plaintext_keys, g_server_settings.keys_count);

                cJSON_Delete(json_root);

                return;
            }

            printf("%s", "server_settings.json found but could not be parsed; using interactive setup\n");
        }
    }

    first_time_setup__run(plaintext_keys);
    _settings_internal__build_and_push_client_config(g_server_settings.websocket_port, plaintext_keys, g_server_settings.keys_count);
}

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
boole settings__write_file_atomically(char* path, char* contents)
{
    /* A leaf mutex: callers may hold state locks, but no state lock is acquired
     * here. Concurrent writers must not truncate the same temporary file. */
    static pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;
    char tmp_path[1024];
    FILE* file = NULL_POINTER;
    uint64 contents_length = 0;
    boole succeeded = FALSE;
    boole io_failed = FALSE;
    int path_length;

    if (path == NULL_POINTER || contents == NULL_POINTER)
    {
        return FALSE;
    }
    path_length = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (path_length < 0 || (size_t)path_length >= sizeof(tmp_path))
    {
        return FALSE;
    }

    pthread_mutex_lock(&write_mutex);
    file = fopen(tmp_path, "wb");
    if (file == NULL_POINTER)
    {
        goto atomic_write_end;
    }

    contents_length = strlen(contents);
    io_failed = (boole)(fwrite(contents, 1, contents_length, file) != contents_length);
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

    if (io_failed == FALSE)
    {
#ifdef WIN32
        DWORD replace_error = 0;
        // Windows scanners/editors may briefly hold a handle without delete
        // sharing. Retry only those errors, with a bounded wait; never remove
        // the original file to make replacement succeed.
        for (int attempt = 0; attempt < 10; attempt++)
        {
            succeeded = (boole)(MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
            if (succeeded == TRUE)
            {
                break;
            }
            replace_error = GetLastError();
            if (replace_error != ERROR_ACCESS_DENIED && replace_error != ERROR_SHARING_VIOLATION && replace_error != ERROR_LOCK_VIOLATION)
            {
                break;
            }
            if (attempt < 9)
            {
                Sleep(20);
            }
        }
        if (succeeded == FALSE)
        {
            log_error("Atomic replacement of %s failed (Windows error %lu)", path, replace_error);
        }
#else
        succeeded = (boole)(rename(tmp_path, path) == 0);
#endif
    }
    if (succeeded == FALSE)
    {
        remove(tmp_path);
    }

atomic_write_end:
    pthread_mutex_unlock(&write_mutex);
    return succeeded;
}

/**
 * @brief writes the in-memory identity store (g_client_stored_data) into json_root under the "identities"
 *        key: one object per stored identity with its public-key hash and the tag ids it owns. entries with
 *        no hash or no tags are skipped. when identities are disabled the existing "identities" key is left
 *        untouched, so disabling then re-enabling does not delete the stored data. guarded by the store mutex.
 *
 * @param cJSON* json_root -> the settings document being written
 *
 * @return void
 */
static void _settings_internal__serialize_identities(cJSON* json_root)
{
    cJSON* json_identities = NULL_POINTER;
    cJSON* json_identity = NULL_POINTER;
    cJSON* json_identity_tag_ids = NULL_POINTER;
    uint64 i = 0;
    uint64 t = 0;

    if (g_server_settings.is_identity_storage_enabled == FALSE)
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
        if (g_server_settings.is_last_seen_enabled == TRUE && g_client_stored_data[i].last_seen_unix_seconds != 0)
        {
            cJSON_AddNumberToObject(json_identity, "last_seen", (double)g_client_stored_data[i].last_seen_unix_seconds);
        }
        // same rule for the raw public key: written only while offline messages are enabled, so
        // turning the feature off also stops carrying the keys into the next save
        if (g_server_settings.is_offline_messaging_allowed == TRUE && g_client_stored_data[i].raw_public_key[0] != 0)
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
boole settings__save_server_settings_to_file(void)
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
        file_buffer = (char*)malloc(file_length + 1);
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
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "server_banner");
    if (cJSON_AddStringToObject(json_root, "server_banner", g_server_settings.server_banner != NULL_POINTER ? g_server_settings.server_banner : "") == NULL_POINTER)
    {
        cJSON_Delete(json_root);
        return FALSE;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_server_uptime_published");
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_server_slots_published");
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_server_banner_published");
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_server_features_published");
    if (cJSON_AddBoolToObject(json_root, "is_server_uptime_published", g_server_settings.is_server_uptime_published) == NULL_POINTER || cJSON_AddBoolToObject(json_root, "is_server_slots_published", g_server_settings.is_server_slots_published) == NULL_POINTER || cJSON_AddBoolToObject(json_root, "is_server_banner_published", g_server_settings.is_server_banner_published) == NULL_POINTER || cJSON_AddBoolToObject(json_root, "is_server_features_published", g_server_settings.is_server_features_published) == NULL_POINTER)
    {
        cJSON_Delete(json_root);
        return FALSE;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_display_country_flags_active");
    cJSON_AddItemToObject(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_admin_country_flag_hidden");
    cJSON_AddItemToObject(json_root, "is_admin_country_flag_hidden", cJSON_CreateBool(g_server_settings.is_admin_country_flag_hidden == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_voice_chat_active");
    cJSON_AddItemToObject(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_audio_over_websocket_allowed");
    cJSON_AddItemToObject(json_root, "is_audio_over_websocket_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_websocket_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_audio_over_webrtc_allowed");
    cJSON_AddItemToObject(json_root, "is_audio_over_webrtc_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_webrtc_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_music_bot_audio_active");
    cJSON_AddItemToObject(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_video_streaming_active");
    cJSON_AddItemToObject(json_root, "is_video_streaming_active", cJSON_CreateBool(g_server_settings.is_video_streaming_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_hide_clients_in_password_protected_channels_active");
    cJSON_AddItemToObject(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_temp_channel_creation_allowed");
    cJSON_AddItemToObject(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));

    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_typing_indicator_allowed");
    cJSON_AddItemToObject(json_root, "is_typing_indicator_allowed", cJSON_CreateBool(g_server_settings.is_typing_indicator_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_client_rename_allowed");
    cJSON_AddItemToObject(json_root, "is_client_rename_allowed", cJSON_CreateBool(g_server_settings.is_client_rename_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_link_preview_allowed");
    cJSON_AddBoolToObject(json_root, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_sending_text_to_idle_clients_allowed");
    cJSON_AddItemToObject(json_root, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_private_messaging_allowed");
    cJSON_AddItemToObject(json_root, "is_private_messaging_allowed", cJSON_CreateBool(g_server_settings.is_private_messaging_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_same_ip_address_allowed");
    cJSON_AddItemToObject(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_fast_reconnect_allowed");
    cJSON_AddItemToObject(json_root, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_identity_takeover_allowed");
    cJSON_AddItemToObject(json_root, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_websocket_ping_active");
    cJSON_AddItemToObject(json_root, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_channel_key_reuse_enabled");
    cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_channel_key_reuse_on_join_allowed");
    cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_channel_key_reuse_on_leave_allowed");
    cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "channel_key_reuse_join_min_clients");
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "channel_key_reuse_leave_min_clients");
    cJSON_AddNumberToObject(json_root, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients);
    cJSON_AddNumberToObject(json_root, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_connection_check_enforced");
    cJSON_AddBoolToObject(json_root, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced == TRUE);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "max_concurrent_logins");
    cJSON_AddNumberToObject(json_root, "max_concurrent_logins", (double)g_server_settings.max_concurrent_logins);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "outgoing_queue_limit_mb");
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "server_ram_limit_mb");
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_blocking_direct_chat_pictures_active");
    cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_pictures_active", g_server_settings.is_blocking_direct_chat_pictures_active);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_blocking_direct_chat_files_active");
    cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_files_active", g_server_settings.is_blocking_direct_chat_files_active);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_blocking_channel_chat_pictures_active");
    cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_pictures_active", g_server_settings.is_blocking_channel_chat_pictures_active);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_blocking_channel_chat_files_active");
    cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_files_active", g_server_settings.is_blocking_channel_chat_files_active);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "webrtc_datachannel_cooldown_seconds");
    cJSON_AddNumberToObject(json_root, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "icon_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_music_bot_marquee_visible_to_everyone");
    cJSON_AddItemToObject(json_root, "is_music_bot_marquee_visible_to_everyone", cJSON_CreateBool(g_server_settings.is_music_bot_marquee_visible_to_everyone == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_file_upload_allowed");
    cJSON_AddItemToObject(json_root, "is_file_upload_allowed", cJSON_CreateBool(g_server_settings.is_file_upload_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "file_upload_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "file_upload_max_size_bytes", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "avatar_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "avatar_max_size_bytes", (double)g_server_settings.avatar_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "chat_picture_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "chat_picture_max_size_bytes", (double)g_server_settings.chat_picture_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_chat_picture_allowed");
    cJSON_AddItemToObject(json_root, "is_chat_picture_allowed", cJSON_CreateBool(g_server_settings.is_chat_picture_allowed == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_country_blocking_active");
    cJSON_AddItemToObject(json_root, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "minimum_rsa_key_bits");
    cJSON_AddNumberToObject(json_root, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_minimum_rsa_key_bits_announced");
    cJSON_AddItemToObject(json_root, "is_minimum_rsa_key_bits_announced", cJSON_CreateBool(g_server_settings.is_minimum_rsa_key_bits_announced == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "blocked_countries");
    json_blocked_countries = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "blocked_countries", json_blocked_countries);
    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        cJSON_AddItemToArray(json_blocked_countries, cJSON_CreateString(&g_server_settings.blocked_countries[i][0]));
    }
    // only the log TOGGLES are persisted; the log lines themselves stay in ram, never in a file
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_client_joins_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_client_joins_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_joins_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_username_changes_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_username_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_username_changes_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_tag_changes_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_tag_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_tag_changes_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_server_settings_updates_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_server_settings_updates_enabled", cJSON_CreateBool(g_server_settings.is_logging_server_settings_updates_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_kicks_and_bans_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_kicks_and_bans_enabled", cJSON_CreateBool(g_server_settings.is_logging_kicks_and_bans_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_client_disconnects_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_client_disconnects_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_disconnects_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_socket_opens_and_closes_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_socket_opens_and_closes_enabled", cJSON_CreateBool(g_server_settings.is_logging_socket_opens_and_closes_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_logging_failed_attempts_enabled");
    cJSON_AddItemToObject(json_root, "is_logging_failed_attempts_enabled", cJSON_CreateBool(g_server_settings.is_logging_failed_attempts_enabled == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_log_max_size_bytes");
    cJSON_AddNumberToObject(json_root, "admin_log_max_size_bytes", (double)g_server_settings.admin_log_max_size_bytes);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_log_retention_days");
    cJSON_AddNumberToObject(json_root, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "admin_password");
    cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]);
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_admin_password_initial");
    cJSON_AddItemToObject(json_root, "is_admin_password_initial", cJSON_CreateBool(g_server_settings.is_admin_password_initial == TRUE));
    cJSON_DeleteItemFromObjectCaseSensitive(json_root, "is_identity_storage_enabled");
    cJSON_AddItemToObject(json_root, "is_identity_storage_enabled", cJSON_CreateBool(g_server_settings.is_identity_storage_enabled == TRUE));

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
        cJSON_AddItemToObject(json_channel, "is_video_stream_enabled", cJSON_CreateBool(channel_in_loop->is_video_stream_enabled == TRUE));
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
    _settings_internal__serialize_identities(json_root);

    json_text = cJSON_Print(json_root);
    if (json_text == NULL_POINTER)
    {
        cJSON_Delete(json_root);
        return FALSE;
    }

    write_succeeded = settings__write_file_atomically("server_settings.json", json_text);
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
 * @brief writes the current settings back to server_settings.json so the next start
 *        is non-interactive
 *
 *        serializes every g_server_settings field with cJSON, appends the plaintext keys as a
 *        "keys" array, and writes the result through settings__write_file_atomically. a failed write
 *        is only reported on stdout - setup still continues, the answers are just not persisted.
 *
 * @param char plaintext_keys[][256] -> the channel keys as entered, one per row (g_server_settings keeps
 *        only their hashes, but the file stores plaintext keys, which are re-hashed on load)
 * @param uint64 keys_count -> number of valid entries in plaintext_keys
 *
 * @return void
 */
void settings__save_initial(char plaintext_keys[][256], uint64 keys_count)
{
    cJSON* json_root = 0;
    cJSON* json_keys = 0;
    char* json_text = 0;
    uint64 i = 0;

    json_root = cJSON_CreateObject();
    if (json_root == NULL_POINTER)
    {
        return;
    }

    cJSON_AddNumberToObject(json_root, "websocket_port", g_server_settings.websocket_port);
    cJSON_AddItemToObject(json_root, "is_wireguard_mode_active", cJSON_CreateBool(g_server_settings.is_wireguard_mode_active == TRUE));
    cJSON_AddStringToObject(json_root, "webrtc_public_address", g_server_settings.webrtc_public_address);
    cJSON_AddNumberToObject(json_root, "webrtc_port_range_begin", (double)g_server_settings.webrtc_port_range_begin);
    cJSON_AddNumberToObject(json_root, "webrtc_port_range_end", (double)g_server_settings.webrtc_port_range_end);
    cJSON_AddItemToObject(json_root, "is_audio_over_websocket_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_websocket_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_audio_over_webrtc_allowed", cJSON_CreateBool(g_server_settings.is_audio_over_webrtc_allowed == TRUE));
    cJSON_AddNumberToObject(json_root, "server_slots", (double)g_server_settings.max_client_count);
    cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]);
    cJSON_AddItemToObject(json_root, "is_admin_password_initial", cJSON_CreateBool(g_server_settings.is_admin_password_initial == TRUE));

    json_keys = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "keys", json_keys);
    for (i = 0; i < keys_count; i++)
    {
        cJSON_AddItemToArray(json_keys, cJSON_CreateString(&plaintext_keys[i][0]));
    }

    cJSON_AddItemToObject(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    // never a wizard question: off until the admin switches it on in the server settings tab
    cJSON_AddItemToObject(json_root, "is_video_streaming_active", cJSON_CreateBool(g_server_settings.is_video_streaming_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_AddNumberToObject(json_root, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_AddItemToObject(json_root, "is_minimum_rsa_key_bits_announced", cJSON_CreateBool(g_server_settings.is_minimum_rsa_key_bits_announced == TRUE));
    cJSON_AddItemToObject(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_idle_mode_allowed", cJSON_CreateBool(g_server_settings.is_idle_mode_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_admin_country_flag_hidden", cJSON_CreateBool(g_server_settings.is_admin_country_flag_hidden == TRUE));
    cJSON_AddItemToObject(json_root, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_enabled", g_server_settings.is_channel_key_reuse_enabled);
    cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_join_allowed", g_server_settings.is_channel_key_reuse_on_join_allowed);
    cJSON_AddBoolToObject(json_root, "is_channel_key_reuse_on_leave_allowed", g_server_settings.is_channel_key_reuse_on_leave_allowed);
    cJSON_AddNumberToObject(json_root, "channel_key_reuse_join_min_clients", (double)g_server_settings.channel_key_reuse_join_min_clients);
    cJSON_AddNumberToObject(json_root, "channel_key_reuse_leave_min_clients", (double)g_server_settings.channel_key_reuse_leave_min_clients);
    cJSON_AddBoolToObject(json_root, "is_connection_check_enforced", g_server_settings.is_connection_check_enforced == TRUE);
    cJSON_AddNumberToObject(json_root, "max_concurrent_logins", (double)g_server_settings.max_concurrent_logins);
    cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_pictures_active", g_server_settings.is_blocking_direct_chat_pictures_active);
    cJSON_AddBoolToObject(json_root, "is_blocking_direct_chat_files_active", g_server_settings.is_blocking_direct_chat_files_active);
    cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_pictures_active", g_server_settings.is_blocking_channel_chat_pictures_active);
    cJSON_AddBoolToObject(json_root, "is_blocking_channel_chat_files_active", g_server_settings.is_blocking_channel_chat_files_active);
    cJSON_AddNumberToObject(json_root, "webrtc_datachannel_cooldown_seconds", (double)g_server_settings.webrtc_datachannel_cooldown_seconds);
    cJSON_AddItemToObject(json_root, "is_music_bot_marquee_visible_to_everyone", cJSON_CreateBool(g_server_settings.is_music_bot_marquee_visible_to_everyone == TRUE));
    cJSON_AddItemToObject(json_root, "is_restart_on_crash_enabled", cJSON_CreateBool(g_server_settings.is_restart_on_crash_enabled == TRUE));

    cJSON_AddItemToObject(json_root, "is_stunnel_enabled", cJSON_CreateBool(g_server_settings.is_stunnel_enabled == TRUE));
    cJSON_AddBoolToObject(json_root, "is_external_wss_enabled", g_server_settings.is_external_wss_enabled == TRUE);
    cJSON_AddNumberToObject(json_root, "wss_port", g_server_settings.wss_port);
    cJSON_AddStringToObject(json_root, "stunnel_domain", &g_server_settings.stunnel_domain[0]);
    cJSON_AddStringToObject(json_root, "stunnel_cert_fullchain", &g_server_settings.stunnel_cert_fullchain[0]);
    cJSON_AddStringToObject(json_root, "stunnel_cert_privkey", &g_server_settings.stunnel_cert_privkey[0]);
    cJSON_AddStringToObject(json_root, "client_html_dest", &g_server_settings.client_html_dest[0]);

    cJSON_AddItemToObject(json_root, "is_client_http_enabled", cJSON_CreateBool(g_server_settings.is_client_http_enabled == TRUE));
    cJSON_AddNumberToObject(json_root, "http_port", g_server_settings.http_port);
    cJSON_AddStringToObject(json_root, "http_webroot", &g_server_settings.http_webroot[0]);
    cJSON_AddItemToObject(json_root, "is_https_enabled", cJSON_CreateBool(g_server_settings.is_https_enabled == TRUE));
    cJSON_AddNumberToObject(json_root, "https_port", g_server_settings.https_port);
    cJSON_AddStringToObject(json_root, "default_theme", &g_server_settings.default_theme[0]);
    cJSON_AddItemToObject(json_root, "is_client_config_embedded", cJSON_CreateBool(g_server_settings.is_client_config_embedded == TRUE));
    cJSON_AddItemToObject(json_root, "is_identity_localstorage_enabled", cJSON_CreateBool(g_server_settings.is_identity_localstorage_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_identity_storage_enabled", cJSON_CreateBool(g_server_settings.is_identity_storage_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_avatar_upload_allowed", cJSON_CreateBool(g_server_settings.is_avatar_upload_allowed == TRUE));
    cJSON_AddNumberToObject(json_root, "avatar_max_size_bytes", (double)g_server_settings.avatar_max_size_bytes);
    cJSON_AddNumberToObject(json_root, "icon_max_size_bytes", (double)g_server_settings.icon_max_size_bytes);
    cJSON_AddItemToObject(json_root, "is_alias_registration_allowed", cJSON_CreateBool(g_server_settings.is_alias_registration_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_stored_client_list_allowed", cJSON_CreateBool(g_server_settings.is_stored_client_list_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_last_seen_enabled", cJSON_CreateBool(g_server_settings.is_last_seen_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_offline_messaging_allowed", cJSON_CreateBool(g_server_settings.is_offline_messaging_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_typing_indicator_allowed", cJSON_CreateBool(g_server_settings.is_typing_indicator_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_client_rename_allowed", cJSON_CreateBool(g_server_settings.is_client_rename_allowed == TRUE));
    cJSON_AddBoolToObject(json_root, "is_link_preview_allowed", g_server_settings.is_link_preview_allowed);
    cJSON_AddItemToObject(json_root, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_private_messaging_allowed", cJSON_CreateBool(g_server_settings.is_private_messaging_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_file_upload_allowed", cJSON_CreateBool(g_server_settings.is_file_upload_allowed == TRUE));
    cJSON_AddNumberToObject(json_root, "file_upload_max_size_bytes", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(json_root, "chat_picture_max_size_bytes", (double)g_server_settings.chat_picture_max_size_bytes);
    // pictures are assumed wanted: on by default and never a setup question, only a settings-tab toggle
    cJSON_AddItemToObject(json_root, "is_chat_picture_allowed", cJSON_CreateBool(g_server_settings.is_chat_picture_allowed == TRUE));
    // country blocking is deliberately NOT a setup question; the admin sets it later in the settings tab
    cJSON_AddItemToObject(json_root, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    cJSON_AddItemToObject(json_root, "blocked_countries", cJSON_CreateArray());
    // the admin log toggles are not setup questions either; all off until the admin turns them on
    cJSON_AddItemToObject(json_root, "is_logging_client_joins_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_joins_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_username_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_username_changes_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_tag_changes_enabled", cJSON_CreateBool(g_server_settings.is_logging_tag_changes_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_server_settings_updates_enabled", cJSON_CreateBool(g_server_settings.is_logging_server_settings_updates_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_kicks_and_bans_enabled", cJSON_CreateBool(g_server_settings.is_logging_kicks_and_bans_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_client_disconnects_enabled", cJSON_CreateBool(g_server_settings.is_logging_client_disconnects_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_socket_opens_and_closes_enabled", cJSON_CreateBool(g_server_settings.is_logging_socket_opens_and_closes_enabled == TRUE));
    cJSON_AddItemToObject(json_root, "is_logging_failed_attempts_enabled", cJSON_CreateBool(g_server_settings.is_logging_failed_attempts_enabled == TRUE));
    cJSON_AddNumberToObject(json_root, "admin_log_max_size_bytes", (double)g_server_settings.admin_log_max_size_bytes);
    cJSON_AddNumberToObject(json_root, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);

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
        cJSON_free(json_text);
    }

    cJSON_Delete(json_root);
}
