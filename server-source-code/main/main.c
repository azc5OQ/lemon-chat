#include "chat_websocket.h"
#include "core/identity_store.h"
#include "stunnel.h"
#include "util.h"
#include "ip_tools.h"
#include "definitions.h"

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" // needed by base.h
#include "base.h"
#include "metadata_crypto.h"
#include "core/server_logs.h"

#include "../third-party/ITH-sha/sha256.h"

#include "../third-party/libviolet-0.5.4/src/options.h"
#include "../third-party/libviolet-0.5.4/src/utils.h"

#include "../third-party/rxi-log/log.h"

#include "memory_manager.h"
#include "core/audio_channel.h"
#include "audio_webrtc.h"
#include "audio_websocket.h"
#include "video_websocket.h"
#include "httpserver/http_server.h"
#include "first_time_setup.h"
#include "core/server_settings.h"
#include "core/login_admission.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // SetConsoleMode, for enabling ANSI color on the Windows console
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <sys/stat.h>
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

char* g_first_run_admin_password = NULL_POINTER; // plaintext admin password, kept only through this run's startup summary, then wiped
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER; // serializes console log output across threads

// declarations
static void _main_internal__print_startup_summary(void);
static void _main_internal__print_browser_addresses(boole is_stunnel_started);
static void _main_internal__log_handler(juice_log_level_t level, const char* message);
static void* _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void* argument);
static void _main_internal__print_debug_information(void);
static void _main_internal__init_terminal(void);
static void _main_internal__log_lock(bool lock, void* udata);
static void _main_internal__print_banner(void);

/**
 * @brief Prints browser links using the page's configured port and available addresses.
 * @param boole is_stunnel_started -> whether the bundled TLS program was launched
 * @return void
 */
static void _main_internal__print_browser_addresses(boole is_stunnel_started)
{
    char addresses[16][IPV4_ADDRESS_STRING_LENGTH];
    uint count;
    uint index;

    if (g_server_settings.is_client_http_enabled == FALSE)
    {
        printf("  %s  browser page       hosted separately\n", g_mark_info);
        return;
    }
    if (g_server_settings.is_stunnel_enabled == TRUE && g_server_settings.is_https_enabled == TRUE)
    {
        if (is_stunnel_started == FALSE)
        {
            printf("  %s  browser page       HTTPS unavailable (stunnel did not start)\n", g_mark_warn);
        }
        else if (g_server_settings.stunnel_domain[0] == 0)
        {
            printf("  %s  browser page       HTTPS domain not configured\n", g_mark_warn);
        }
        else if (g_server_settings.https_port == 443)
        {
            printf("  %s  open in browser    https://%s/\n", g_mark_info, g_server_settings.stunnel_domain);
        }
        else
        {
            printf("  %s  open in browser    https://%s:%lld/\n", g_mark_info, g_server_settings.stunnel_domain, g_server_settings.https_port);
        }
        return;
    }

    printf("  %s  browser (this PC)  http://127.0.0.1:%lld/\n", g_mark_info, g_server_settings.http_port);
    count = ip_tools__get_local_ipv4_addresses(addresses, sizeof(addresses) / sizeof(addresses[0]));
    for (index = 0; index < count; index++)
    {
        printf("  %s  browser (network)  http://%s:%lld/\n", g_mark_info, addresses[index], g_server_settings.http_port);
    }
    if (g_server_settings.is_external_wss_enabled == TRUE)
    {
        printf("  %s  public page        use your tunnel's HTTPS address\n", g_mark_info);
    }
}

/**
 * @brief prints a summary of which services are running and on which ports, after startup is done.
 *        uses the same [+]/[-] markers (green = running, red = off) as the setup prompts.
 *
 * @return void
 */
static void _main_internal__print_startup_summary(void)
{
    boole is_stunnel_started = stunnel__was_started();
    int64 certificate_days_left = -1;
#ifndef WIN32
    struct stat certificate_file_info;

    // estimate the days of validity left on a certbot-managed certificate: Let's Encrypt certs are
    // valid for exactly 90 days from issuance and the fullchain.pem symlink target's modification
    // time is the issuance time. renewal already ran before this summary, so under 30 days here
    // means the renewal did not succeed
    if (is_stunnel_started == TRUE && strncmp(g_server_settings.stunnel_cert_fullchain, "/etc/letsencrypt/live/", 22) == 0 && stat(g_server_settings.stunnel_cert_fullchain, &certificate_file_info) == 0)
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
    printf("  %s  %-18s port %lld\n", g_mark_ok, "websocket (ws)", g_server_settings.websocket_port);

    // Another program can provide WSS even when the bundled stunnel is disabled.
    if (g_server_settings.is_external_wss_enabled == TRUE)
    {
        printf("  %s  %-18s configured public port %lld (external program)\n", g_mark_info, "websocket (wss)", g_server_settings.wss_port);
    }
    else if (is_stunnel_started == TRUE)
    {
        printf("  %s  %-18s port %lld (stunnel -> ws %lld)\n", g_mark_ok, "websocket (wss)", g_server_settings.wss_port, g_server_settings.websocket_port);
    }
    else if (g_server_settings.is_stunnel_enabled == TRUE)
    {
        printf("  %s  %-18s not running\n", g_mark_warn, "websocket (wss)");
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "websocket (wss)");
    }

    // The bundled STUN/TURN listener uses UDP port 3478 for WebRTC connections.
    printf("  %s  %-18s port 3478 (udp)\n", g_mark_ok, "webrtc + stun/turn");

    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        printf("  %s  %-18s on, public address %s, udp %lld-%lld forwarded by the router\n", g_mark_ok, "wireguard mode", g_server_settings.webrtc_public_address, (long long)g_server_settings.webrtc_port_range_begin, (long long)g_server_settings.webrtc_port_range_end);
    }

    // optional bundled http server that serves the client
    if (g_server_settings.is_client_http_enabled == TRUE)
    {
        printf("  %s  %-18s port %lld\n", g_mark_ok, "http server", g_server_settings.http_port);
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "http server");
    }

    // Report the bundled stunnel program separately from the WSS connection.
    if (is_stunnel_started == TRUE)
    {
        printf("  %s  %-18s started\n", g_mark_ok, "stunnel");

        if (certificate_days_left >= 0 && certificate_days_left < 30)
        {
            printf("  %s  %-18s about %lld days of validity left, but the renewal on this start did not succeed - check the certbot output above\n", g_mark_warn, "certificate", certificate_days_left);
        }
        else if (certificate_days_left >= 0)
        {
            printf("  %s  %-18s about %lld days of validity left (renews on server start once under 30)\n", g_mark_ok, "certificate", certificate_days_left);
        }
    }
    else if (g_server_settings.is_stunnel_enabled == TRUE)
    {
        printf("  %s  %-18s not running\n", g_mark_warn, "stunnel");
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "stunnel");
    }

    if (g_server_settings.is_client_http_enabled == TRUE && g_server_settings.is_stunnel_enabled == TRUE && g_server_settings.is_https_enabled == TRUE)
    {
        if (is_stunnel_started == TRUE)
        {
            printf("  %s  %-18s port %lld (https -> http %lld)\n", g_mark_ok, "https (page)", g_server_settings.https_port, g_server_settings.http_port);
        }
        else
        {
            printf("  %s  %-18s not running\n", g_mark_warn, "https (page)");
        }
    }

    printf("\n");
    printf("  ============================================================\n");
    printf("  %s%s%s\n", g_color_banner, " your chat server is ready", g_color_reset);
    printf("  ============================================================\n");
    _main_internal__print_browser_addresses(is_stunnel_started);
    if (is_stunnel_started == TRUE && g_server_settings.is_client_http_enabled == TRUE && g_server_settings.is_https_enabled == TRUE && g_server_settings.stunnel_domain[0] != 0)
    {
        if (g_first_run_admin_password[0] != 0)
        {
            printf("   admin password:         %s\n", g_first_run_admin_password);
            printf("\n");
            printf("   log in with that password, then change it right away\n");
            printf("   (you will be prompted to change it on the first admin login)\n");
        }
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
 * @brief Starts the STUN/TURN listener used by the WebRTC data channels.
 * @param void* argument -> unused thread argument
 * @return void* -> NULL_POINTER after starting the listener or if startup fails
 */
static void* _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void* argument)
{
    violet_options_t vopts;
    char* argv[] = { "violet", "--log-level=fatal", 0 };
    juice_server_t* server = NULL_POINTER;

    (void)argument;
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
    return NULL_POINTER;
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
        g_mark_info = "\033[1;36m[*]\033[0m"; // cyan
        g_mark_ok = "\033[1;32m[+]\033[0m"; // green
        g_mark_off = "\033[1;31m[-]\033[0m"; // red
        g_mark_warn = "\033[1;33m[!]\033[0m"; // yellow
        g_mark_ask = "\033[1;36m[?]\033[0m"; // cyan
        g_color_banner = "\033[1;33m"; // bold yellow
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
    pthread_t chat_listener_thread;
    pthread_t connection_check_thread;
    pthread_t stun_listener_thread;

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
    if (g_first_run_admin_password == NULL_POINTER)
    {
        exit(EXIT_FAILURE);
    }
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
    if (g_server_settings.is_offline_messaging_allowed == TRUE)
    {
        g_offline_messages = (offline_chat_message_t*)memorymanager__allocate(sizeof(offline_chat_message_t) * MAX_OFFLINE_MESSAGES, MEMALLOC_OFFLINE_MESSAGES_ARRAY);
    }
    g_webrtc_peers_array = (webrtc_peer_t*)memorymanager__allocate(sizeof(webrtc_peer_t) * g_server_settings.max_client_count, MEMALLOC_WEBRTC_PEERS);
    if (g_clients_array == NULL_POINTER || g_channel_array == NULL_POINTER || g_client_stored_data == NULL_POINTER || g_icons_array == NULL_POINTER || g_tags_array == NULL_POINTER || g_ban_array == NULL_POINTER || g_webrtc_peers_array == NULL_POINTER ||
        (g_server_settings.is_offline_messaging_allowed == TRUE && g_offline_messages == NULL_POINTER)
        // reserve chat, audio and video sockets even when media is enabled later at run time
        || ws_init_client_slots((uint32_t)g_server_settings.max_client_count * WEBSOCKET_CONNECTIONS_PER_CLIENT) != 0)
    {
        fprintf(stderr, "Unable to reserve startup arrays. Reduce server_slots in server_settings.json or provide more memory.\n");
        exit(EXIT_FAILURE);
    }

    if ((g_server_settings.is_voice_chat_active == TRUE || g_server_settings.is_music_bot_audio_active == TRUE) && g_server_settings.is_audio_over_websocket_allowed == FALSE && g_server_settings.is_audio_over_webrtc_allowed == FALSE)
    {
        printf("Warning: audio is enabled but no audio connection is allowed (is_audio_over_websocket_allowed and is_audio_over_webrtc_allowed are both false in server_settings.json); no client can send or receive audio.\n");
    }

    settings__init_channel_list();
    settings__init_tags_and_icons();
    settings__load_persisted_state();

    // resolve the http webroot and warn about a missing client.html now (main thread), so this and the
    // startup summary print before any worker thread is launched and cannot interleave on the console
    if (g_server_settings.is_client_http_enabled == TRUE)
    {
        clib__null_memory(http_webroot_resolved, sizeof(http_webroot_resolved));

        if (g_server_settings.http_webroot[0] != 0)
        {
            clib__copy_memory(g_server_settings.http_webroot, http_webroot_resolved, clib__utf8_string_length(g_server_settings.http_webroot), sizeof(http_webroot_resolved) - 1);
        }
#ifndef WIN32
        else
        {
            util__get_executable_directory(http_webroot_resolved, sizeof(http_webroot_resolved));
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
    stunnel__renew_certificate_if_due();

    // launch stunnel (on the main thread) and print the startup summary BEFORE
    // any worker thread is started, so they cannot interleave on the console with thread log output
    stunnel__launch();

    _main_internal__print_startup_summary();

#ifndef DEBUG_ACTIVE
    // Console writes can block network threads on a slow Windows terminal.
    // Release builds show the startup summary, then stop writing runtime logs.
    log_set_quiet(true);
#endif

    pthread_create(&chat_listener_thread, 0, chat_websocket__listen_thread, 0);
    pthread_create(&connection_check_thread, 0, chat_websocket__check_connections_thread, 0);

    // http_server__start spawns its own thread and returns immediately (webroot already resolved above)
    if (g_server_settings.is_client_http_enabled == TRUE)
    {
        if (g_server_settings.is_stunnel_enabled == TRUE && g_server_settings.is_https_enabled == TRUE)
        {
            http_server__set_https_redirect(TRUE, g_server_settings.https_port);
        }
        http_server__start(g_server_settings.http_port, http_webroot_resolved);
    }

    // the STUN/TURN + webrtc datachannel listener is always started and kept up; is_voice_chat_active
    // only controls whether the server re-transmits audio between clients (gated in audio_channel.c)
    pthread_create(&stun_listener_thread, 0, _main_internal__start_stun_turn_listener_for_webrtc_datachannel, 0);

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
