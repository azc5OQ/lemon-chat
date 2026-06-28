#include "definitions.h"

/* use forward slashes "/" when specifying paths, not backward slashes "\" linux environment has trouble finding files that way
   windows compiler will work with both */

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" /* needed by base.h */
#include "base.h"

#include "util.h"

/**
 * @brief ment to be used within acquired lock on clients
 *
 * @param int client_id ->
 *
 * client id and client_index are the same in the server's context
 * @return boole
 * */
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