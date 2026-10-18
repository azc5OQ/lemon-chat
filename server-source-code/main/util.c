#include "definitions.h"

// use forward slashes "/" when specifying paths, not backward slashes "\" linux environment has trouble finding files that way
// windows compiler will work with both

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"

#include "util.h"
#include "../third-party/rxi-log/log.h"
#include <string.h>
#include <stdatomic.h>

static _Atomic(uint64) webrtc_received_bytes;
static _Atomic(uint64) webrtc_sent_bytes;

/**
 * @brief Counts WebRTC data-channel bytes received or accepted for sending since startup.
 * @param uint64 received -> bytes received in this call
 * @param uint64 sent -> bytes accepted for sending in this call
 * @return void
 */
void util__count_webrtc_bytes(uint64 received, uint64 sent)
{
    if (received != 0)
    {
        atomic_fetch_add_explicit(&webrtc_received_bytes, received, memory_order_relaxed);
    }
    if (sent != 0)
    {
        atomic_fetch_add_explicit(&webrtc_sent_bytes, sent, memory_order_relaxed);
    }
}

/**
 * @brief Reads WebRTC traffic totals without blocking the threads that relay audio and video.
 * @param uint64* received -> receives the incoming byte total
 * @param uint64* sent -> receives the outgoing byte total
 * @return void
 */
void util__get_webrtc_bytes(uint64* received, uint64* sent)
{
    *received = atomic_load_explicit(&webrtc_received_bytes, memory_order_relaxed);
    *sent = atomic_load_explicit(&webrtc_sent_bytes, memory_order_relaxed);
}

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#if SIZE_MAX <= UINT32_MAX
// Leave room for libraries and thread stacks in a 32-bit process.
#ifndef LEMON_32BIT_RAM_BUDGET_MB
#define LEMON_32BIT_RAM_BUDGET_MB 1536U
#endif
#endif

/**
 * @brief Reads the RAM currently occupied by this server process, including shared pages.
 * @param uint64* bytes -> receives the resident memory size in bytes, or zero on failure
 * @return boole -> TRUE when the operating system supplied a value
 */
boole util__get_process_ram_usage(uint64* bytes)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters = {0};
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
#elif defined(__linux__)
    FILE* file = NULL_POINTER;
    uint64 virtual_pages = 0;
    uint64 resident_pages = 0;
    long page_size = 0;
    int fields = 0;
#endif

    if (bytes == NULL_POINTER)
    {
        return FALSE;
    }
    *bytes = 0;
#ifdef _WIN32
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
        return FALSE;
    }
    *bytes = (uint64)counters.WorkingSetSize;
#elif defined(__APPLE__)
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
    {
        return FALSE;
    }
    *bytes = (uint64)info.resident_size;
#elif defined(__linux__)
    page_size = sysconf(_SC_PAGESIZE);
    file = fopen("/proc/self/statm", "r");
    if (file == NULL_POINTER)
    {
        return FALSE;
    }
    fields = fscanf(file, "%llu %llu", &virtual_pages, &resident_pages);
    fclose(file);
    if (fields != 2 || page_size <= 0 || resident_pages > UINT64_MAX / (uint64)page_size)
    {
        return FALSE;
    }
    *bytes = resident_pages * (uint64)page_size;
#else
    return FALSE;
#endif
    return TRUE;
}

/**
 * @brief Pauses new logins and uploads near the 32-bit build's memory budget.
 * Existing connections and transfers continue. The 64-bit build has no RAM budget.
 * @return boole -> TRUE when the 32-bit budget is reached or usage cannot be read
 */
boole util__is_server_ram_budget_reached(void)
{
#if SIZE_MAX <= UINT32_MAX
    uint64 bytes = 0;

    if (!util__get_process_ram_usage(&bytes))
    {
        return TRUE;
    }
    return bytes / (1024U * 1024U) >= LEMON_32BIT_RAM_BUDGET_MB;
#else
    return FALSE;
#endif
}

/**
 * @brief Checks whether this user chose to block this kind of picture or file and the server allows it.
 * @param client_t* client -> recipient, protected by the clients lock
 * @param cstring receive_type -> direct_chat_picture, direct_chat_file, channel_chat_picture or channel_chat_file
 * @return boole -> TRUE when a new attachment should skip this recipient
 */
boole util__is_attachment_blocked(client_t* client, cstring receive_type)
{
    if (strcmp(receive_type, "direct_chat_picture") == 0)
    {
        return g_server_settings.allow_block_direct_chat_pictures && client->block_direct_chat_pictures;
    }
    if (strcmp(receive_type, "direct_chat_file") == 0)
    {
        return g_server_settings.allow_block_direct_chat_files && client->block_direct_chat_files;
    }
    if (strcmp(receive_type, "channel_chat_picture") == 0)
    {
        return g_server_settings.allow_block_channel_chat_pictures && client->block_channel_chat_pictures;
    }
    if (strcmp(receive_type, "channel_chat_file") == 0)
    {
        return g_server_settings.allow_block_channel_chat_files && client->block_channel_chat_files;
    }
    return FALSE;
}

/**
 * @brief Starts a worker thread that releases its thread resources when it finishes.
 * @param void* (*)(void*) entry -> function run by the worker
 * @param void* argument -> value passed to entry; the caller retains it if starting fails
 * @return boole -> TRUE when started; FALSE when thread setup or creation fails
 * @note No join is needed, even if the worker finishes before this function returns.
 */
boole util__start_detached_thread(void* (*entry)(void*), void* argument)
{
    pthread_attr_t attributes;
    pthread_t thread;
    int result;

    if (pthread_attr_init(&attributes) != 0)
    {
        return FALSE;
    }
    result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (result == 0)
    {
        result = pthread_create(&thread, &attributes, entry, argument);
    }
    pthread_attr_destroy(&attributes);
    if (result != 0)
    {
        DBG_UTIL log_info("%s %d", "detached thread could not be started, error", result);
    }
    return (boole)(result == 0);
}

/**
 * @brief Checks that a client slot exists, is authenticated and has a WebSocket connection.
 * @param int client_id -> index into g_clients_array; client id and client index are the same
 * @return boole -> TRUE when all checks pass; FALSE for an invalid or unused slot
 * @attention The caller holds the clients lock.
 */
boole util__is_client_valid(int client_id)
{
    if (client_id < 0 || (uint64)client_id >= g_server_settings.max_client_count) { return FALSE; }
    boole status0 = (boole)(client_id >= 0 && (uint64)client_id < g_server_settings.max_client_count);
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
    if (client_id < 0 || (uint64)client_id >= g_server_settings.max_client_count) { return FALSE; }
    boole status0 = (boole)(client_id >= 0 && (uint64)client_id < g_server_settings.max_client_count);
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
    if (client_id < 0 || (uint64)client_id >= g_server_settings.max_client_count) { return FALSE; }
    boole status0 = (boole)(client_id >= 0 && (uint64)client_id < g_server_settings.max_client_count);
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
    if (client_id < 0 || (uint64)client_id >= g_server_settings.max_client_count) { return FALSE; }
    boole status0 = (boole)(client_id >= 0 && (uint64)client_id < g_server_settings.max_client_count);
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
    if (client_id < 0 || (uint64)client_id >= g_server_settings.max_client_count) { return FALSE; }
    boole status0 = (boole)(client_id >= 0 && (uint64)client_id < g_server_settings.max_client_count);
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
        DBG_UTIL log_info("%s %s", "country code rejected, not two characters:", json_entry->valuestring);
        return FALSE;
    }

    first = json_entry->valuestring[0];
    second = json_entry->valuestring[1];

    if (first >= 'a' && first <= 'z') { first = (char)(first - 32); }
    if (second >= 'a' && second <= 'z') { second = (char)(second - 32); }

    if (first < 'A' || first > 'Z' || second < 'A' || second > 'Z')
    {
        DBG_UTIL log_info("%s %s", "country code rejected, not two letters:", json_entry->valuestring);
        return FALSE;
    }

    out_code[0] = first;
    out_code[1] = second;
    out_code[2] = 0;
    return TRUE;
}
