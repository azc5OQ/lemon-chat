#include "../util.h"
#include "../definitions.h"

#include "../clib/clib.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../base.h"
#include "../memory_manager.h"
#include "server_logs.h"
#include "../../third-party/rxi-log/log.h"

#include <time.h> // the log lines are stamped with local date and time

// ADMIN_LOG_TEST_FAST build: retention days count as seconds, the daily purge runs every 5s
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
    struct admin_log_entry_s *next;
    time_t created;
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH];
} admin_log_entry_t;

static admin_log_entry_t *g_admin_log_head = NULL_POINTER; // oldest
static admin_log_entry_t *g_admin_log_tail = NULL_POINTER; // newest
static uint64 g_admin_log_total_bytes = 0;
static uint64 g_admin_log_count = 0;
static uint64 g_admin_log_last_purge_ms = 0;
static pthread_mutex_t g_admin_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// declarations
static void _server_logs_internal__pop_oldest_locked(void);
static void _server_logs_internal__purge_over_cap_locked(uint64 cap);
static void _server_logs_internal__append(char *text);

/** @brief Reads live logging policy using the same lock as its writers. */
static boole _server_logs_internal__enabled(const boole *setting)
{
    boole enabled;

    // read the setting
    clib__read_lock(&g_clients_global_rwlock_guard);
    enabled = *setting;
    clib__unlock(&g_clients_global_rwlock_guard);
    return enabled;
}

/** @brief Snapshots limits before taking the log mutex, preserving clients-to-log lock order. */
static uint64 _server_logs_internal__limits(int64 *retention)
{
    uint64 cap;

    // read both limits
    clib__read_lock(&g_clients_global_rwlock_guard);
    cap = (uint64)g_server_settings.admin_log_max_size_bytes / ADMIN_LOG_SIZE_DIVISOR;
    if (retention != NULL_POINTER)
    {
        *retention = g_server_settings.admin_log_retention_days;
    }
    clib__unlock(&g_clients_global_rwlock_guard);
    return cap;
}

/**
 * @brief Removes and frees the oldest admin log entry, if one exists.
 * @return void
 * @attention The caller holds g_admin_log_mutex.
 */
static void _server_logs_internal__pop_oldest_locked(void)
{
    admin_log_entry_t *entry = g_admin_log_head;

    // nothing to remove
    if (entry == NULL_POINTER)
    {
        return;
    }

    // move the list head
    g_admin_log_head = entry->next;

    // clear the list tail
    if (g_admin_log_head == NULL_POINTER)
    {
        g_admin_log_tail = NULL_POINTER;
    }

    // update the counters
    g_admin_log_total_bytes -= sizeof(admin_log_entry_t);
    g_admin_log_count--;

    // free the entry
    memorymanager__free((nuint)entry);
}

/**
 * @brief Removes old entries until the log fits its size cap, keeping at least the newest entry.
 * @return void
 * @attention The caller holds g_admin_log_mutex.
 */
static void _server_logs_internal__purge_over_cap_locked(uint64 cap)
{
    // remove the oldest entries
    while (g_admin_log_total_bytes > cap && g_admin_log_count > 1)
    {
        _server_logs_internal__pop_oldest_locked();
    }
}

/**
 * @brief appends one line, stamped with the server's local date and time. the oldest entries fall
 *        out when the log grows over its size cap
 *
 * @param char* text -> the event text, truncated to fit the entry
 *
 * @return void
 */
static void _server_logs_internal__append(char *text)
{
    time_t now = 0;
    struct tm local_parts;
    struct tm *time_parts = NULL_POINTER;
    uint64 cap = _server_logs_internal__limits(NULL_POINTER);
    admin_log_entry_t *entry = NULL_POINTER;

    // check the text
    if (text == NULL_POINTER || text[0] == 0)
    {
        return;
    }

    // allocate the entry
    entry = (admin_log_entry_t *)memorymanager__allocate(sizeof(admin_log_entry_t), MEMALLOC_ADMIN_LOG);

    // check the allocation
    if (entry == NULL_POINTER)
    {
        return;
    }

    // clear the entry
    clib__null_memory(entry, sizeof(admin_log_entry_t));

    // read the local time
    now = time(NULL_POINTER);
    entry->created = now;
#ifdef _WIN32
    if (localtime_s(&local_parts, &now) == 0)
    {
        time_parts = &local_parts;
    }
#else
    time_parts = localtime_r(&now, &local_parts);
#endif

    // build the entry text
    if (time_parts != NULL_POINTER)
    {
        clib__string_format(entry->text, ADMIN_LOG_ENTRY_MAX_LENGTH, "%04d-%02d-%02d %02d:%02d:%02d | %s", time_parts->tm_year + 1900, time_parts->tm_mon + 1, time_parts->tm_mday, time_parts->tm_hour, time_parts->tm_min, time_parts->tm_sec, text);
    }
    else
    {
        clib__string_format(entry->text, ADMIN_LOG_ENTRY_MAX_LENGTH, "%s", text);
    }

    // lock the log
    pthread_mutex_lock(&g_admin_log_mutex);

    // add to the list
    if (g_admin_log_tail == NULL_POINTER)
    {
        g_admin_log_head = entry;
    }
    else
    {
        g_admin_log_tail->next = entry;
    }

    // update tail and counters
    g_admin_log_tail = entry;
    g_admin_log_total_bytes += sizeof(admin_log_entry_t);
    g_admin_log_count++;

    // log for debugging
    DBG_SERVER_LOGS log_info("%s %llu %s %llu %s", "admin log: entry appended, now", (uint64)g_admin_log_count, "entries,", (uint64)g_admin_log_total_bytes, "bytes");

    // apply the size limit
    _server_logs_internal__purge_over_cap_locked(cap);

    // unlock the log
    pthread_mutex_unlock(&g_admin_log_mutex);
}

/**
 * @brief the daily purge: entries older than the retention period are dropped, and the size cap is
 *        re-applied (the admin may have lowered it). call it from any periodic thread - it
 *        rate-limits itself to one purge per day
 *
 * @return void
 */
void server_logs__purge_tick(void)
{
    uint64 now_ms = util__get_timestamp_ms();
    time_t cutoff = 0;
    uint64 removed_count = 0;
    int64 retention = 0;
    uint64 cap = _server_logs_internal__limits(&retention);

    // lock the log
    pthread_mutex_lock(&g_admin_log_mutex);

    // check the purge interval
    if (g_admin_log_last_purge_ms != 0 && now_ms - g_admin_log_last_purge_ms < ADMIN_LOG_PURGE_INTERVAL_MS)
    {
        pthread_mutex_unlock(&g_admin_log_mutex);
        return;
    }

    // store the purge time
    g_admin_log_last_purge_ms = now_ms;

    // calculate the cutoff time
    cutoff = time(NULL_POINTER) - (time_t)(retention * ADMIN_LOG_RETENTION_UNIT_SECONDS);

    // remove old entries
    while (g_admin_log_head != NULL_POINTER && g_admin_log_head->created < cutoff)
    {
        _server_logs_internal__pop_oldest_locked();
        removed_count++;
    }

    // log for debugging
    DBG_SERVER_LOGS log_info("%s %llu %s", "admin log: purge removed", (uint64)removed_count, "entries older than the retention period");

    // apply the size limit
    _server_logs_internal__purge_over_cap_locked(cap);

    // unlock the log
    pthread_mutex_unlock(&g_admin_log_mutex);
}

/**
 * @brief records a completed join (username and ip address), if join logging is on
 *
 * @param client_t* client -> the client that finished joining; its username must be final
 *
 * @return void
 */
void server_logs__client_joined(client_t *client)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check setting and client
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_client_joins_enabled) == FALSE || client == NULL_POINTER)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "join: %s (ip %s)", client->username, client->ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a fast reconnect: an identity adopted its still-open session instead of joining
 *        fresh. always logged, it is the one visible log entry that shows the feature did its job
 *
 * @param client_t* client -> the resumed session, already carrying the new socket's ip
 *
 * @return void
 */
void server_logs__fast_reconnect(client_t *client)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the client
    if (client == NULL_POINTER)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "fast reconnect: %s (ip %s)", client->username, client->ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records WHY the server is about to drop a client (identity takeover, heartbeat timeout),
 *        one line before the plain "disconnect:" entry the teardown writes. same switch as that
 *        entry
 *
 * @param client_t* client -> the client being dropped, still intact
 * @param char* reason -> short reason, becomes the line's prefix
 *
 * @return void
 */
void server_logs__client_disconnect_reason(client_t *client, char *reason)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check setting and client
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_client_disconnects_enabled) == FALSE || client == NULL_POINTER || client->is_authenticated == FALSE || client->username[0] == 0)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "%s: %s (ip %s)", reason, client->username, client->ip_address);
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
void server_logs__username_changed(char *old_username, char *new_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_username_changes_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "rename: %s -> %s", old_username, new_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records a group being added to a client
 *
 * @param uint64 group_id -> id of the added group
 * @param char* target_username -> who received the group
 * @param char* admin_username -> who added it
 *
 * @return void
 */
void server_logs__group_added(uint64 group_id, char *target_username, char *admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_group_changes_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "group %llu added to %s by %s", group_id, target_username, admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records a client being kicked
 *
 * @param char* target_username -> who got kicked
 * @param char* admin_username -> the admin who kicked the client
 *
 * @return void
 */
void server_logs__client_kicked(char *target_username, char *admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_kicks_and_bans_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "kick: %s by %s", target_username, admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records a client being banned
 *
 * @param char* target_username -> who got banned
 * @param char* target_ip -> the ip address the ban is placed on
 * @param char* admin_username -> the admin who banned the client
 *
 * @return void
 */
void server_logs__client_banned(char *target_username, char *target_ip, char *admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_kicks_and_bans_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "ban: %s (ip %s) by %s", target_username, target_ip, admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief records an authenticated client disconnecting. clients that never finished joining
 *        (refused or half-open sockets) are not recorded here - a username only exists once every
 *        refusal gate was passed, so an empty one means the join never completed
 *
 * @param client_t* client -> the disconnecting client, still intact
 *
 * @return void
 */
void server_logs__client_disconnected(client_t *client)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check setting and client
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_client_disconnects_enabled) == FALSE || client == NULL_POINTER || client->is_authenticated == FALSE || client->username[0] == 0)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "disconnect: %s (ip %s)", client->username, client->ip_address);
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
void server_logs__join_refused(char *reason, char *ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_failed_attempts_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "join refused: %s (ip %s)", reason, ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a socket opening, before any check has been applied, so a bot that never logs in
 *        still leaves a log entry
 *
 * @param char* ip_address -> where the socket came from
 *
 * @return void
 */
void server_logs__socket_opened(char *ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check setting and address
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_socket_opens_and_closes_enabled) == FALSE || ip_address == NULL_POINTER)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "socket opened (ip %s)", ip_address);
    _server_logs_internal__append(text);
}

/**
 * @brief records a socket closing; names the user when the socket had logged in, otherwise says it
 *        never did
 *
 * @param char* ip_address -> where the socket came from, NULL when the library no longer knows
 * @param char* username -> the logged-in name, NULL or empty when the socket never got past the handshake
 *
 * @return void
 */
void server_logs__socket_closed(char *ip_address, char *username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };
    char *shown_ip = (ip_address != NULL_POINTER) ? ip_address : "unknown";

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_socket_opens_and_closes_enabled) == FALSE)
    {
        return;
    }

    // build the log text
    if (username != NULL_POINTER && username[0] != 0)
    {
        clib__string_format(text, sizeof(text), "socket closed: %s (ip %s)", username, shown_ip);
    }
    else
    {
        clib__string_format(text, sizeof(text), "socket closed before login (ip %s)", shown_ip);
    }

    // add the log entry
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
void server_logs__join_refused_country(char *country_iso_code, char *ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_failed_attempts_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "join refused: country %s is blocked (ip %s)", country_iso_code, ip_address);
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
void server_logs__admin_password_failed(char *username, char *ip_address)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check the setting
    if (_server_logs_internal__enabled(&g_server_settings.is_logging_failed_attempts_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "wrong admin password from %s (ip %s)", username, ip_address);
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
void server_logs__server_settings_updated(char *admin_username, boole save_succeeded)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // check result and setting
    if (save_succeeded == FALSE || _server_logs_internal__enabled(&g_server_settings.is_logging_server_settings_updates_enabled) == FALSE)
    {
        return;
    }

    // add the log entry
    clib__string_format(text, sizeof(text), "server settings updated by %s", admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief empties the admin log and stamps who did it
 *
 * @param char* admin_username -> who cleared the log
 *
 * @return void
 */
void server_logs__cleared_by(char *admin_username)
{
    char text[ADMIN_LOG_ENTRY_MAX_LENGTH] = { 0 };

    // lock the log
    pthread_mutex_lock(&g_admin_log_mutex);

    // remove every entry
    while (g_admin_log_head != NULL_POINTER)
    {
        _server_logs_internal__pop_oldest_locked();
    }

    // unlock the log
    pthread_mutex_unlock(&g_admin_log_mutex);

    // add the log entry
    clib__string_format(text, sizeof(text), "log cleared by %s", admin_username);
    _server_logs_internal__append(text);
}

/**
 * @brief the whole admin log as a cJSON string array, oldest line first.
 *
 * @return cJSON* -> the array; the caller owns it (attach or delete)
 */
cJSON *server_logs__build_json_array(void)
{
    cJSON *json_lines = NULL_POINTER;
    admin_log_entry_t *entry = NULL_POINTER;

    // create the JSON array
    json_lines = cJSON_CreateArray();

    // check the array
    if (json_lines == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // lock the log
    pthread_mutex_lock(&g_admin_log_mutex);

    // add every log line
    for (entry = g_admin_log_head; entry != NULL_POINTER; entry = entry->next)
    {
        util__json_add_owned_array(json_lines, cJSON_CreateString(entry->text));
    }

    // unlock the log
    pthread_mutex_unlock(&g_admin_log_mutex);

    return json_lines;
}
