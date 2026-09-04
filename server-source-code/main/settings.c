#include "definitions.h"


#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "../third-party/ITH-sha/sha256.h"
#include "../third-party/rxi-log/log.h"
#include "http/http_server.h"
#include "util.h"
#include "first_time_setup.h"
#include "settings.h"
#include <string.h>

static void _settings_internal__build_and_push_client_config(int64 websocket_port, char plaintext_keys[][256], uint64 keys_count);

/**
 * @brief seeds channel slot 0 as the root channel every client lands in: named "root", audio on,
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
        return;
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
            if (cJSON_IsNumber(json_field) == TRUE) { channel_in_loop->parent_channel_id = (int64)json_field->valuedouble; }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "type");
            if (cJSON_IsNumber(json_field) == TRUE) { channel_in_loop->type = json_field->valueint; }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_root_channel");
            if (cJSON_IsBool(json_field)) { channel_in_loop->is_root_channel = cJSON_IsTrue(json_field); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_using_password");
            if (cJSON_IsBool(json_field)) { channel_in_loop->is_using_password = cJSON_IsTrue(json_field); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_audio_enabled");
            if (cJSON_IsBool(json_field)) { channel_in_loop->is_audio_enabled = cJSON_IsTrue(json_field); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "is_client_limit_active");
            if (cJSON_IsBool(json_field)) { channel_in_loop->is_client_limit_active = cJSON_IsTrue(json_field); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "max_client_count");
            if (cJSON_IsNumber(json_field)) { channel_in_loop->max_client_count = (uint64)json_field->valuedouble; }

            channel_in_loop->has_channel_icon = FALSE;
            channel_in_loop->icon_id = 0;
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "has_channel_icon");
            if (cJSON_IsBool(json_field)) { channel_in_loop->has_channel_icon = cJSON_IsTrue(json_field); }
            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "channel_icon_id");
            if (cJSON_IsNumber(json_field)) { channel_in_loop->icon_id = (uint64)json_field->valuedouble; }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &channel_in_loop->name[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_NAME_MAX_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "password");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &channel_in_loop->password[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_PASSWORD_MAX_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "description");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &channel_in_loop->description[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH - 1); }

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
            clib__null_memory(icon_in_loop, sizeof(icon_t));
            icon_in_loop->id = icon_id;
            icon_in_loop->is_existing = TRUE;

            json_field = cJSON_GetObjectItemCaseSensitive(json_icon, "base64");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &icon_in_loop->base64[0], clib__utf8_string_length(json_field->valuestring), ICON_MAX_LENGTH - 1); }

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
            if (cJSON_IsNumber(json_field) == TRUE) { tag_in_loop->icon_id = (uint64)json_field->valueint; }

            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "has_icon");
            if (cJSON_IsBool(json_field) == TRUE) { tag_in_loop->has_icon = cJSON_IsTrue(json_field); }
            // settings written before has_icon existed had a mandatory icon, but icon id 0 is the seeded
            // admin icon - defaulting those to TRUE hands every iconless tag the admin shield
            else { tag_in_loop->has_icon = (boole)(tag_in_loop->icon_id != 0); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &tag_in_loop->name[0], clib__utf8_string_length(json_field->valuestring), TAG_MAX_NAME_LENGTH - 1); }

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
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER) { clib__copy_memory(json_field->valuestring, &ban_in_loop->country_iso_code[0], clib__utf8_string_length(json_field->valuestring), COUNTRY_ISO_CODE_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "identity");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER) { clib__copy_memory(json_field->valuestring, &ban_in_loop->identity[0], clib__utf8_string_length(json_field->valuestring), MAX_PUBLIC_KEY_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "extra_data");
            if (cJSON_IsString(json_field) && json_field->valuestring != NULL_POINTER) { clib__copy_memory(json_field->valuestring, &ban_in_loop->extra_data[0], clib__utf8_string_length(json_field->valuestring), BAN_EXTRA_DATA_MAX_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_ban, "timestamp_banned");
            if (cJSON_IsNumber(json_field)) { ban_in_loop->timestamp_banned = (uint64)json_field->valuedouble; }

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
    if (cJSON_IsNumber(json_field)) { g_tags_array[ADMIN_TAG_ID].icon_id = (uint64)json_field->valuedouble; }
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_tag_has_icon");
    if (cJSON_IsBool(json_field)) { g_tags_array[ADMIN_TAG_ID].has_icon = cJSON_IsTrue(json_field); }

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
    clib__copy_memory((void*)&base64_icon, (void*)&admin_icon->base64, strlen(base64_icon), ICON_MAX_LENGTH);
}

/**
 * @brief builds the window.__SERVER_CONFIG__ script and hands it to the bundled http server, which injects
 *        it into the client.html it serves so the served page can autoconnect.
 *
 *        the connection details (websocket port, the wss port when stunnel is on, and the plaintext connect
 *        keys) are only baked in when the admin set embed_client_config. the default theme is added when one
 *        is configured. the client policy flags - whether the identity passphrase may be persisted in
 *        localStorage, and whether avatars are allowed plus their accepted max raw size - are always baked
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

    // only bake the connection details (port + keys) into the served page if the admin opted in
    if (g_server_settings.embed_client_config == TRUE)
    {
        cJSON_AddNumberToObject(config_object, "port", (double)websocket_port);
        if (g_server_settings.use_stunnel == TRUE)
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
    // whether avatars are allowed (with the accepted max raw image size). always baked so the client
    // knows the server's stance; both default off when this config is absent (page loaded directly)
    cJSON_AddBoolToObject(config_object, "persist_identity", g_server_settings.persist_identity_in_localstorage == TRUE);
    cJSON_AddBoolToObject(config_object, "allow_avatars", g_server_settings.allow_avatars == TRUE);
    if (g_server_settings.allow_avatars == TRUE)
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

    ITH_SHA256_CTX ctx;
    FILE* settings_file = 0;
    int64 file_length = 0;
    char* file_buffer = NULL_POINTER;
    cJSON* json_root = NULL_POINTER;
    uint64 bytes_read = 0;
    cJSON* json_field = NULL_POINTER;
    cJSON* json_key = NULL_POINTER;
    int64 key_index = 0;

    clib__null_memory(&g_server_settings, sizeof(server_settings_t));
    clib__null_memory(plaintext_keys, sizeof(plaintext_keys));
    g_server_settings.websocket_message_max_length = 5000000;
    g_server_settings.websocket_chat_message_string_max_length = 8000;
    g_server_settings.chat_cooldown_milliseconds = 100;
    g_server_settings.join_channel_request_cooldown_milliseconds = 100;
    g_server_settings.create_channel_request_cooldown_milliseconds = 1000;
    g_server_settings.is_same_ip_address_allowed = TRUE;
    g_server_settings.is_voice_chat_active = TRUE;
    g_server_settings.is_music_bot_audio_active = TRUE;
    g_server_settings.is_hide_clients_in_password_protected_channels_active = TRUE;
    g_server_settings.is_temp_channel_creation_allowed = FALSE;
    g_server_settings.is_restrict_channel_deletion_creation_editing_to_admin_active = FALSE;
    g_server_settings.is_display_country_flags_active = FALSE;
    g_server_settings.hide_admin_country_flag = FALSE;
    g_server_settings.is_display_admin_tag_active = TRUE;
    g_server_settings.is_idle_mode_allowed = TRUE;
    g_server_settings.is_fast_reconnect_allowed = FALSE;
    g_server_settings.is_identity_takeover_allowed = FALSE;
    g_server_settings.is_websocket_ping_active = FALSE;
    g_server_settings.webrtc_datachannel_cooldown_seconds = 600;
    g_server_settings.show_music_bot_marquee_to_everyone = FALSE;
    g_server_settings.is_sending_text_to_idle_clients_allowed = TRUE;
    g_server_settings.allow_private_messages = TRUE;
    g_server_settings.are_identities_enabled = TRUE;
    g_server_settings.persist_identity_in_localstorage = FALSE;
    g_server_settings.allow_avatars = FALSE;
    g_server_settings.allow_alias_registrations = FALSE;
    g_server_settings.allow_stored_clients_list = FALSE;
    g_server_settings.allow_last_seen = FALSE;
    g_server_settings.allow_offline_messages = FALSE;
    g_server_settings.allow_typing_indicator = FALSE;
    g_server_settings.allow_client_renames = TRUE;
    g_server_settings.avatar_max_size_bytes = 51200;  // 50 KB raw image (~68 KB base64, fits MAX_CLIENT_AVATAR_LENGTH)
    g_server_settings.allow_file_uploads = FALSE;
    g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_DEFAULT_SIZE_BYTES;
    g_server_settings.chat_picture_max_size_bytes = PICTURE_DEFAULT_SIZE_BYTES;
    g_server_settings.allow_chat_pictures = TRUE;
    g_server_settings.is_country_blocking_active = FALSE;
    g_server_settings.minimum_rsa_key_bits = 2048;
    g_server_settings.announce_minimum_rsa_key_bits = FALSE;
    g_server_settings.blocked_countries_count = 0;
    g_server_settings.log_client_joins = FALSE;
    g_server_settings.log_username_changes = FALSE;
    g_server_settings.log_tag_changes = FALSE;
    g_server_settings.log_server_settings_updates = FALSE;
    g_server_settings.log_kicks_and_bans = FALSE;
    g_server_settings.log_client_disconnects = FALSE;
    g_server_settings.log_failed_attempts = FALSE;
    g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_DEFAULT_SIZE_BYTES;
    g_server_settings.admin_log_retention_days = ADMIN_LOG_DEFAULT_RETENTION_DAYS;

    // set the max client/channel counts here too; the JSON path below returns early, so without this the arrays would allocate at size 0
    g_server_settings.max_client_count = MAX_CLIENTS;
    g_server_settings.max_channel_count = MAX_CHANNELS;

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
                file_buffer = (char* )malloc(file_length + 1);
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "websocket_port");
                if (cJSON_IsNumber(json_field) == TRUE)
                {
                    g_server_settings.websocket_port = json_field->valueint;
                }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_password");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER))
                {
                    clib__copy_memory(json_field->valuestring, &g_server_settings.admin_password[0], clib__utf8_string_length(json_field->valuestring), 50);
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_password_is_initial");
                if (cJSON_IsBool(json_field)) { g_server_settings.admin_password_is_initial = cJSON_IsTrue(json_field); }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "keys");
                if (cJSON_IsArray(json_field) == TRUE)
                {
                    cJSON_ArrayForEach(json_key, json_field)
                    {
                        if ((key_index < 100) && cJSON_IsString(json_key) && (json_key->valuestring != NULL_POINTER))
                        {
                            ith_sha256_init(&ctx);
                            ith_sha256_update(&ctx, (unsigned char* )json_key->valuestring, strlen(json_key->valuestring));
                            ith_sha256_final(&ctx, g_server_settings.keys[key_index].key_value);
                            clib__copy_memory(json_key->valuestring, &plaintext_keys[key_index][0], clib__utf8_string_length(json_key->valuestring), 255);
                            key_index++;
                        }
                    }
                    g_server_settings.keys_count = key_index;
                }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_voice_chat_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_voice_chat_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_music_bot_audio_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_music_bot_audio_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_same_ip_address_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_same_ip_address_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_display_country_flags_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_display_country_flags_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "hide_admin_country_flag");
                if (cJSON_IsBool(json_field)) { g_server_settings.hide_admin_country_flag = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_hide_clients_in_password_protected_channels_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_hide_clients_in_password_protected_channels_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_temp_channel_creation_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_temp_channel_creation_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_idle_mode_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_idle_mode_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_fast_reconnect_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_fast_reconnect_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_identity_takeover_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_identity_takeover_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_websocket_ping_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_websocket_ping_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "webrtc_datachannel_cooldown_seconds");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.webrtc_datachannel_cooldown_seconds = (int64)json_field->valuedouble;
                    if (g_server_settings.webrtc_datachannel_cooldown_seconds < 0) { g_server_settings.webrtc_datachannel_cooldown_seconds = 0; }
                    if (g_server_settings.webrtc_datachannel_cooldown_seconds > 86400) { g_server_settings.webrtc_datachannel_cooldown_seconds = 86400; }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "show_music_bot_marquee_to_everyone");
                if (cJSON_IsBool(json_field)) { g_server_settings.show_music_bot_marquee_to_everyone = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "restart_on_crash");
                if (cJSON_IsBool(json_field)) { g_server_settings.restart_on_crash = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "are_identities_enabled");
                if (cJSON_IsBool(json_field)) { g_server_settings.are_identities_enabled = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "persist_identity_in_localstorage");
                if (cJSON_IsBool(json_field)) { g_server_settings.persist_identity_in_localstorage = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_avatars");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_avatars = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_alias_registrations");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_alias_registrations = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_stored_clients_list");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_stored_clients_list = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_last_seen");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_last_seen = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_offline_messages");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_offline_messages = cJSON_IsTrue(json_field); }

                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_typing_indicator");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_typing_indicator = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_client_renames");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_client_renames = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "avatar_max_size_bytes");
                if (cJSON_IsNumber(json_field)) { g_server_settings.avatar_max_size_bytes = (int64)json_field->valuedouble; }

                // optional bundled-stunnel front-end (wss)
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "use_stunnel");
                if (cJSON_IsBool(json_field)) { g_server_settings.use_stunnel = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "wss_port");
                if (cJSON_IsNumber(json_field) == TRUE) { g_server_settings.wss_port = json_field->valueint; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_domain");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.stunnel_domain[0], clib__utf8_string_length(json_field->valuestring), 255); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_cert_fullchain");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.stunnel_cert_fullchain[0], clib__utf8_string_length(json_field->valuestring), 511); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "stunnel_cert_privkey");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.stunnel_cert_privkey[0], clib__utf8_string_length(json_field->valuestring), 511); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "client_html_dest");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.client_html_dest[0], clib__utf8_string_length(json_field->valuestring), 511); }

                // optional bundled http server that serves the client (mainly for LAN testing)
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "serve_client_http");
                if (cJSON_IsBool(json_field)) { g_server_settings.serve_client_http = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_port");
                if (cJSON_IsNumber(json_field) == TRUE) { g_server_settings.http_port = json_field->valueint; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "serve_https");
                if (cJSON_IsBool(json_field)) { g_server_settings.serve_https = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "https_port");
                if (cJSON_IsNumber(json_field) == TRUE) { g_server_settings.https_port = json_field->valueint; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_webroot");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.http_webroot[0], clib__utf8_string_length(json_field->valuestring), 511); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "default_theme");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.default_theme[0], clib__utf8_string_length(json_field->valuestring), 31); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "embed_client_config");
                if (cJSON_IsBool(json_field)) { g_server_settings.embed_client_config = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_sending_text_to_idle_clients_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_sending_text_to_idle_clients_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_private_messages");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_private_messages = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_file_uploads");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_file_uploads = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "file_upload_max_size_bytes");
                if (cJSON_IsNumber(json_field)) { g_server_settings.file_upload_max_size_bytes = (int64)json_field->valuedouble; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "minimum_rsa_key_bits");
                if (cJSON_IsNumber(json_field))
                {
                    g_server_settings.minimum_rsa_key_bits = (int64)json_field->valuedouble;
                    if (g_server_settings.minimum_rsa_key_bits < 2048) { g_server_settings.minimum_rsa_key_bits = 2048; }
                    if (g_server_settings.minimum_rsa_key_bits > 8192) { g_server_settings.minimum_rsa_key_bits = 8192; }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "announce_minimum_rsa_key_bits");
                if (cJSON_IsBool(json_field)) { g_server_settings.announce_minimum_rsa_key_bits = cJSON_IsTrue(json_field); }
                if (g_server_settings.file_upload_max_size_bytes < FILE_UPLOAD_MIN_SIZE_BYTES) { g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MIN_SIZE_BYTES; }
                if (g_server_settings.file_upload_max_size_bytes > FILE_UPLOAD_MAX_SIZE_BYTES) { g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MAX_SIZE_BYTES; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "chat_picture_max_size_bytes");
                if (cJSON_IsNumber(json_field)) { g_server_settings.chat_picture_max_size_bytes = (int64)json_field->valuedouble; }
                if (g_server_settings.chat_picture_max_size_bytes < PICTURE_MIN_SIZE_BYTES) { g_server_settings.chat_picture_max_size_bytes = PICTURE_MIN_SIZE_BYTES; }
                if (g_server_settings.chat_picture_max_size_bytes > PICTURE_MAX_SIZE_BYTES) { g_server_settings.chat_picture_max_size_bytes = PICTURE_MAX_SIZE_BYTES; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "allow_chat_pictures");
                if (cJSON_IsBool(json_field)) { g_server_settings.allow_chat_pictures = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_country_blocking_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_country_blocking_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "blocked_countries");
                if (cJSON_IsArray(json_field))
                {
                    g_server_settings.blocked_countries_count = 0;
                    cJSON_ArrayForEach(json_key, json_field)
                    {
                        if (g_server_settings.blocked_countries_count >= MAX_BLOCKED_COUNTRIES) { break; }
                        if (util__normalize_country_code(json_key, &g_server_settings.blocked_countries[g_server_settings.blocked_countries_count][0]) == TRUE)
                        {
                            g_server_settings.blocked_countries_count++;
                        }
                    }
                }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_client_joins");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_client_joins = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_username_changes");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_username_changes = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_tag_changes");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_tag_changes = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_server_settings_updates");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_server_settings_updates = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_kicks_and_bans");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_kicks_and_bans = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_client_disconnects");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_client_disconnects = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "log_failed_attempts");
                if (cJSON_IsBool(json_field)) { g_server_settings.log_failed_attempts = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_log_max_size_bytes");
                if (cJSON_IsNumber(json_field)) { g_server_settings.admin_log_max_size_bytes = (int64)json_field->valuedouble; }
                if (g_server_settings.admin_log_max_size_bytes < ADMIN_LOG_MIN_SIZE_BYTES) { g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MIN_SIZE_BYTES; }
                if (g_server_settings.admin_log_max_size_bytes > ADMIN_LOG_MAX_SIZE_BYTES) { g_server_settings.admin_log_max_size_bytes = ADMIN_LOG_MAX_SIZE_BYTES; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "admin_log_retention_days");
                if (cJSON_IsNumber(json_field)) { g_server_settings.admin_log_retention_days = (int64)json_field->valuedouble; }
                if (g_server_settings.admin_log_retention_days < 1) { g_server_settings.admin_log_retention_days = 1; }
                if (g_server_settings.admin_log_retention_days > ADMIN_LOG_MAX_RETENTION_DAYS) { g_server_settings.admin_log_retention_days = ADMIN_LOG_MAX_RETENTION_DAYS; }

                _settings_internal__build_and_push_client_config(g_server_settings.websocket_port, plaintext_keys, g_server_settings.keys_count);

                cJSON_Delete(json_root);

                printf("%s%lld%s%llu%s%llu%s", "loaded settings from server_settings.json (websocket_port=", g_server_settings.websocket_port, ", keys_count=", g_server_settings.keys_count, ", max_clients=", g_server_settings.max_client_count, ")\n");
                return;
            }

            printf("%s", "server_settings.json found but could not be parsed; using interactive setup\n");
        }
    }

    first_time_setup__run(plaintext_keys);
    _settings_internal__build_and_push_client_config(g_server_settings.websocket_port, plaintext_keys, g_server_settings.keys_count);
}
