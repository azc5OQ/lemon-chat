#include "definitions.h"

// use forward slashes "/" when specifying paths, not backward slashes "\" linux environment has trouble finding files that way
// windows compiler will work with both

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"

#include "util.h"

/**
 * @brief ment to be used within acquired lock on clients
 *
 * @param int client_id ->
 *
 * client id and client_index are the same in the server's context
 * @return boole
 */
boole util__is_client_valid(int client_id)
{
    boole status0 = (boole)(client_id >= 0 && client_id < MAX_CLIENTS);
    boole status1 = g_clients_array[client_id].is_existing;
    boole status2 = g_clients_array[client_id].is_authenticated;
    boole status3 = g_clients_array[client_id].p_ws_connection != NULL_POINTER;

    if (status0 == TRUE && status1 == TRUE && status2 == TRUE && status3 == TRUE)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/**
 * @brief tells whether the client id belongs to a usable human client, meaning a real
 *        connected and authenticated client that is not a music bot.
 *
 *        checks the id is inside the clients array range, the slot is existing, the client is
 *        authenticated, the websocket connection pointer is set, and the is_music_bot flag is
 *        FALSE. all five have to hold, otherwise FALSE is returned.
 *
 * @param int client_id -> index into g_clients_array, client id and client index are the same in the server's context
 *
 * @note ment to be used within acquired lock on clients
 *
 * @return boole -> TRUE when the client is valid and is not a music bot, FALSE otherwise
 */
boole util__is_client_valid_and_not_music_bot(int client_id)
{
    boole status0 = (boole)(client_id >= 0 && client_id < MAX_CLIENTS);
    boole status1 = g_clients_array[client_id].is_existing;
    boole status2 = g_clients_array[client_id].is_authenticated;
    boole status3 = g_clients_array[client_id].p_ws_connection != NULL_POINTER;
    boole status4 = g_clients_array[client_id].is_music_bot == FALSE;

    if (status0 == TRUE && status1 == TRUE && status2 == TRUE && status3 == TRUE && status4 == TRUE)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/**
 * @brief tells whether the client id belongs to a valid client that also holds admin rights.
 *
 *        checks the id is inside the clients array range, the slot is existing, the client is
 *        authenticated, the websocket connection pointer is set, and the is_admin flag is set.
 *        all five have to hold, otherwise FALSE is returned.
 *
 * @param int client_id -> index into g_clients_array, client id and client index are the same in the server's context
 *
 * @note ment to be used within acquired lock on clients
 *
 * @return boole -> TRUE when the client is valid and is an admin, FALSE otherwise
 */
boole util__is_client_valid_admin(int client_id)
{
    boole status0 = (boole)(client_id >= 0 && client_id < MAX_CLIENTS);
    boole status1 = g_clients_array[client_id].is_existing;
    boole status2 = g_clients_array[client_id].is_authenticated;
    boole status3 = g_clients_array[client_id].p_ws_connection != NULL_POINTER;
    boole status4 = g_clients_array[client_id].is_admin;

    if (status0 == TRUE && status1 == TRUE && status2 == TRUE && status3 == TRUE && status4 == TRUE)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/**
 * @brief tells whether the client id belongs to an existing music bot client.
 *
 *        checks the id is inside the clients array range, the slot is existing, and the
 *        is_music_bot flag is set. unlike the human client checks this one does not require
 *        authentication or a websocket connection pointer, since a music bot has neither.
 *
 * @param int client_id -> index into g_clients_array, client id and client index are the same in the server's context
 *
 * @note ment to be used within acquired lock on clients
 *
 * @return boole -> TRUE when the slot holds an existing music bot, FALSE otherwise
 */
boole util__is_client_valid_musicbot(int client_id)
{
    boole status0 = (boole)(client_id >= 0 && client_id < MAX_CLIENTS);
    boole status1 = g_clients_array[client_id].is_existing;
    boole status2 = g_clients_array[client_id].is_music_bot;

    if (status0 == TRUE && status1 == TRUE && status2 == TRUE)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/**
 * @brief tells whether the client id belongs to an existing music bot that still has room for
 *        one more song.
 *
 *        checks the id is inside the clients array range, the slot is existing, the is_music_bot
 *        flag is set, and music_bot_client_extension.music_bot_songs_count has not reached
 *        MUSIC_BOT_MAX_FILE_COUNT. all four have to hold, otherwise FALSE is returned.
 *
 * @param int client_id -> index into g_clients_array, client id and client index are the same in the server's context
 *
 * @note ment to be used within acquired lock on clients
 *
 * @return boole -> TRUE when the slot holds an existing music bot with a free song slot, FALSE otherwise
 */
boole util__is_music_bot_and_song_slot_valid(int client_id)
{
    boole status0 = (boole)(client_id >= 0 && client_id < MAX_CLIENTS);
    boole status1 = g_clients_array[client_id].is_existing;
    boole status2 = g_clients_array[client_id].is_music_bot;
    boole status3 = g_clients_array[client_id].music_bot_client_extension.music_bot_songs_count != MUSIC_BOT_MAX_FILE_COUNT;

    if (status0 == TRUE && status1 == TRUE && status2 == TRUE && status3 == TRUE)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}
/**
 * @brief validates one json entry as an iso 3166-1 alpha-2 country code and writes it uppercased
 *
 * @param cJSON* json_entry -> candidate array element (must be a 2-letter string)
 * @param char* out_code -> receives 2 uppercase letters plus terminator (COUNTRY_ISO_CODE_LENGTH bytes)
 *
 * @return boole TRUE when the entry was usable
 */
boole util__normalize_country_code(cJSON* json_entry, char* out_code)
{
    char first = 0;
    char second = 0;

    if (json_entry == NULL_POINTER || cJSON_IsString(json_entry) == FALSE || json_entry->valuestring == NULL_POINTER)
    {
        return FALSE;
    }

    if (clib__utf8_string_length(json_entry->valuestring) != 2)
    {
        return FALSE;
    }

    first = json_entry->valuestring[0];
    second = json_entry->valuestring[1];

    if (first >= 'a' && first <= 'z') { first = (char)(first - 32); }
    if (second >= 'a' && second <= 'z') { second = (char)(second - 32); }

    if (first < 'A' || first > 'Z' || second < 'A' || second > 'Z')
    {
        return FALSE;
    }

    out_code[0] = first;
    out_code[1] = second;
    out_code[2] = 0;
    return TRUE;
}
