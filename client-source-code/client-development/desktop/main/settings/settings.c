#include "platform/memory.h"
#include "crypto/crypto_session.h"
// Shared validation and JSON serialization for local desktop settings.
#include "settings/settings.h"
#include "settings/settings_file.h"
#include "platform/hotkey.h"
#include "network/endpoint.h"
#include <stdlib.h>
#include "clib.h"
#include "cJSON.h"

/**
 * @brief confines theme names to CSS files in the application's themes directory.
 * @param const char* name -> bounded filename; empty selects the classic theme
 * @return boole -> TRUE for a simple ASCII filename without path components
 */
boole desktop_settings__valid_theme(const char *name)
{
    int64 length = clib__string_length(name, DESKTOP_THEME_NAME_CAPACITY);
    int64 index = 0;
    char value = 0;

    // empty name is allowed
    if (length == 0)
    {
        return TRUE;
    }

    // check the file ending
    if (length < 5 || clib__strcmp(name + length - 4, ".css") != 0)
    {
        return FALSE;
    }

    // check every character
    for (index = 0; index < length - 4; index++)
    {
        value = name[index];
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-' || value == '_'))
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief validates a named TCP WebSocket address without retaining credentials.
 * @param const desktop_bookmark_t* bookmark -> bounded name and address
 * @return boole -> TRUE for a nonblank name and a valid endpoint
 */
boole desktop_settings__valid_bookmark(const desktop_bookmark_t *bookmark)
{
    desktop_endpoint_t endpoint = { 0 };
    int64 length = 0;
    int64 index = 0;
    boole named = FALSE;

    // check arguments
    if (!bookmark || clib__string_length(bookmark->address, sizeof(bookmark->address)) < 0)
    {
        return FALSE;
    }

    // check the name length
    length = clib__string_length(bookmark->name, sizeof(bookmark->name));
    if (length <= 0)
    {
        return FALSE;
    }

    // check the name characters
    for (index = 0; index < length; index++)
    {
        if ((ubyte)bookmark->name[index] < 32)
        {
            return FALSE;
        }
        named |= bookmark->name[index] != ' ';
    }

    // check the address
    return named && desktop_endpoint__parse(bookmark->address, &endpoint);
}

/**
 * @brief accepts an executable basename and rejects paths, reserved characters and unterminated data.
 *
 * @param const char* executable -> game executable basename
 *
 * @return boole -> 1 for a valid .exe name; 0 otherwise
 */
boole desktop_settings__valid_game(const char *executable)
{
    uint64 length = 0;
    ubyte value = 0;

    // check arguments
    if (!executable)
    {
        return 0;
    }

    // check every character
    length = 0;
    while (length < DESKTOP_GAME_NAME_CAPACITY && executable[length])
    {
        value = (ubyte)executable[length];
        if (value < 32 || clib__strchr("\\/:*?\"<>|", value))
        {
            return 0;
        }
        length++;
    }

    // check the file ending
    return length > 4 && length < DESKTOP_GAME_NAME_CAPACITY && executable[length - 4] == '.' && clib__tolower(executable[length - 3]) == 'e' && clib__tolower(executable[length - 2]) == 'x' && clib__tolower(executable[length - 1]) == 'e';
}

/**
 * @brief loads saved preferences, leaving the overlay disabled on invalid or missing data.
 * @param desktop_settings_t* settings -> destination settings
 * @return void
 */
void desktop_settings__load(desktop_settings_t *settings)
{
    char *bytes = NULL_POINTER;
    uint64 length = 0;
    cJSON *root = NULL_POINTER;
    const cJSON *game = NULL_POINTER;
    const cJSON *enabled = NULL_POINTER;
    const cJSON *field = NULL_POINTER;
    const cJSON *bookmarks = NULL_POINTER;
    const cJSON *name = NULL_POINTER;
    const cJSON *address = NULL_POINTER;
    desktop_bookmark_t bookmark = { 0 };
    int64 index = 0;

    // set the defaults
    clib__null_memory(settings, sizeof(*settings));
    desktop_chat_format__default(&settings->chat_format);
    settings->key_bits = 2048;
    settings->audio_enabled = 1;
    settings->audio_volume = 100;
    settings->sound_effects = TRUE;
    settings->microphone_gain = 100;
    settings->push_to_talk_key = DESKTOP_HOTKEY_DEFAULT;
    settings->activation_threshold = -45;
    settings->voice_bitrate = 64;
    settings->chat.image_width = 480;
    settings->chat.image_height = 360;
    settings->chat.scale_percent = 100;
    clib__string_copy(settings->address, sizeof(settings->address), "ws://127.0.0.1:3000/");

    // read the settings file
    bytes = calloc(1, DESKTOP_SETTINGS_FILE_CAPACITY);
    if (!bytes || !desktop_settings_file__read(bytes, DESKTOP_SETTINGS_FILE_CAPACITY, &length))
    {
        if (bytes)
        {
            desktop_memory__erase(bytes, DESKTOP_SETTINGS_FILE_CAPACITY);
            free(bytes);
        }
        return;
    }

    // read the JSON
    root = cJSON_ParseWithLengthOpts(bytes, length + 1, NULL, 1);

    // read the chat format
    desktop_chat_format__read(&settings->chat_format, cJSON_GetObjectItemCaseSensitive(root, "chat_format"));

    // read game and overlay
    game = cJSON_GetObjectItemCaseSensitive(root, "game_executable");
    enabled = cJSON_GetObjectItemCaseSensitive(root, "overlay_enabled");
    if (cJSON_IsString(game) && desktop_settings__valid_game(game->valuestring))
    {
        clib__string_copy(settings->game_executable, sizeof(settings->game_executable), game->valuestring);
        settings->overlay_enabled = cJSON_IsTrue(enabled);
    }

    // read the address
    field = cJSON_GetObjectItemCaseSensitive(root, "address");
    if (cJSON_IsString(field))
    {
        clib__string_copy(settings->address, sizeof(settings->address), field->valuestring);
    }

    // read the username
    field = cJSON_GetObjectItemCaseSensitive(root, "username");
    if (cJSON_IsString(field))
    {
        clib__string_copy(settings->username, sizeof(settings->username), field->valuestring);
    }

    // read the theme
    field = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (cJSON_IsString(field) && desktop_settings__valid_theme(field->valuestring))
    {
        clib__string_copy(settings->theme, sizeof(settings->theme), field->valuestring);
    }

    // read the identity
    field = cJSON_GetObjectItemCaseSensitive(root, "identity");
    if (cJSON_IsString(field))
    {
        clib__string_copy(settings->identity, sizeof(settings->identity), field->valuestring);
    }

    // read the server keys
    field = cJSON_GetObjectItemCaseSensitive(root, "server_keys");
    if (cJSON_IsString(field))
    {
        clib__string_copy(settings->server_keys, sizeof(settings->server_keys), field->valuestring);
    }

    // read the key size
    field = cJSON_GetObjectItemCaseSensitive(root, "key_bits");
    if (cJSON_IsNumber(field) && field->valuedouble >= 2048 && field->valuedouble <= 8192 && field->valuedouble == (double)field->valueint && field->valueint % 256 == 0)
    {
        settings->key_bits = field->valueint;
    }

    // read the audio setting
    field = cJSON_GetObjectItemCaseSensitive(root, "audio_enabled");
    if (cJSON_IsBool(field))
    {
        settings->audio_enabled = cJSON_IsTrue(field);
    }

    // read the sound effects
    field = cJSON_GetObjectItemCaseSensitive(root, "sound_effects");
    if (cJSON_IsBool(field))
    {
        settings->sound_effects = cJSON_IsTrue(field);
    }

    // read the audio volume
    field = cJSON_GetObjectItemCaseSensitive(root, "audio_volume");
    if (cJSON_IsNumber(field) && field->valuedouble >= 0 && field->valuedouble <= 100)
    {
        settings->audio_volume = field->valueint;
    }

    // read the microphone gain
    field = cJSON_GetObjectItemCaseSensitive(root, "microphone_gain");
    if (cJSON_IsNumber(field) && field->valuedouble >= 0 && field->valuedouble <= 200)
    {
        settings->microphone_gain = field->valueint;
    }

    // read the push-to-talk key
    field = cJSON_GetObjectItemCaseSensitive(root, "push_to_talk_key");
    if (cJSON_IsNumber(field) && field->valuedouble == field->valueint && desktop_hotkey__index(field->valueint) >= 0)
    {
        settings->push_to_talk_key = field->valueint;
    }

    // read the voice activation
    field = cJSON_GetObjectItemCaseSensitive(root, "voice_activation");
    settings->voice_activation = cJSON_IsTrue(field);

    // read the activation threshold
    field = cJSON_GetObjectItemCaseSensitive(root, "activation_threshold");
    if (cJSON_IsNumber(field) && field->valuedouble >= -60 && field->valuedouble <= -10)
    {
        settings->activation_threshold = field->valueint;
    }

    // find the bookmarks
    bookmarks = cJSON_GetObjectItemCaseSensitive(root, "bookmarks");

    // read the image width
    field = cJSON_GetObjectItemCaseSensitive(root, "chat_image_width");
    if (cJSON_IsNumber(field) && field->valuedouble >= 64 && field->valuedouble <= 1024)
    {
        settings->chat.image_width = field->valueint;
    }

    // read the image height
    field = cJSON_GetObjectItemCaseSensitive(root, "chat_image_height");
    if (cJSON_IsNumber(field) && field->valuedouble >= 64 && field->valuedouble <= 1024)
    {
        settings->chat.image_height = field->valueint;
    }

    // read the chat scale
    field = cJSON_GetObjectItemCaseSensitive(root, "chat_scale_percent");
    if (cJSON_IsNumber(field) && field->valuedouble >= 75 && field->valuedouble <= 150)
    {
        settings->chat.scale_percent = field->valueint / 5 * 5;
    }

    // read the block settings
    settings->chat.block_channel_pictures = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "block_channel_pictures"));
    settings->chat.block_channel_files = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "block_channel_files"));
    settings->chat.block_direct_pictures = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "block_direct_pictures"));
    settings->chat.block_direct_files = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "block_direct_files"));

    // read the input device
    field = cJSON_GetObjectItemCaseSensitive(root, "input_device");
    if (field && (!cJSON_IsString(field) || !clib__string_copy(settings->input_device, sizeof(settings->input_device), field->valuestring)))
    {
        clib__string_copy(settings->input_device, sizeof(settings->input_device), "invalid-saved-device");
    }

    // read the output device
    field = cJSON_GetObjectItemCaseSensitive(root, "output_device");
    if (field && (!cJSON_IsString(field) || !clib__string_copy(settings->output_device, sizeof(settings->output_device), field->valuestring)))
    {
        clib__string_copy(settings->output_device, sizeof(settings->output_device), "invalid-saved-device");
    }

    // read the voice bitrate
    field = cJSON_GetObjectItemCaseSensitive(root, "voice_bitrate");
    if (cJSON_IsNumber(field) && field->valuedouble >= 32 && field->valuedouble <= 128 && field->valuedouble == (double)field->valueint)
    {
        settings->voice_bitrate = field->valueint;
    }

    // read the bookmarks
    for (index = 0; cJSON_IsArray(bookmarks) && index < DESKTOP_BOOKMARK_LIMIT && index < cJSON_GetArraySize(bookmarks); index++)
    {
        field = cJSON_GetArrayItem(bookmarks, (int)index);
        name = cJSON_GetObjectItemCaseSensitive(field, "name");
        address = cJSON_GetObjectItemCaseSensitive(field, "address");
        clib__null_memory(&bookmark, sizeof(bookmark));
        if (cJSON_IsString(name) && cJSON_IsString(address) && clib__string_copy(bookmark.name, sizeof(bookmark.name), name->valuestring) && clib__string_copy(bookmark.address, sizeof(bookmark.address), address->valuestring) && desktop_settings__valid_bookmark(&bookmark))
        {
            settings->bookmarks[settings->bookmark_count++] = bookmark;
        }
    }

    // free JSON and buffer
    crypto_session__free_json(root);
    desktop_memory__erase(bytes, DESKTOP_SETTINGS_FILE_CAPACITY);
    free(bytes);
}

/**
 * @brief validates and serializes preferences before asking the platform to save them.
 * @param const desktop_settings_t* settings -> preferences selected by the user
 * @return boole -> 1 when saved; 0 on invalid input or a write error
 */
boole desktop_settings__save(const desktop_settings_t *settings)
{
    cJSON *root = NULL_POINTER;
    char *bytes = NULL_POINTER;
    boole complete = 0;
    boole saved = 0;
    cJSON *bookmarks = NULL_POINTER;
    cJSON *bookmark = NULL_POINTER;
    int64 index = 0;

    // check the settings
    if (!settings || !desktop_settings__valid_theme(settings->theme) || !clib__memchr(settings->game_executable, 0, sizeof(settings->game_executable)) || !clib__memchr(settings->address, 0, sizeof(settings->address)) || !clib__memchr(settings->username, 0, sizeof(settings->username)) || !clib__memchr(settings->identity, 0, sizeof(settings->identity)) || !clib__memchr(settings->server_keys, 0, sizeof(settings->server_keys)) || !clib__memchr(settings->input_device, 0, sizeof(settings->input_device)) || !clib__memchr(settings->output_device, 0, sizeof(settings->output_device)) || (settings->overlay_enabled && !desktop_settings__valid_game(settings->game_executable)) || settings->bookmark_count < 0 || settings->bookmark_count > DESKTOP_BOOKMARK_LIMIT)
    {
        return 0;
    }

    // check every bookmark
    for (index = 0; index < settings->bookmark_count; index++)
    {
        if (!desktop_settings__valid_bookmark(&settings->bookmarks[index]))
        {
            return FALSE;
        }
    }

    // create the JSON object
    root = cJSON_CreateObject();
    if (!root)
    {
        return 0;
    }

    // add all settings
    complete = cJSON_AddBoolToObject(root, "overlay_enabled", settings->overlay_enabled != 0) && cJSON_AddStringToObject(root, "game_executable", settings->game_executable) && cJSON_AddStringToObject(root, "address", settings->address) && cJSON_AddStringToObject(root, "username", settings->username) && cJSON_AddStringToObject(root, "theme", settings->theme) && cJSON_AddStringToObject(root, "identity", settings->identity) && cJSON_AddStringToObject(root, "server_keys", settings->server_keys) && cJSON_AddNumberToObject(root, "key_bits", settings->key_bits ? settings->key_bits : 2048) && cJSON_AddBoolToObject(root, "audio_enabled", settings->audio_enabled != 0) && cJSON_AddBoolToObject(root, "sound_effects", settings->sound_effects) && cJSON_AddNumberToObject(root, "audio_volume", settings->audio_volume) && cJSON_AddNumberToObject(root, "microphone_gain", settings->microphone_gain) && cJSON_AddNumberToObject(root, "push_to_talk_key", settings->push_to_talk_key) && cJSON_AddBoolToObject(root, "voice_activation", settings->voice_activation != 0) && cJSON_AddNumberToObject(root, "activation_threshold", settings->activation_threshold >= -60 && settings->activation_threshold <= -10 ? settings->activation_threshold : -45) && cJSON_AddNumberToObject(root, "voice_bitrate", settings->voice_bitrate >= 32 && settings->voice_bitrate <= 128 ? settings->voice_bitrate : 64) && cJSON_AddStringToObject(root, "input_device", settings->input_device) && cJSON_AddStringToObject(root, "output_device", settings->output_device) && cJSON_AddNumberToObject(root, "chat_image_width", settings->chat.image_width >= 64 && settings->chat.image_width <= 1024 ? settings->chat.image_width : 480) && cJSON_AddNumberToObject(root, "chat_image_height", settings->chat.image_height >= 64 && settings->chat.image_height <= 1024 ? settings->chat.image_height : 360) &&
               cJSON_AddNumberToObject(root, "chat_scale_percent", settings->chat.scale_percent >= 75 && settings->chat.scale_percent <= 150 ? settings->chat.scale_percent : 100) && cJSON_AddBoolToObject(root, "block_channel_pictures", settings->chat.block_channel_pictures) && cJSON_AddBoolToObject(root, "block_channel_files", settings->chat.block_channel_files) && cJSON_AddBoolToObject(root, "block_direct_pictures", settings->chat.block_direct_pictures) && cJSON_AddBoolToObject(root, "block_direct_files", settings->chat.block_direct_files);

    // add the chat format
    complete = complete && desktop_chat_format__write(&settings->chat_format, cJSON_AddObjectToObject(root, "chat_format"));

    // add the bookmarks
    bookmarks = complete ? cJSON_AddArrayToObject(root, "bookmarks") : NULL;
    complete = complete && bookmarks;
    for (index = 0; complete && index < settings->bookmark_count; index++)
    {
        bookmark = cJSON_CreateObject();
        complete = bookmark && cJSON_AddStringToObject(bookmark, "name", settings->bookmarks[index].name) && cJSON_AddStringToObject(bookmark, "address", settings->bookmarks[index].address) && cJSON_AddItemToArray(bookmarks, bookmark);
        if (!complete)
        {
            crypto_session__free_json(bookmark);
        }
    }

    // build the JSON text
    bytes = complete ? cJSON_PrintUnformatted(root) : NULL;
    crypto_session__free_json(root);

    // write the settings file
    if (bytes)
    {
        saved = desktop_settings_file__write(bytes, clib__strlen(bytes));
        desktop_memory__erase(bytes, clib__strlen(bytes) + 1);
        cJSON_free(bytes);
    }
    return saved;
}
