#include "definitions.h"

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"
#include "metadata_crypto.h"
#include "server_logs.h"

#include "../third-party/ITH-sha/sha256.h"

#include "../third-party/libviolet-0.5.4/src/options.h"
#include "../third-party/libviolet-0.5.4/src/utils.h"

#include "../third-party/rxi-log/log.h"

#include "memory_manager.h"
#include "audio_channel.h"
#include "audio_webrtc.h"
#include "audio_websocket.h"
#include "video_websocket.h"
#include "httpserver/http_server.h"
#include "first_time_setup.h"
#include "settings.h"
#include "login_admission.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // SetConsoleMode, for enabling ANSI color on the Windows console
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#endif

// terminal status markers ([*] info, [+] ok, [-] off, [!] warn, [?] prompt) plus the banner color. they
// default to plain ASCII and are upgraded to ANSI-colored versions by _main_internal__init_terminal (called
// once at startup) when stdout is an interactive, color-capable console.
const char* g_mark_info = "[*]";
const char* g_mark_ok = "[+]";
const char* g_mark_off = "[-]";
const char* g_mark_warn = "[!]";
const char* g_mark_ask = "[?]";
static const char* g_color_banner = "";
static const char* g_color_reset = "";

#ifndef WIN32
#include <unistd.h>     // fork, execl, access, readlink, _exit
#include <dirent.h>     // opendir / readdir (Let's Encrypt cert detection)
#include <signal.h>
#include <sys/wait.h>   // waitpid (certbot)
#include <sys/stat.h>   // stat (certificate age for the days-of-validity-left estimate)
#ifdef __linux__
#include <sys/prctl.h>  // PR_SET_PDEATHSIG (stunnel dies with the server); Linux-only, absent on macOS/BSD
#endif
#if defined(__APPLE__)
#include <stdlib.h>       // realpath
#include <mach-o/dyld.h>  // _NSGetExecutablePath (macOS has no /proc/self/exe)
#endif
#endif

static int g_stunnel_pid = 0; // pid of the optional bundled stunnel child, 0 = none
#ifdef WIN32
static HANDLE g_stunnel_job = NULL_POINTER; // job object holding the stunnel child; closing it (at exit, however this process ends) kills the child
#endif
char* g_first_run_admin_password = NULL_POINTER; // plaintext admin password, kept only through this run's startup summary, then wiped
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER; // serializes console log output across threads

// declarations
static void _main_internal__renew_certificate_if_due(void);
static void _main_internal__launch_stunnel(void);
static int64 _main_internal__get_client_index_by_ws_client_pointer(ws_cli_conn_t* p_ws_connection);
static void _main_internal__print_debug_information(void);
static void _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void);
static void _main_internal__print_startup_summary(void);
static void _main_internal__log_lock(bool lock, void* udata);
static void _main_internal__executable_dir(char* out_directory, uint64 out_directory_size);
static void _main_internal__log_handler(juice_log_level_t level, const char* message);
static void _main_internal__init_terminal(void);
static void _main_internal__print_banner(void);
static boole _main_internal__has_connection_timed_out(client_t* client, uint64 now);
#ifdef WIN32
static boole _main_internal__is_plain_domain(const char* domain);
static boole _main_internal__run_and_wait(char* command_line, DWORD timeout_ms, DWORD* out_exit_code);
static void _main_internal__terminate_stale_stunnel(const char* pid_path);
#endif

uint64 g_thread_id0 = 0;
uint64 g_thread_id1 = 0;
uint64 g_thread_id2 = 0;
uint64 g_thread_id3 = 0;

boole g_is_server_running = TRUE;

/**
 * @brief finds which slot of g_clients_array holds a given websocket connection.
 *
 * @param ws_cli_conn_t* p_ws_connection -> websocket connection pointer to look up in the clients array
 *
 * @note takes the clients read lock itself, so it must not be called with that lock already held.
 *
 * @return int64 -> the client's index, or -1 when the pointer is null or matches no connected client
 */
static int64 _main_internal__get_client_index_by_ws_client_pointer(ws_cli_conn_t* p_ws_connection)
{
    uint64 i = 0;
    int64 result = -1;

    if (p_ws_connection == NULL_POINTER)
    {
        return result;
    }

    clib__read_lock(&g_clients_global_rwlock_guard);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].p_ws_connection == p_ws_connection)
        {
            result = i;
            break;
        }
    }

    clib__unlock(&g_clients_global_rwlock_guard);

    return result;
}

/**
 * @brief gets called by an individual websocket thread
 *
 * @param ws_cli_conn_t* client -> websocket client structure
 *
 * @return void
 *
 * @attention onopen is called by different thread everytime
 */
void onopen(ws_cli_conn_t* client)
{
    char* ip_address = NULL_POINTER;
    int64 index = 0;

    if (ws_media_purpose(client) == WS_MEDIA_PURPOSE_AUDIO)
    {
        audio_websocket__onopen(client);
        return;
    }
    if (ws_media_purpose(client) == WS_MEDIA_PURPOSE_VIDEO)
    {
        video_websocket__onopen(client);
        return;
    }

    uint64 started_ms = base__get_timestamp_ms();
    clib__write_lock(&g_clients_global_rwlock_guard);
    uint64 acquired_ms = base__get_timestamp_ms();
    g_server_settings.client_count = g_server_settings.client_count + 1;
    // log_info("%s %d", "client_count , ", g_server_settings.client_count);
    DBG_AUTHENTICATION log_info("%s %p %s", "client connected , ", client, "\n");
    index = base__get_new_index_for_client();

    if (index == -1)
    {
        DBG_AUTHENTICATION log_info("%s", "base__get_new_index_for_client returned -1, closing socket");
        ws_close_client(client);
        goto label_onopen_end;
    }

    ip_address = ws_getaddress(client);
    if (ip_address == NULL_POINTER)
    {
        DBG_AUTHENTICATION log_info("%s", "failed to get ip address of a client");
        ws_close_client(client);
        goto label_onopen_end;
    }

    // every socket, before any check is applied: this is the only log entry a bot leaves
    server_logs__socket_opened(ip_address);

    // drop banned ips right away, before any client slot is set up
    if (base__is_ip_banned(ip_address) == TRUE)
    {
        DBG_AUTHENTICATION log_info("%s", "ip address is banned, closing socket");
        server_logs__join_refused("banned ip", ip_address);
        ws_close_client(client);
        goto label_onopen_end;
    }

    // with same-ip off, a second session from an ip is checked once its identity is proven (a returning
    // client's own earlier session must not block it), so only a second unfinished handshake is refused this early
    if (g_server_settings.is_same_ip_address_allowed == FALSE)
    {
        if (base__is_there_an_unfinished_handshake_from_same_ip_address(ip_address) == TRUE)
        {
            DBG_AUTHENTICATION log_info("%s", "another handshake from this ip is still in progress, closing socket");
            server_logs__join_refused("same ip already connected", ip_address);
            ws_close_client(client);
            goto label_onopen_end;
        }
    }

    DBG_AUTHENTICATION log_info("%s%lld", "[i] onopen : new client id: ", index);

    g_clients_array[index].is_authenticated = FALSE;
    g_clients_array[index].block_direct_chat_pictures = FALSE;
    g_clients_array[index].block_direct_chat_files = FALSE;
    g_clients_array[index].block_channel_chat_pictures = FALSE;
    g_clients_array[index].block_channel_chat_files = FALSE;
    g_clients_array[index].timestamp_connected = base__get_timestamp_ms();
    g_clients_array[index].p_ws_connection = client;
    g_clients_array[index].is_existing = TRUE;
    g_clients_array[index].client_id = index;
    g_clients_array[index].audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    clib__copy_memory(ip_address, g_clients_array[index].ip_address, clib__utf8_string_length(ip_address), 45);

label_onopen_end:
    clib__unlock(&g_clients_global_rwlock_guard);
    DBG_CONNECTION_PROGRESS log_info("[connection] onopen client=%lld wait_ms=%llu work_ms=%llu",
        (long long)index, (unsigned long long)(acquired_ms - started_ms),
        (unsigned long long)(base__get_timestamp_ms() - acquired_ms));
}

/**
 * @brief websocket callback for a closed connection: drops the client count and tears the client
 *        down through base__process_client_disconnect. an unknown socket is ignored.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of the client that disconnected
 *
 * @note holds the clients AND channels write locks for the whole teardown.
 *
 * @return void
 */
void onclose(ws_cli_conn_t* websocket)
{
    uint64 i = 0;
    int64 client_index = -1;

    if (ws_media_purpose(websocket) == WS_MEDIA_PURPOSE_AUDIO)
    {
        audio_websocket__onclose(websocket);
        return;
    }
    if (ws_media_purpose(websocket) == WS_MEDIA_PURPOSE_VIDEO)
    {
        video_websocket__onclose(websocket);
        return;
    }

    uint64 started_ms = base__get_timestamp_ms();
    clib__write_lock(&g_clients_global_rwlock_guard);
    uint64 acquired_ms = base__get_timestamp_ms();
    clib__write_lock(&g_channels_global_rwlock_guard);

    g_server_settings.client_count = g_server_settings.client_count - 1;

    // log_info("%s %d", "client_count , ", g_server_settings.client_count);

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].p_ws_connection == websocket)
        {
            client_index = i;
            break;
        }
    }

    DBG_AUTHENTICATION log_info("%s %d %s", "onclose", client_index, "\n");

    // the socket log needs the address and name before the slot is cleared; a socket that never got
    // a slot (refused at open) still has its address in the library until this callback returns
    if (client_index != -1)
    {
        server_logs__socket_closed(g_clients_array[client_index].ip_address,
            (g_clients_array[client_index].is_authenticated == TRUE) ? g_clients_array[client_index].username : NULL_POINTER);
    }
    else
    {
        server_logs__socket_closed(ws_getaddress(websocket), NULL_POINTER);
    }

    if (client_index == -1)
    {
        goto label_onclose_end;
    }

    base__process_client_disconnect(client_index);

label_onclose_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
    DBG_CONNECTION_PROGRESS log_info("[connection] onclose client=%lld wait_ms=%llu work_ms=%llu",
        (long long)client_index, (unsigned long long)(acquired_ms - started_ms),
        (unsigned long long)(base__get_timestamp_ms() - acquired_ms));
}

/**
 * @brief websocket callback for an incoming frame: decodes and decrypts the payload, then routes it
 *        to the authenticated or the not-yet-authenticated message handler.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of the client that sent the message
 * @param unsigned char* base64_to_process_and_decrypt -> received base64 payload to decode and decrypt
 * @param uint64_t size -> length in bytes of the received payload
 * @param int type -> websocket frame type of the received message
 *
 * @return void
 */
void onmessage(ws_cli_conn_t* websocket, unsigned char* base64_to_process_and_decrypt, uint64_t size, int type)
{
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    int64 client_index = 0;
    char* decrypted_metadata_cstring = 0;

    if (ws_media_purpose(websocket) == WS_MEDIA_PURPOSE_AUDIO)
    {
        audio_websocket__onmessage(websocket, base64_to_process_and_decrypt, size, type);
        return;
    }
    if (ws_media_purpose(websocket) == WS_MEDIA_PURPOSE_VIDEO)
    {
        video_websocket__onmessage(websocket, base64_to_process_and_decrypt, size, type);
        return;
    }

    // These timings report delays without recording the message or its keys.
    uint64 started_ms = base__get_timestamp_ms();
    client_index = _main_internal__get_client_index_by_ws_client_pointer(websocket);
    if (client_index == -1)
    {
        return;
    }

    uint64 found_ms = base__get_timestamp_ms();

    DBG_ONMESSAGE log_info("%s %d %s", "onmessage() : ", client_index, "\n");

    DBG_ONMESSAGE log_info("%s %llu %s", "onmessage received websocket data size is : ", size, "\n");

    if (size > g_server_settings.websocket_message_max_length)
    {
        base__close_websocket_connection(client_index, TRUE);
        // ws_close_client(websocket);
        return;
    }

    if (size == 0)
    {
        base__close_websocket_connection(client_index, TRUE);
        // ws_close_client(websocket);
        return;
    }

    decrypted_metadata_cstring = (char* )(unsigned char* )memorymanager__allocate(size, MEMALLOC_TYPE_DECRYPT);

    if (decrypted_metadata_cstring == NULL_POINTER)
    {
        DBG_ONMESSAGE log_info("%s %d %s", "onmessage decrypted_metadata_cstring is NULL", client_index, "\n");
        return;
    }

    // just a simple readlock, nothing expensive

    clib__read_lock(&g_clients_global_rwlock_guard);

    is_authenticated = g_clients_array[client_index].is_authenticated;
    is_existing = g_clients_array[client_index].is_existing;
    if (!metadata_crypto__decrypt_from_client(&g_clients_array[client_index],
        (const char*)base64_to_process_and_decrypt, (unsigned char*)decrypted_metadata_cstring, size))
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        memorymanager__free((nuint)decrypted_metadata_cstring);
        return;
    }

    clib__unlock(&g_clients_global_rwlock_guard);

    uint64 decrypted_ms = base__get_timestamp_ms();

    if (is_existing == TRUE)
    {
        if (is_authenticated == TRUE)
        {
            base__process_authenticated_client_message(websocket, client_index, decrypted_metadata_cstring);
        }
        else
        {
            base__process_not_authenticated_client_message(websocket, client_index, decrypted_metadata_cstring);
        }
    }

    memorymanager__free((nuint)decrypted_metadata_cstring);
    decrypted_metadata_cstring = 0;
    uint64 finished_ms = base__get_timestamp_ms();
    if (finished_ms - started_ms >= 250)
    {
        DBG_CONNECTION_PROGRESS log_info("[connection] slow request client=%lld bytes=%llu authenticated=%d lookup_ms=%llu decrypt_ms=%llu handler_ms=%llu",
            (long long)client_index, (unsigned long long)size, (int)is_authenticated,
            (unsigned long long)(found_ms - started_ms), (unsigned long long)(decrypted_ms - found_ms),
            (unsigned long long)(finished_ms - decrypted_ms));
    }
}

/**
 * @brief this is function used that is used as an entry point for websocket thread
 *
 * this function calls theldus internal function that handles incoming websocket connections and that takes it from here
 * @return void
 *
 */
void websocket_thread(void)
{
    struct ws_events evs;

    evs.onopen = &onopen;
    evs.onclose = &onclose;
    evs.onmessage = &onmessage;
    if (ws_socket(&evs, g_server_settings.websocket_port, 1, 2000) != 0)
    {
        exit(EXIT_FAILURE);
    }
#ifdef __ANDROID__
    // the wrapper must not report hosting until the listener actually exists
    puts("LEMON_ANDROID_READY");
    fflush(stdout);
#endif
}

/**
 * @brief Checks whether a connection has taken too long to finish signing in or
 * has stopped sending keep-alive messages while that timeout check is enabled.
 *
 * @param client_t* client -> the client slot whose connection is being checked
 * @param uint64 now -> current time in milliseconds from base__get_timestamp_ms
 *
 * @attention The caller must hold the clients lock while reading the connection
 * and settings. Repeat the check after taking the write lock before disconnecting
 * a client, because a keep-alive message or a settings change may have arrived.
 *
 * @return boole -> TRUE when the connection has passed its time limit; FALSE otherwise
 */
static boole _main_internal__has_connection_timed_out(client_t* client, uint64 now)
{
    uint64 maintain_limit_ms = 0;

    if (client->is_existing == FALSE && client->timestamp_connected == 0)
    {
        return FALSE;
    }

    if (client->is_authenticated == TRUE)
    {
        if (g_server_settings.enforce_connection_check == FALSE || client->is_music_bot == TRUE)
        {
            return FALSE;
        }

        // Idle clients send a message every 120 seconds; active clients send one every 10 seconds.
        maintain_limit_ms = (g_server_settings.is_idle_mode_allowed == TRUE) ? 150000 : 80000;
        return client->timestamp_last_maintain_connection_message_received + maintain_limit_ms < now;
    }

    // Waiting for a login slot has its own time limit. Once admitted, the
    // connection gets 60 seconds to finish authentication, as before.
    return client->is_waiting_for_login_slot == FALSE && client->timestamp_connected + 60000 < now;
}

/**
 * @brief Runs the thread that disconnects clients whose login or keep-alive time
 * limit has expired. It reads the current settings on each pass so an administrator
 * can enable or disable keep-alive timeout checks while the server is running.
 *
 * @return void
 */
void websocket_connection_check_thread(void)
{
    uint64 timestamp_now = 0;
    uint64 i = 0;
    int64* marked_client_ids_for_disconnect = 0;
    int64 number_of_marked_clients = 0;
    boole send_websocket_ping = FALSE;
    client_t* client = NULL_POINTER;

    marked_client_ids_for_disconnect = (int64*)memorymanager__allocate(sizeof(int64) * g_server_settings.max_client_count, MEMALLOC_MARKED_CLIENT_INDICES);
    if (marked_client_ids_for_disconnect == NULL_POINTER)
    {
        fprintf(stderr, "Unable to reserve heartbeat storage; reduce server_slots.\n");
        exit(EXIT_FAILURE);
    }


    while (g_is_server_running)
    {
        timestamp_now = base__get_timestamp_ms();

        DBG_CONNECTION_CHECK_THREAD log_info("%s", "websocket_connection_check_thread tick");

        // ws-level liveness, off by default and switched in the settings tab: a client 4 pings
        // behind gets its socket shut down (ws.c send_ping_close) and its reader thread runs onclose
        clib__read_lock(&g_clients_global_rwlock_guard);
        send_websocket_ping = g_server_settings.is_websocket_ping_active;
        clib__unlock(&g_clients_global_rwlock_guard);

        if (send_websocket_ping == TRUE)
        {
            ws_ping(0, 3);
        }

        // clib__null_memory(marked_client_ids_for_disconnect, sizeof(int) * g_server_settings.max_client_count);
        number_of_marked_clients = 0;

        clib__read_lock(&g_clients_global_rwlock_guard);

        for (i = 0; i < g_server_settings.max_client_count; i++)
        {
            if (_main_internal__has_connection_timed_out(&g_clients_array[i], timestamp_now) == TRUE)
            {
                marked_client_ids_for_disconnect[number_of_marked_clients] = i;
                number_of_marked_clients++;
            }
        }

        clib__unlock(&g_clients_global_rwlock_guard);

        if (number_of_marked_clients > 0)
        {
            clib__write_lock(&g_clients_global_rwlock_guard);
            clib__write_lock(&g_channels_global_rwlock_guard);

            for (i = 0; i < number_of_marked_clients; i++)
            {
                client = &g_clients_array[marked_client_ids_for_disconnect[i]];

                // A message, new login, or settings change may have happened while
                // the read lock was released. Only close a connection still overdue.
                if (_main_internal__has_connection_timed_out(client, base__get_timestamp_ms()) == FALSE)
                {
                    continue;
                }

                // the socket itself: without this only the entry went away and the closed connection
                // kept its reader thread and ws slot forever (the ws ping used to reap those)
                server_logs__client_disconnect_reason(client, client->is_authenticated == TRUE ? "heartbeat timeout" : "authentication timeout");
                base__close_websocket_connection(marked_client_ids_for_disconnect[i], FALSE);
                base__process_client_disconnect(marked_client_ids_for_disconnect[i]);
            }

            clib__unlock(&g_channels_global_rwlock_guard);
            clib__unlock(&g_clients_global_rwlock_guard);
        }

        // rate-limited inside to one purge per day
        server_logs__purge_tick();

        // Check again after 15 seconds on every platform.
        sleep(15);
    }
}

#ifndef WIN32
/**
 * @brief resolves the directory of the running executable, so the bundled stunnel
 *        and its generated stunnel.conf can be located next to it
 *
 * @param char* out_directory -> caller buffer that receives the directory path
 * @param uint64 out_directory_size -> size of out_directory in bytes
 *
 * @return void
 */
static void _main_internal__executable_dir(char* out_directory, uint64 out_directory_size)
{
    char* last_slash = 0;
#if defined(__APPLE__)
    uint image_path_size = 0;     // _NSGetExecutablePath takes a uint32_t* == unsigned int* == uint*
    char image_path[4096];        // raw (possibly relative/symlinked) path from the loader
    char resolved_path[4096];     // realpath output; realpath needs a PATH_MAX-sized buffer
#else
    int64 link_length = 0;
#endif

#if defined(__APPLE__)
    // macOS has no /proc; the dynamic loader hands back the executable's path, then realpath
    // canonicalizes it to an absolute path so sibling files (stunnel.conf, the binary) resolve
    image_path_size = (uint)sizeof(image_path);
    if (_NSGetExecutablePath(image_path, &image_path_size) != 0)
    {
        out_directory[0] = 0;
        return;
    }

    if (realpath(image_path, resolved_path) == NULL_POINTER)
    {
        out_directory[0] = 0;
        return;
    }

    snprintf(out_directory, out_directory_size, "%s", resolved_path);
#else
    link_length = readlink("/proc/self/exe", out_directory, out_directory_size - 1);
    if (link_length <= 0)
    {
        out_directory[0] = 0;
        return;
    }

    out_directory[link_length] = 0;
#endif

    last_slash = strrchr(out_directory, '/');
    if (last_slash != NULL_POINTER)
    {
        *last_slash = 0;
    }
    else
    {
        out_directory[0] = 0;
    }
}
#else

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/**
 * @brief resolves the directory of the running executable (windows), so the bundled stunnel and its
 *        generated stunnel.conf can be located next to it
 *
 * @param char* out_directory -> caller buffer that receives the directory path, without a trailing backslash
 * @param uint64 out_directory_size -> size of out_directory in bytes
 *
 * @return void
 */
static void _main_internal__executable_dir(char* out_directory, uint64 out_directory_size)
{
    char* last_separator = 0;
    DWORD length = 0;

    length = GetModuleFileNameA(NULL_POINTER, out_directory, (DWORD)out_directory_size);
    if (length == 0 || length >= out_directory_size)
    {
        out_directory[0] = 0;
        return;
    }

    last_separator = strrchr(out_directory, '\\');
    if (last_separator != NULL_POINTER)
    {
        *last_separator = 0;
    }
    else
    {
        out_directory[0] = 0;
    }
}

/**
 * @brief a domain name made of letters, digits, dots and dashes only: the one thing that may be placed on a command line built for another program
 *
 * @param const char* domain -> the value from the settings
 *
 * @return boole TRUE when every character is allowed and the string is not empty
 */
static boole _main_internal__is_plain_domain(const char* domain)
{
    uint64 i = 0;
    char c = 0;

    if (domain == NULL_POINTER || domain[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; domain[i] != 0; i++)
    {
        c = domain[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
        {
            continue;
        }
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief starts a program with the given command line, inheriting this console, and waits for it
 *
 * @param char* command_line -> the full command line (mutable, as CreateProcess wants it)
 * @param DWORD timeout_ms -> after this long the program is terminated
 * @param DWORD* out_exit_code -> receives the exit code (1 when terminated)
 *
 * @return boole TRUE when the program could be started
 */
static boole _main_internal__run_and_wait(char* command_line, DWORD timeout_ms, DWORD* out_exit_code)
{
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;

    *out_exit_code = 1;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, 0, NULL_POINTER, NULL_POINTER, &startup_info, &process_info) == 0)
    {
        return FALSE;
    }

    if (WaitForSingleObject(process_info.hProcess, timeout_ms) == WAIT_TIMEOUT)
    {
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 5000);
    }

    GetExitCodeProcess(process_info.hProcess, out_exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    return TRUE;
}

/**
 * @brief terminates the stunnel a previous run of this server started, when it is still running
 *        the previous run left its child's pid and creation time in stunnel.pid; only a process that
 *        still has exactly that creation time is the same process (a pid alone gets reused by windows)
 *
 * @param const char* pid_path -> the stunnel.pid file next to the executable
 *
 * @return void
 */
static void _main_internal__terminate_stale_stunnel(const char* pid_path)
{
    FILE* pid_file = 0;
    unsigned long old_pid = 0;
    unsigned long saved_creation_high = 0;
    unsigned long saved_creation_low = 0;
    HANDLE old_process = NULL_POINTER;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;

    pid_file = fopen(pid_path, "r");
    if (pid_file == NULL_POINTER)
    {
        return;
    }
    if (fscanf(pid_file, "%lu %lu %lu", &old_pid, &saved_creation_high, &saved_creation_low) != 3)
    {
        old_pid = 0;
    }
    fclose(pid_file);

    if (old_pid == 0)
    {
        return;
    }

    old_process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, old_pid);
    if (old_process == NULL_POINTER)
    {
        return;
    }

    if (GetProcessTimes(old_process, &creation_time, &exit_time, &kernel_time, &user_time) != 0
        && creation_time.dwHighDateTime == saved_creation_high && creation_time.dwLowDateTime == saved_creation_low)
    {
        printf("%s%lu%s", "terminating the stunnel left by a previous run (pid ", old_pid, ")\n");
        TerminateProcess(old_process, 1);
        WaitForSingleObject(old_process, 3000);
    }

    CloseHandle(old_process);
}
#endif

/**
 * @brief runs "certbot renew" for the configured domain before anything binds port 80, so the same
 *        standalone challenge that issued the certificate can also renew it. certbot checks the
 *        expiry itself and only acts when the certificate has under 30 days of validity left, so on
 *        most startups this returns in about a second without contacting Let's Encrypt at all.
 *
 * @attention failure is never fatal - the server keeps running with the existing certificate
 *
 * @return void
 */
static void _main_internal__renew_certificate_if_due(void)
{
#ifndef WIN32
    pid_t certbot_pid = 0;
    int certbot_status = 0;

    // only certbot-managed certificates can be renewed by certbot; manually entered cert paths
    // outside /etc/letsencrypt/live/ are the operator's own responsibility
    if (g_server_settings.use_stunnel == FALSE
        || g_server_settings.stunnel_domain[0] == 0
        || strncmp(g_server_settings.stunnel_cert_fullchain, "/etc/letsencrypt/live/", 22) != 0)
    {
        return;
    }

    printf("checking whether the https/wss certificate needs renewal (certbot renew)...\n");

    certbot_pid = fork();
    if (certbot_pid == 0)
    {
        // exec certbot directly (no shell) so the stored domain cannot inject commands
        execlp("certbot", "certbot", "renew", "--cert-name", g_server_settings.stunnel_domain, "--non-interactive", (char* )NULL_POINTER);
        _exit(127); // exec failed -> certbot is not installed / not on PATH
    }
    else if (certbot_pid > 0)
    {
        waitpid(certbot_pid, &certbot_status, 0);
    }

    if (WIFEXITED(certbot_status) && WEXITSTATUS(certbot_status) == 127)
    {
        printf("%s %s\n", g_mark_warn, "certbot is not installed (or not on PATH) - skipping the renewal check");
    }
    else if (WIFEXITED(certbot_status) == 0 || WEXITSTATUS(certbot_status) != 0)
    {
        printf("%s %s\n", g_mark_warn, "certbot renew failed (see its output above) - continuing with the existing certificate");
    }
#else
    // windows: certbot is found on PATH (installed with pip, or the certbot installer) and run the same
    // way. only a certificate under a certbot "live" directory (C:\Certbot\live\<domain>\) is its to renew
    char certbot_path[MAX_PATH];
    char command_line[MAX_PATH + 600];
    DWORD found_length = 0;
    DWORD exit_code = 0;

    if (g_server_settings.use_stunnel == FALSE
        || g_server_settings.stunnel_domain[0] == 0
        || (strstr(g_server_settings.stunnel_cert_fullchain, "\\live\\") == NULL_POINTER && strstr(g_server_settings.stunnel_cert_fullchain, "/live/") == NULL_POINTER))
    {
        return;
    }

    if (_main_internal__is_plain_domain(g_server_settings.stunnel_domain) == FALSE)
    {
        printf("%s %s\n", g_mark_warn, "stunnel_domain holds characters other than letters, digits, dots and dashes - skipping the renewal check");
        return;
    }

    clib__null_memory(certbot_path, sizeof(certbot_path));
    found_length = SearchPathA(NULL_POINTER, "certbot", ".exe", (DWORD)sizeof(certbot_path), certbot_path, NULL_POINTER);
    if (found_length == 0 || found_length >= sizeof(certbot_path))
    {
        printf("%s %s\n", g_mark_warn, "certbot is not installed (or not on PATH) - skipping the renewal check. install it with: pip install certbot");
        return;
    }

    printf("checking whether the https/wss certificate needs renewal (certbot renew)...\n");

    clib__null_memory(command_line, sizeof(command_line));
    snprintf(command_line, sizeof(command_line), "\"%s\" renew --cert-name %s --non-interactive", certbot_path, g_server_settings.stunnel_domain);

    if (_main_internal__run_and_wait(command_line, 180000, &exit_code) == FALSE)
    {
        printf("%s %s\n", g_mark_warn, "certbot could not be started - skipping the renewal check");
    }
    else if (exit_code != 0)
    {
        printf("%s %s\n", g_mark_warn, "certbot renew failed (see its output above) - continuing with the existing certificate");
    }
#endif
}

/**
 * @brief if wss is enabled, writes stunnel.conf next to the executable and launches
 *        the bundled stunnel so the server is reachable over wss as well as ws
 *
 * @attention the launched child is set to end with this server (PR_SET_PDEATHSIG)
 *
 * @return void
 */
static void _main_internal__launch_stunnel(void)
{
#ifndef WIN32
    char exe_dir[600];
    char conf_path[700];
    char stunnel_path[700];
    char kill_old_stunnel_command[800];
    FILE* conf_file = 0;
    pid_t stunnel_pid = 0;

    clib__null_memory(exe_dir, sizeof(exe_dir));
    clib__null_memory(conf_path, sizeof(conf_path));
    clib__null_memory(stunnel_path, sizeof(stunnel_path));
    clib__null_memory(kill_old_stunnel_command, sizeof(kill_old_stunnel_command));

    if (g_server_settings.use_stunnel != TRUE)
    {
        return;
    }

    _main_internal__executable_dir(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        snprintf(exe_dir, sizeof(exe_dir), ".");
    }
    snprintf(conf_path, sizeof(conf_path), "%s/stunnel.conf", exe_dir);
    snprintf(stunnel_path, sizeof(stunnel_path), "%s/stunnel", exe_dir);

    conf_file = fopen(conf_path, "w");
    if (conf_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", conf_path, "; stunnel not started\n");
        return;
    }
    fprintf(conf_file, "[chatserver]\n");
    fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.wss_port);
    fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.websocket_port);
    fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
    fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
    // the patched-stunnel option key is still "xforwardedfor"; it injects the renamed X-Stunnel-Client-IP header
    fprintf(conf_file, "# inject the real client IP as the X-Stunnel-Client-IP header\n");
    fprintf(conf_file, "xforwardedfor = yes\n");

    if (g_server_settings.serve_https == TRUE && g_server_settings.serve_client_http == TRUE)
    {
        fprintf(conf_file, "\n[https]\n");
        fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.https_port);
        fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.http_port);
        fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
        fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
        fprintf(conf_file, "xforwardedfor = yes\n");
    }
    fclose(conf_file);

    if (access(stunnel_path, X_OK) != 0)
    {
        printf("%s%s%s", "stunnel binary not found / not executable at ", stunnel_path, "; not started\n");
        return;
    }

    // never launch stunnel without a usable cert + key, or it just crash-loops on startup
    if (access(g_server_settings.stunnel_cert_privkey, R_OK) != 0 || access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0)
    {
        printf("%s%s%s", "wss disabled: TLS certificate not found at ", g_server_settings.stunnel_cert_privkey, "\n");
        printf("%s", "get one (certbot) and restart to enable wss; ws + http keep running normally.\n");
        g_server_settings.use_stunnel = FALSE;
        return;
    }

    // a previous run's stunnel may still hold the wss/https ports (e.g. the server was SIGKILLed so
    // PR_SET_PDEATHSIG never fired); kill the one launched with this same conf, then let the ports free
    snprintf(kill_old_stunnel_command, sizeof(kill_old_stunnel_command), "pkill -f '%s' 2>/dev/null", conf_path);
    system(kill_old_stunnel_command);
    usleep(300000);

    stunnel_pid = fork();
    if (stunnel_pid == 0)
    {
#ifdef __linux__
        // end with the parent server; Linux-only. on macOS/BSD the stale-stunnel pkill above
        // handles a leftover from a hard kill
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        execl(stunnel_path, stunnel_path, conf_path, (char* )NULL_POINTER);
        _exit(127); // exec failed
    }
    else if (stunnel_pid > 0)
    {
        g_stunnel_pid = (int)stunnel_pid;
        printf("%s%d%s%lld%s%lld%s%s%s", "launched stunnel (pid ", g_stunnel_pid, "): wss 0.0.0.0:", g_server_settings.wss_port, " -> ws 127.0.0.1:", g_server_settings.websocket_port, " [domain ", g_server_settings.stunnel_domain, "]\n");
    }
    else
    {
        printf("%s", "fork() failed; stunnel not started\n");
    }
#else
    // windows: the console build tstunnel.exe is preferred (no tray icon, no window), the gui build
    // stunnel.exe otherwise. the child is placed in a job object that kills it when this process
    // ends, however it ends; stunnel.pid lets the next start remove a leftover from a hard kill
    char exe_dir[600];
    char conf_path[700];
    char log_path[700];
    char pid_path[700];
    char stunnel_path[700];
    char command_line[1500];
    FILE* conf_file = 0;
    FILE* pid_file = 0;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    boole is_in_job = FALSE;

    if (g_server_settings.use_stunnel != TRUE)
    {
        return;
    }

    clib__null_memory(exe_dir, sizeof(exe_dir));
    _main_internal__executable_dir(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        snprintf(exe_dir, sizeof(exe_dir), ".");
    }
    snprintf(conf_path, sizeof(conf_path), "%s\\stunnel.conf", exe_dir);
    snprintf(log_path, sizeof(log_path), "%s\\stunnel.log", exe_dir);
    snprintf(pid_path, sizeof(pid_path), "%s\\stunnel.pid", exe_dir);
    snprintf(stunnel_path, sizeof(stunnel_path), "%s\\tstunnel.exe", exe_dir);
    if (access(stunnel_path, 0) != 0)
    {
        snprintf(stunnel_path, sizeof(stunnel_path), "%s\\stunnel.exe", exe_dir);
    }

    if (access(stunnel_path, 0) != 0)
    {
        printf("%s%s%s", "stunnel binary not found at ", stunnel_path, "; not started (the windows build script produces it when Perl is installed)\n");
        return;
    }

    // never launch stunnel without a usable cert + key, or it just crash-loops on startup
    if (access(g_server_settings.stunnel_cert_privkey, R_OK) != 0 || access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0)
    {
        printf("%s%s%s", "wss disabled: TLS certificate not found at ", g_server_settings.stunnel_cert_privkey, "\n");
        printf("%s", "get one (certbot certonly --standalone, or win-acme) and restart to enable wss; ws + http keep running normally.\n");
        g_server_settings.use_stunnel = FALSE;
        return;
    }

    conf_file = fopen(conf_path, "w");
    if (conf_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", conf_path, "; stunnel not started\n");
        return;
    }
    fprintf(conf_file, "; written by the chat server at every start\n");
    fprintf(conf_file, "output = %s\n\n", log_path);
    fprintf(conf_file, "[chatserver]\n");
    fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.wss_port);
    fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.websocket_port);
    fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
    fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
    // the patched-stunnel option key is still "xforwardedfor"; it injects the renamed X-Stunnel-Client-IP header
    fprintf(conf_file, "; inject the real client IP as the X-Stunnel-Client-IP header\n");
    fprintf(conf_file, "xforwardedfor = yes\n");

    if (g_server_settings.serve_https == TRUE && g_server_settings.serve_client_http == TRUE)
    {
        fprintf(conf_file, "\n[https]\n");
        fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.https_port);
        fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.http_port);
        fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
        fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
        fprintf(conf_file, "xforwardedfor = yes\n");
    }
    fclose(conf_file);

    _main_internal__terminate_stale_stunnel(pid_path);

    snprintf(command_line, sizeof(command_line), "\"%s\" \"%s\"", stunnel_path, conf_path);
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, CREATE_NO_WINDOW, NULL_POINTER, exe_dir, &startup_info, &process_info) == 0)
    {
        printf("%s%lu%s", "CreateProcess for stunnel failed (error ", (unsigned long)GetLastError(), "); stunnel not started\n");
        return;
    }

    if (g_stunnel_job == NULL_POINTER)
    {
        g_stunnel_job = CreateJobObjectA(NULL_POINTER, NULL_POINTER);
        if (g_stunnel_job != NULL_POINTER)
        {
            ZeroMemory(&job_limits, sizeof(job_limits));
            job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_stunnel_job, JobObjectExtendedLimitInformation, &job_limits, sizeof(job_limits));
        }
    }
    if (g_stunnel_job != NULL_POINTER && AssignProcessToJobObject(g_stunnel_job, process_info.hProcess) != 0)
    {
        is_in_job = TRUE;
    }

    g_stunnel_pid = (int)process_info.dwProcessId;

    // pid + creation time, so the next start can tell this very process from a reused pid
    pid_file = fopen(pid_path, "w");
    if (pid_file != NULL_POINTER)
    {
        if (GetProcessTimes(process_info.hProcess, &creation_time, &exit_time, &kernel_time, &user_time) != 0)
        {
            fprintf(pid_file, "%lu %lu %lu\n", (unsigned long)process_info.dwProcessId, (unsigned long)creation_time.dwHighDateTime, (unsigned long)creation_time.dwLowDateTime);
        }
        fclose(pid_file);
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    printf("%s%d%s%lld%s%lld%s%s%s", "launched stunnel (pid ", g_stunnel_pid, "): wss 0.0.0.0:", g_server_settings.wss_port, " -> ws 127.0.0.1:", g_server_settings.websocket_port, " [domain ", g_server_settings.stunnel_domain, "]\n");
    if (is_in_job == FALSE)
    {
        printf("%s", "note: stunnel could not be tied to this process; if this server is killed, stunnel keeps running until the next start removes it\n");
    }
#endif
}

/**
 * @brief prints a summary of which services are running and on which ports, after startup is done.
 *        uses the same [+]/[-] markers (green = running, red = off) as the setup prompts.
 *
 * @return void
 */
static void _main_internal__print_startup_summary(void)
{
    boole is_voice_on = g_server_settings.is_voice_chat_active;
    boole is_bot_audio_on = g_server_settings.is_music_bot_audio_active;
    int64 certificate_days_left = -1;
#ifndef WIN32
    struct stat certificate_file_info;

    // estimate the days of validity left on a certbot-managed certificate: Let's Encrypt certs are
    // valid for exactly 90 days from issuance and the fullchain.pem symlink target's modification
    // time is the issuance time. renewal already ran before this summary, so under 30 days here
    // means the renewal did not succeed
    if (g_stunnel_pid > 0
        && strncmp(g_server_settings.stunnel_cert_fullchain, "/etc/letsencrypt/live/", 22) == 0
        && stat(g_server_settings.stunnel_cert_fullchain, &certificate_file_info) == 0)
    {
        certificate_days_left = 90 - (((int64)time(NULL_POINTER) - (int64)certificate_file_info.st_mtime) / 86400);
        if (certificate_days_left < 0)
        {
            certificate_days_left = 0;
        }
    }
#endif

    printf("\n");
    printf("  %s%s%s\n", g_color_banner, "lemon-chat is running - services and ports", g_color_reset);
    printf("\n");

    // the websocket listener is always running
    printf("  %s  %-18s port %lld\n", g_mark_ok, "websocket", g_server_settings.websocket_port);

    // the webrtc datachannel + bundled libviolet STUN/TURN listener (udp 3478) are always up; this is just
    // the transport. whether the server actually forwards audio over it is gated separately for client
    // voice and for music bots, shown on the next two lines
    printf("  %s  %-18s port 3478 (udp)\n", g_mark_ok, "webrtc + stun/turn");

    if (is_voice_on == TRUE)
    {
        printf("  %s  %-18s on\n", g_mark_ok, "client voice relay");
    }
    else
    {
        printf("  %s  %-18s off\n", g_mark_off, "client voice relay");
    }

    if (is_bot_audio_on == TRUE)
    {
        printf("  %s  %-18s on\n", g_mark_ok, "music bot audio");
    }
    else
    {
        printf("  %s  %-18s off\n", g_mark_off, "music bot audio");
    }

    if (g_server_settings.is_video_streaming_active == TRUE)
    {
        printf("  %s  %-18s on\n", g_mark_ok, "video streaming");
    }
    else
    {
        printf("  %s  %-18s off\n", g_mark_off, "video streaming");
    }

    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        printf("  %s  %-18s on, public address %s, udp %lld-%lld forwarded by the router\n", g_mark_ok, "wireguard mode",
            g_server_settings.webrtc_public_address, (long long)g_server_settings.webrtc_port_range_begin, (long long)g_server_settings.webrtc_port_range_end);
    }

    // optional bundled http server that serves the client
    if (g_server_settings.serve_client_http == TRUE)
    {
        printf("  %s  %-18s port %lld\n", g_mark_ok, "http server", g_server_settings.http_port);
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "http server");
    }

    // optional bundled stunnel wss front-end; g_stunnel_pid is only set when it actually launched
    // (it does not launch on windows), so this reflects what is really running
    if (g_stunnel_pid > 0)
    {
        printf("  %s  %-18s port %lld (wss -> ws %lld)\n", g_mark_ok, "stunnel (wss)", g_server_settings.wss_port, g_server_settings.websocket_port);

        if (certificate_days_left >= 0 && certificate_days_left < 30)
        {
            printf("  %s  %-18s about %lld days of validity left, but the renewal on this start did not succeed - check the certbot output above\n", g_mark_warn, "certificate", certificate_days_left);
        }
        else if (certificate_days_left >= 0)
        {
            printf("  %s  %-18s about %lld days of validity left (renews on server start once under 30)\n", g_mark_ok, "certificate", certificate_days_left);
        }
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "stunnel (wss)");
    }

    if (g_server_settings.serve_https == TRUE)
    {
        printf("  %s  %-18s port %lld (https -> http %lld)\n", g_mark_ok, "https (page)", g_server_settings.https_port, g_server_settings.http_port);
    }

    printf("\n");
    printf("  ============================================================\n");
    printf("  %s%s%s\n", g_color_banner, " your chat server is ready", g_color_reset);
    printf("  ============================================================\n");
    if (g_server_settings.use_stunnel == TRUE && g_server_settings.serve_https == TRUE && g_server_settings.stunnel_domain[0] != 0)
    {
        printf("   open it in a browser:   https://%s/\n", g_server_settings.stunnel_domain);
        if (g_first_run_admin_password[0] != 0)
        {
            printf("   admin password:         %s\n", g_first_run_admin_password);
            printf("\n");
            printf("   log in with that password, then change it right away\n");
            printf("   (you will be prompted to change it on the first admin login)\n");
        }
    }
    else
    {
        printf("   running on websocket port %lld\n", g_server_settings.websocket_port);
        printf("   point a client at this machine's address on that port\n");
    }
    printf("  ============================================================\n");

    // do not let the plaintext admin password linger in memory past this one-time summary
    clib__null_memory(g_first_run_admin_password, ADMIN_PASSWORD_MAX_LENGTH);
    free(g_first_run_admin_password);
    g_first_run_admin_password = NULL_POINTER;

    printf("\n");
}

/**
 * @brief log callback that formats and prints a juice/violet log line
 *
 * @param juice_log_level_t level -> severity level of the log message
 * @param const char* message -> the log message text to print
 *
 * @return void
 */
static void _main_internal__log_handler(juice_log_level_t level, const char* message)
{
#ifndef WIN32

    FILE* file = stdout;
    time_t t = time(NULL_POINTER);
    struct tm lt;
    char buffer[32];


    clib__null_memory(buffer, sizeof(buffer));
    if (localtime_r(&t, &lt) == NULL_POINTER || strftime(buffer, 32, "%Y-%m-%d %H:%M:%S", &lt) == 0)
    {
        buffer[0] = '\0';
    }
    fprintf(file, "%s %-7s %s\n", buffer, log_level_to_string(level), message);
    fflush(file);
#endif

    DBG_VIOLET log_info("%s %s %s", "[violet]", message, "\n");
}

/**
 * @brief starts the libviolet STUN/TURN listener thread for the webrtc datachannel
 *
 * @return void
 */
static void _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void)
{
    violet_options_t vopts;
    char* argv[] = { "violet", "--log-level=fatal", 0 };
    juice_server_t* server = NULL_POINTER;

    
    violet_options_init(&vopts);

    // printf("%s", "[important] start_stun_turn_listener_for_webrtc_datachannel started \n");
    // char* argv[] = {
    // "violet",
    // "--credentials=usweger123:pw1wegweg23Q --log-level=verbose",
    // 0
    // };


    // char* argv[] = { "violet", "--log-level=error", 0 };

    // char* argv[] = { "violet", "--log-level=warn", 0 };
    // char* argv[] = { "violet", "--log-level=info", 0 };
    // char* argv[] = { "violet", "--log-level=verbose", 0 };

    if (violet_options_from_arg(2, argv, &vopts) < 0)
    {
        printf("%s", "[important] !violet_options_from_arg error \n");
        goto error;
    }

    juice_set_log_handler(_main_internal__log_handler);
    juice_set_log_level(vopts.log_level);

    vopts.config.port = 3478;

    server = juice_server_create(&vopts.config);
    if (server == NULL_POINTER)
    {
        fprintf(stderr, "Server initialization failed\n");
        goto error;
    }

    // juice_server_destroy(server);
    // violet_options_destroy(&vopts);

error:

    // violet_options_destroy(&vopts);
    return;
}

/**
 * @brief prints one "<name> active" line for every debug category that is switched on at compile time
 *
 *        each DBG_* macro expands to either the following printf or to nothing, so the startup output
 *        lists exactly the debug categories built into this binary.
 *
 * @return void
 */
static void _main_internal__print_debug_information(void)
{
    DBG_DLLMAIN printf("%s", "DBG_DLLMAIN active \n");
    DBG_CLIENT_MESSAGE printf("%s", "DBG_CLIENT_MESSAGE active \n");
    DBG_CLIENT_MESSAGE_MAIN_FUNCTION printf("%s", "DBG_CLIENT_MESSAGE_MAIN_FUNCTION active \n");
    DBG_AUTHENTICATION printf("%s", "DBG_AUTHENTICATION active \n");
    DBG_ENCRYPTION printf("%s", "DBG_ENCRYPTION active \n");
    DBG_SERVER_MESSAGE printf("%s", "DBG_SERVER_MESSAGE active \n");
    DBG_CLOSE_CONNECTION printf("%s", "DBG_CLOSE_CONNECTION active \n");
    DBG_ONMESSAGE printf("%s", "DBG_ONMESSAGE active \n");
    DBG_MEMORY_MANAGER printf("%s", "DBG_MEMORY_MANAGER active \n");
    DBG_CONNECTION_CHECK_THREAD printf("%s", "DBG_CONNECTION_CHECK_THREAD active \n");
    DBG_CLIENT_DISCONNECT printf("%s", "DBG_CLIENT_DISCONNECT active \n");
    DBG_AUDIO_WEBRTC printf("%s", "DBG_AUDIO_WEBRTC active \n");
    DBG_VIOLET printf("%s", "DBG_VIOLET active \n");
    DBG_DBG_MEMORY_ALLOCATIONS printf("%s", "DBG_DBG_MEMORY_ALLOCATIONS active \n");
    DBG_IP_TOOLS printf("%s", "DBG_IP_TOOLS active \n");
    DBG_MUSIC_BOT printf("%s", "DBG_MUSIC_BOT active \n");
    DBG_FILE_UPLOAD printf("%s", "DBG_FILE_UPLOAD active \n");
}

/**
 * @brief detects whether stdout can show ANSI color (enabling it on the Windows console) and, when so,
 *        upgrades the status markers and banner to colored versions
 *
 * @return void
 */
static void _main_internal__init_terminal(void)
{
    boole use_color = FALSE;

#ifdef WIN32
    {
        HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD console_mode = 0;

        // GetConsoleMode only succeeds for a real console (not a redirected file/pipe), which also keeps
        // escape codes out of redirected output. ENABLE_VIRTUAL_TERMINAL_PROCESSING turns on ANSI.
        if (stdout_handle != INVALID_HANDLE_VALUE && GetConsoleMode(stdout_handle, &console_mode) != 0)
        {
            if (SetConsoleMode(stdout_handle, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
            {
                use_color = TRUE;
            }
        }
    }
#else
    use_color = (isatty(1) != 0) ? TRUE : FALSE;
#endif

    if (use_color == TRUE)
    {
        g_mark_info = "\033[1;36m[*]\033[0m";   // cyan
        g_mark_ok = "\033[1;32m[+]\033[0m";     // green
        g_mark_off = "\033[1;31m[-]\033[0m";    // red
        g_mark_warn = "\033[1;33m[!]\033[0m";   // yellow
        g_mark_ask = "\033[1;36m[?]\033[0m";    // cyan
        g_color_banner = "\033[1;33m";          // bold yellow
        g_color_reset = "\033[0m";
    }
}

/**
 * @brief lock callback for the logging library so concurrent threads cannot interleave their log lines
 *        on the console (registered via log_set_lock)
 *
 * @param bool lock -> TRUE to acquire the lock, FALSE to release it
 * @param void* udata -> unused
 *
 * @return void
 */
static void _main_internal__log_lock(bool lock, void* udata)
{
    (void)udata;

    if (lock)
    {
        pthread_mutex_lock(&g_log_mutex);
    }
    else
    {
        pthread_mutex_unlock(&g_log_mutex);
    }
}

/**
 * @brief prints a small startup banner (color when stdout is a color-capable console)
 *
 * @return void
 */
static void _main_internal__print_banner(void)
{
    printf("\n");
    printf("%s%s%s\n", g_color_banner, "  Fresh-squeezed chat - light, private, yours.", g_color_reset);
    printf("\n");
}

/**
 * @brief entry point
 *
 * @return int process exit code
 */
#ifdef __ANDROID__
int lemon_server_main(void)
#else
int main(void)
#endif
{
    char input[50];
    char http_webroot_resolved[512];
    char client_html_path[640];
    FILE* client_html_file = 0;

    base__start_server_uptime();

    // flush logs line-by-line even when stdout/stderr are piped (tee / file);
    // without this, output is block-buffered and only appears in delayed chunks
    setvbuf(stdout, NULL_POINTER, _IOLBF, 0);
    setvbuf(stderr, NULL_POINTER, _IOLBF, 0);

    // serialize log output so threads cannot interleave their lines on the console
    log_set_lock(_main_internal__log_lock, NULL_POINTER);

    _main_internal__init_terminal();
    _main_internal__print_banner();
    DBG_CONNECTION_PROGRESS puts("Connection timing logs enabled; message contents and keys are not logged.");

#ifdef DEBUG_ACTIVE
    printf("%s", "this is debug build \n");
    _main_internal__print_debug_information();
#endif

    // run this so rand() gives random output every time
    srand(time(0));

    clib__rwlock_init(&g_clients_global_rwlock_guard);
    clib__rwlock_init(&g_webrtc_peers_rwlock_guard);
    clib__rwlock_init(&g_channels_global_rwlock_guard);
    clib__rwlock_init(&g_tags_global_rwlock_guard);
    clib__rwlock_init(&g_icons_global_rwlock_guard);
    clib__rwlock_init(&g_bans_global_rwlock_guard);

    if (pthread_mutex_init(&g_chat_message_id_mutex, NULL_POINTER))
    {
        log_info("%s", "pthread_rwlock_init chat_message_id_mutex init failed", 100, "\n");
        exit(0);
    }

    memorymanager__init();

    g_first_run_admin_password = calloc(ADMIN_PASSWORD_MAX_LENGTH, 1);
    if (g_first_run_admin_password == NULL_POINTER) { exit(EXIT_FAILURE); }
    settings__load();
    login_admission__set_limit((uint)g_server_settings.max_concurrent_logins);


    g_clients_array = (client_t*)memorymanager__allocate(sizeof(client_t) * g_server_settings.max_client_count, MEMALLOC_CLIENTS_ARRAY);
    g_channel_array = (channel_t*)memorymanager__allocate(sizeof(channel_t) * g_server_settings.max_channel_count, MEMALLOC_CHANNELS_ARRAY);
    g_client_stored_data = (client_stored_data_t*)memorymanager__allocate(sizeof(client_stored_data_t) * MAX_CLIENT_STORED_DATA, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
    g_icons_array = (icon_t*)memorymanager__allocate(sizeof(icon_t) * MAX_ICONS, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
    g_tags_array = (tag_t*)memorymanager__allocate(sizeof(tag_t) * MAX_TAGS, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
    g_ban_array = (ban_entry_t*)memorymanager__allocate(sizeof(ban_entry_t) * MAX_BANS, MEMALLOC_BANS_ARRAY);


    // offline message queue: ram only, allocated once, never persisted. the payloads themselves are
    // allocated per message, so an empty queue costs only these slot headers
    if (g_server_settings.allow_offline_messages == TRUE)
    {
        g_offline_messages = (offline_chat_message_t*)memorymanager__allocate(sizeof(offline_chat_message_t) * MAX_OFFLINE_MESSAGES, MEMALLOC_OFFLINE_MESSAGES_ARRAY);

    }
    g_webrtc_peers_array = (webrtc_peer_t*)memorymanager__allocate(sizeof(webrtc_peer_t) * g_server_settings.max_client_count, MEMALLOC_WEBRTC_PEERS);
    if (g_clients_array == NULL_POINTER || g_channel_array == NULL_POINTER || g_client_stored_data == NULL_POINTER
        || g_icons_array == NULL_POINTER || g_tags_array == NULL_POINTER || g_ban_array == NULL_POINTER
        || g_webrtc_peers_array == NULL_POINTER
        || (g_server_settings.allow_offline_messages == TRUE && g_offline_messages == NULL_POINTER)
        // reserve chat, audio and video sockets even when media is enabled later at run time
        || ws_init_client_slots((uint32_t)g_server_settings.max_client_count * WEBSOCKET_CONNECTIONS_PER_CLIENT) != 0)
    {
        fprintf(stderr, "Unable to reserve startup arrays. Reduce server_slots in server_settings.json or provide more memory.\n");
        exit(EXIT_FAILURE);
    }

    printf("Audio connections: %s (changeable in the server settings tab). About %llu bytes per client slot; an attached audio websocket adds about 70 KiB and two threads, a video websocket about 530 KiB.\n",
        audio_channel__policy_value(),
        (uint64)(sizeof(client_t) + sizeof(webrtc_peer_t) + ws_client_slot_size() * WEBSOCKET_CONNECTIONS_PER_CLIENT));

    if ((g_server_settings.is_voice_chat_active == TRUE || g_server_settings.is_music_bot_audio_active == TRUE)
        && g_server_settings.is_audio_over_websocket_allowed == FALSE && g_server_settings.is_audio_over_webrtc_allowed == FALSE)
    {
        printf("Warning: audio is enabled but no audio connection is allowed (is_audio_over_websocket_allowed and is_audio_over_webrtc_allowed are both false in server_settings.json); no client can send or receive audio.\n");
    }

    settings__init_channel_list();
    settings__init_tags_and_icons();
    settings__load_persisted_state();

    // resolve the http webroot and warn about a missing client.html now (main thread), so this and the
    // startup summary print before any worker thread is launched and cannot interleave on the console
    if (g_server_settings.serve_client_http == TRUE)
    {
        clib__null_memory(http_webroot_resolved, sizeof(http_webroot_resolved));

        if (g_server_settings.http_webroot[0] != 0)
        {
            clib__copy_memory(g_server_settings.http_webroot, http_webroot_resolved, clib__utf8_string_length(g_server_settings.http_webroot), sizeof(http_webroot_resolved) - 1);
        }
#ifndef WIN32
        else
        {
            _main_internal__executable_dir(http_webroot_resolved, sizeof(http_webroot_resolved));
        }
#endif

        // if nothing resolved (no webroot configured on windows, or the executable-dir lookup failed),
        // serve from the current working directory - the same place server_settings.json lives. without
        // this the served path would be "/client.html", which on windows means the drive root, not the cwd
        if (http_webroot_resolved[0] == 0)
        {
            http_webroot_resolved[0] = '.';
            http_webroot_resolved[1] = 0;
        }

        // warn loudly if client.html is not where the http server will look for it, so the operator does
        // not get a silent 404 in the browser
        clib__null_memory(client_html_path, sizeof(client_html_path));
        snprintf(client_html_path, sizeof(client_html_path), "%s/client.html", http_webroot_resolved);
        client_html_file = fopen(client_html_path, "rb");
        if (client_html_file != NULL_POINTER)
        {
            fclose(client_html_file);
            printf("%s %s%s\n", g_mark_ok, "http server: serving client.html from ", client_html_path);
        }
        else
        {
            printf("%s %s%s%s\n", g_mark_warn, "http server: client.html NOT found at ", client_html_path, " - copy client.html there or the browser will get 404");
        }
    }

    // renew the Let's Encrypt certificate if it is close to expiry. this must run BEFORE stunnel and
    // the http server are started, while port 80 is still free for certbot's standalone challenge -
    // the same window the first issuance used. the /etc/letsencrypt/live/ paths are symlinks to the
    // newest cert, so the stunnel launched right after this automatically picks up a renewed one
    _main_internal__renew_certificate_if_due();

    // launch stunnel (synchronous, main thread; sets g_stunnel_pid) and print the startup summary BEFORE
    // any worker thread is started, so they cannot interleave on the console with thread log output
    _main_internal__launch_stunnel();

    _main_internal__print_startup_summary();

#ifndef DEBUG_ACTIVE
    // Console writes can block network threads on a slow Windows terminal.
    // Release builds show the startup summary, then stop writing runtime logs.
    log_set_quiet(true);
#endif

    pthread_create((pthread_t*)&g_thread_id0, 0, (void*)&websocket_thread, 0);
    pthread_create((pthread_t*)&g_thread_id1, 0, (void*)&websocket_connection_check_thread, 0);

    // http_server__start spawns its own thread and returns immediately (webroot already resolved above)
    if (g_server_settings.serve_client_http == TRUE)
    {
        if (g_server_settings.use_stunnel == TRUE && g_server_settings.serve_https == TRUE)
        {
            http_server__set_https_redirect(TRUE, g_server_settings.https_port);
        }
        http_server__start(g_server_settings.http_port, http_webroot_resolved);
    }

    // the STUN/TURN + webrtc datachannel listener is always started and kept up; is_voice_chat_active
    // only controls whether the server re-transmits audio between clients (gated in audio_channel.c)
    pthread_create((pthread_t*)&g_thread_id2, 0, (void*)&_main_internal__start_stun_turn_listener_for_webrtc_datachannel, 0);

    for (;;)
    {
        clib__null_memory(input, sizeof(input));
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__sanitize_stdin(input);
    }

    log_info("%s", "stdin closed, console input is off");

    for (;;)
    {
        sleep(3600);
    }

    return 0;
}
