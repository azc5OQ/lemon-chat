#include "definitions.h"

#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "memory_manager.h"
#include "server_logs.h"

#include <time.h> // the log lines are stamped with local date and time

// ADMIN_LOG_TEST_FAST build: retention days count as seconds, the daily purge fires every 5s
// and the size cap reads as KB, so the purge behaviour can be watched live
#ifdef ADMIN_LOG_TEST_FAST
#define ADMIN_LOG_RETENTION_UNIT_SECONDS 1
#define ADMIN_LOG_PURGE_INTERVAL_MS 5000
#define ADMIN_LOG_SIZE_DIVISOR 1024
#else
#define ADMIN_LOG_RETENTION_UNIT_SECONDS 86400
#define ADMIN_LOG_PURGE_INTERVAL_MS 86400000
#define ADMIN_LOG_SIZE_DIVISOR 1
#endif

// the admin log. ram only by design - the lines carry ip addresses, so they must never reach
// a file. a linked list, oldest first: capped by admin_log_max_size_bytes (oldest entries fall
// out over it), and entries older than admin_log_retention_days go in the daily purge
typedef struct admin_log_entry_s
{
    struct admin_log_entry_s* next;
    time_t created;
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];
} admin_log_entry_t;

static admin_log_entry_t* g_admin_log_head = NULL_POINTER; // oldest
static admin_log_entry_t* g_admin_log_tail = NULL_POINTER; // newest
static uint64 g_admin_log_total_bytes = 0;
static uint64 g_admin_log_count = 0;
static uint64 g_admin_log_last_purge_ms = 0;
static pthread_mutex_t g_admin_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// drops the oldest entry; caller holds the mutex
static void _server_logs_internal__pop_oldest_locked(void)
{
    admin_log_entry_t* entry = g_admin_log_head;

    if (entry == NULL_POINTER)
    {
        return;
    }

    g_admin_log_head = entry->next;

    if (g_admin_log_head == NULL_POINTER)
    {
        g_admin_log_tail = NULL_POINTER;
    }

    g_admin_log_total_bytes -= sizeof(admin_log_entry_t);
    g_admin_log_count--;
    memorymanager__free((nuint)entry);
}

// keeps the log under the admin's size cap, always leaving at least the newest entry
static void _server_logs_internal__purge_over_cap_locked(void)
{
    uint64 cap = (uint64)g_server_settings.admin_log_max_size_bytes / ADMIN_LOG_SIZE_DIVISOR;

    while (g_admin_log_total_bytes > cap && g_admin_log_count > 1)
    {
        _server_logs_internal__pop_oldest_locked();
    }
}

/**
 * @brief appends one line, stamped with the server's local date and time. the oldest entries
 *        fall out when the log grows over its size cap
 *
 * @param char* text -> the event text, truncated to fit the entry
 *
 * @return void
 */
static void _server_logs_internal__append(char* text)
{
    time_t now = 0;
    struct tm* time_parts = NULL_POINTER;
    admin_log_entry_t* entry = NULL_POINTER;

    if (text == NULL_POINTER || text[0] == 0)
    {
        return;
    }

    entry = (admin_log_entry_t*)memorymanager__allocate(sizeof(admin_log_entry_t), MEMALLOC_ADMIN_LOG);

    if (entry == NULL_POINTER)
    {
        return;
    }

    clib__null_memory(entry, sizeof(admin_log_entry_t));

    now = time(NULL_POINTER);
    entry->created = now;
    time_parts = localtime(&now);

    if (time_parts != NULL_POINTER)
    {
        snprintf(entry->text, ADMIN_LOG_ENTRY_MAX_LENGTH, "%04d-%02d-%02d %02d:%02d:%02d | %s", time_parts->tm_year + 1900, time_parts->tm_mon + 1, time_parts->tm_mday, time_parts->tm_hour, time_parts->tm_min, time_parts->tm_sec, text);
    }
    else
    {
        snprintf(entry->text, ADMIN_LOG_ENTRY_MAX_LENGTH, "%s", text);
    }

    pthread_mutex_lock(&g_admin_log_mutex);

    if (g_admin_log_tail == NULL_POINTER)
    {
        g_admin_log_head = entry;
    }
    else
    {
        g_admin_log_tail->next = entry;
    }

    g_admin_log_tail = entry;
    g_admin_log_total_bytes += sizeof(admin_log_entry_t);
    g_admin_log_count++;

    _server_logs_internal__purge_over_cap_locked();

    pthread_mutex_unlock(&g_admin_log_mutex);
}

/**
 * @brief the daily purge: entries older than the retention period are dropped, and the size cap
 *        is re-applied (the admin may have lowered it). call it from any periodic thread - it
 *        rate-limits itself to one purge per day
 *
 * @return void
 */
void server_logs__purge_tick(void)
{
    uint64 now_ms = base__get_timestamp_ms();
    time_t cutoff = 0;

    if (g_admin_log_last_purge_ms != 0 && now_ms - g_admin_log_last_purge_ms < ADMIN_LOG_PURGE_INTERVAL_MS)
    {
        return;
    }

    g_admin_log_last_purge_ms = now_ms;

    cutoff = time(NULL_POINTER) - (time_t)(g_server_settings.admin_log_retention_days * ADMIN_LOG_RETENTION_UNIT_SECONDS);

    pthread_mutex_lock(&g_admin_log_mutex);

    while (g_admin_log_head != NULL_POINTER && g_admin_log_head->created < cutoff)
    {
        _server_logs_internal__pop_oldest_locked();
    }

    _server_logs_internal__purge_over_cap_locked();

    pthread_mutex_unlock(&g_admin_log_mutex);
}

/**
 * @brief records a completed join (username and ip address), if join logging is on
 *
 * @param client_t* client -> the client that finished joining; his username must be final
 *
 * @return void
 */
void server_logs__client_joined(client_t* client)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_client_joins == FALSE || client == NULL_POINTER)
    {
        return;
    }

    snprintf(text, sizeof(text), "join: %s (ip %s)", client->username, client->ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a fast reconnect: an identity adopted its still-open session instead of joining fresh.
 *        always logged, it is the one visible trace that the feature did its job
 *
 * @param client_t* client -> the resumed session, already carrying the new socket's ip
 *
 * @return void
 */
void server_logs__fast_reconnect(client_t* client)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (client == NULL_POINTER)
    {
        return;
    }

    snprintf(text, sizeof(text), "fast reconnect: %s (ip %s)", client->username, client->ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records WHY the server is about to drop a client (identity takeover, heartbeat timeout),
 *        one line before the plain "disconnect:" entry the teardown writes. same switch as that entry
 *
 * @param client_t* client -> the client being dropped, still intact
 * @param char* reason -> short reason, becomes the line's prefix
 *
 * @return void
 */
void server_logs__client_disconnect_reason(client_t* client, char* reason)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_client_disconnects == FALSE || client == NULL_POINTER || client->is_authenticated == FALSE || client->username[0] == 0)
    {
        return;
    }

    snprintf(text, sizeof(text), "%s: %s (ip %s)", reason, client->username, client->ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a username change (old -> new)
 *
 * @param char* old_username -> the name being replaced
 * @param char* new_username -> the name it becomes
 *
 * @return void
 */
void server_logs__username_changed(char* old_username, char* new_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_username_changes == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "rename: %s -> %s", old_username, new_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records a tag being added to a client
 *
 * @param int tag_id -> id of the added tag
 * @param char* target_username -> who received the tag
 * @param char* admin_username -> who added it
 *
 * @return void
 */
void server_logs__tag_added(int tag_id, char* target_username, char* admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_tag_changes == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "tag %d added to %s by %s", tag_id, target_username, admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records a client being kicked
 *
 * @param char* target_username -> who got kicked
 * @param char* admin_username -> who kicked him
 *
 * @return void
 */
void server_logs__client_kicked(char* target_username, char* admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_kicks_and_bans == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "kick: %s by %s", target_username, admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records a client being banned
 *
 * @param char* target_username -> who got banned
 * @param char* target_ip -> the ip address the ban is placed on
 * @param char* admin_username -> who banned him
 *
 * @return void
 */
void server_logs__client_banned(char* target_username, char* target_ip, char* admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_kicks_and_bans == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "ban: %s (ip %s) by %s", target_username, target_ip, admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records an authenticated client disconnecting. clients that never finished joining
 *        (refused or half-open sockets) are not recorded here - a username only exists once
 *        every refusal gate was passed, so an empty one means the join never completed
 *
 * @param client_t* client -> the disconnecting client, still intact
 *
 * @return void
 */
void server_logs__client_disconnected(client_t* client)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_client_disconnects == FALSE || client == NULL_POINTER || client->is_authenticated == FALSE || client->username[0] == 0)
    {
        return;
    }

    snprintf(text, sizeof(text), "disconnect: %s (ip %s)", client->username, client->ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a refused join attempt (wrong key, banned ip, same ip, ...)
 *
 * @param char* reason -> short reason text
 * @param char* ip_address -> where the attempt came from
 *
 * @return void
 */
void server_logs__join_refused(char* reason, char* ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_failed_attempts == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "join refused: %s (ip %s)", reason, ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a socket opening, before any check has judged it, so a bot that never logs in still leaves a trace
 *
 * @param char* ip_address -> where the socket came from
 *
 * @return void
 */
void server_logs__socket_opened(char* ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_socket_opens_and_closes == FALSE || ip_address == NULL_POINTER)
    {
        return;
    }

    snprintf(text, sizeof(text), "socket opened (ip %s)", ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a socket closing; names the user when the socket had logged in, otherwise says it never did
 *
 * @param char* ip_address -> where the socket came from, NULL when the library no longer knows
 * @param char* username -> the logged-in name, NULL or empty when the socket never got past the handshake
 *
 * @return void
 */
void server_logs__socket_closed(char* ip_address, char* username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];
    char* shown_ip = (ip_address != NULL_POINTER) ? ip_address : "unknown";

    if (g_server_settings.log_socket_opens_and_closes == FALSE)
    {
        return;
    }

    if (username != NULL_POINTER && username[0] != 0)
    {
        snprintf(text, sizeof(text), "socket closed: %s (ip %s)", username, shown_ip);
    }
    else
    {
        snprintf(text, sizeof(text), "socket closed before login (ip %s)", shown_ip);
    }
    _server_logs_internal__append(text);
}

/**
 * @brief records a join refused because the ip resolved to a blocked country
 *
 * @param char* country_iso_code -> the blocked country
 * @param char* ip_address -> where the attempt came from
 *
 * @return void
 */
void server_logs__join_refused_country(char* country_iso_code, char* ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_failed_attempts == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "join refused: country %s is blocked (ip %s)", country_iso_code, ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a wrong admin password attempt
 *
 * @param char* username -> who tried
 * @param char* ip_address -> from where
 *
 * @return void
 */
void server_logs__admin_password_failed(char* username, char* ip_address, char* attempted_password)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (g_server_settings.log_failed_attempts == FALSE)
    {
        return;
    }

    // the attempt is shown so the admin can see what was tried; capped so a long one cannot fill the line
    snprintf(text, sizeof(text), "wrong admin password \"%.64s\" from %s (ip %s)", attempted_password != NULL_POINTER ? attempted_password : "", username, ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records who saved the server settings
 *
 * @param char* admin_username -> who saved
 * @param boole save_succeeded -> outcome of the save
 *
 * @return void
 */
void server_logs__server_settings_updated(char* admin_username, boole save_succeeded)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    if (save_succeeded == FALSE || g_server_settings.log_server_settings_updates == FALSE)
    {
        return;
    }

    snprintf(text, sizeof(text), "server settings updated by %s", admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief empties the admin log and stamps who did it
 *
 * @param char* admin_username -> who cleared the log
 *
 * @return void
 */
void server_logs__cleared_by(char* admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];

    pthread_mutex_lock(&g_admin_log_mutex);

    while (g_admin_log_head != NULL_POINTER)
    {
        _server_logs_internal__pop_oldest_locked();
    }

    pthread_mutex_unlock(&g_admin_log_mutex);

    snprintf(text, sizeof(text), "log cleared by %s", admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief the whole admin log as a cJSON string array, oldest line first.
 *
 * @return cJSON* -> the array; the caller owns it (attach or delete)
 */
cJSON* server_logs__build_json_array(void)
{
    cJSON* json_lines = NULL_POINTER;
    admin_log_entry_t* entry = NULL_POINTER;

    json_lines = cJSON_CreateArray();

    if (json_lines == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    pthread_mutex_lock(&g_admin_log_mutex);

    for (entry = g_admin_log_head; entry != NULL_POINTER; entry = entry->next)
    {
        cJSON_AddItemToArray(json_lines, cJSON_CreateString(entry->text));
    }

    pthread_mutex_unlock(&g_admin_log_mutex);

    return json_lines;
}
