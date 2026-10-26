// This file accepts chat WebSocket connections, receives and decrypts messages,
// and passes them to base.c to choose the request handler. It also checks connection
// timeouts. client_session.c manages the client's record, which can stay in use
// when a reconnect replaces the WebSocket connection.
#include "chat_websocket.h"
#include "tailscale.h"

#include "core/client_session.h"
#include "core/administration.h"
#include "definitions.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "../third-party/rxi-log/log.h"
#include "../third-party/eteran-cvector/cvector.h"
#include "base.h"

#include "memory_manager.h"
#include "clib/clib_memory.h"
#include "clib/clib_string.h"
#include "util.h"
#include "metadata_crypto.h"
#include "audio_websocket.h"
#include "video_websocket.h"
#include "core/server_logs.h"
#include "ip_tools.h"

// declarations
static int64 _chat_websocket_internal__get_client_index(ws_cli_conn_t* p_ws_connection);
static void _chat_websocket_internal__onopen(ws_cli_conn_t* client);
static void _chat_websocket_internal__onclose(ws_cli_conn_t* websocket);
static void _chat_websocket_internal__onmessage(ws_cli_conn_t* websocket, unsigned char* base64_to_process_and_decrypt, uint64_t size, int type);
static boole _chat_websocket_internal__has_connection_timed_out(client_t* client, uint64 now);

/**
 * @brief finds which slot of g_clients_array holds a given websocket connection.
 *
 * @param ws_cli_conn_t* p_ws_connection -> websocket connection pointer to look up in the clients array
 *
 * @note takes the clients read lock itself, so it must not be called with that lock already held.
 *
 * @return int64 -> the client's index, or -1 when the pointer is null or matches no connected client
 */
static int64 _chat_websocket_internal__get_client_index(ws_cli_conn_t* p_ws_connection)
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
 * @attention _chat_websocket_internal__onopen is called by different thread everytime
 */
static void _chat_websocket_internal__onopen(ws_cli_conn_t* client)
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

    uint64 started_ms = util__get_timestamp_ms();
    clib__write_lock(&g_clients_global_rwlock_guard);
    uint64 acquired_ms = util__get_timestamp_ms();
    g_server_settings.client_count = g_server_settings.client_count + 1;
    // log_info("%s %d", "client_count , ", g_server_settings.client_count);
    DBG_AUTHENTICATION log_info("%s %p %s", "client connected , ", client, "\n");
    index = client_session__get_new_index_for_client();

    if (index == -1)
    {
        DBG_AUTHENTICATION log_info("%s", "client_session__get_new_index_for_client returned -1, closing socket");
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
    if (server_admin__is_ip_banned(ip_address) == TRUE)
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
        if (client_session__is_there_an_unfinished_handshake_from_same_ip_address(ip_address) == TRUE)
        {
            DBG_AUTHENTICATION log_info("%s", "another handshake from this ip is still in progress, closing socket");
            server_logs__join_refused("same ip already connected", ip_address);
            ws_close_client(client);
            goto label_onopen_end;
        }
    }

    DBG_AUTHENTICATION log_info("%s%lld", "[i] _chat_websocket_internal__onopen : new client id: ", index);

    g_clients_array[index].is_authenticated = FALSE;
    g_clients_array[index].block_direct_chat_pictures = FALSE;
    g_clients_array[index].block_direct_chat_files = FALSE;
    g_clients_array[index].block_channel_chat_pictures = FALSE;
    g_clients_array[index].block_channel_chat_files = FALSE;
    g_clients_array[index].timestamp_connected = util__get_timestamp_ms();
    g_clients_array[index].p_ws_connection = client;
    g_clients_array[index].is_existing = TRUE;
    g_clients_array[index].client_id = index;
    g_clients_array[index].audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    clib__copy_memory(ip_address, g_clients_array[index].ip_address, clib__utf8_string_length(ip_address), 45);

label_onopen_end:
    clib__unlock(&g_clients_global_rwlock_guard);
    DBG_CONNECTION_PROGRESS log_info("[connection] _chat_websocket_internal__onopen client=%lld wait_ms=%llu work_ms=%llu", (long long)index, (unsigned long long)(acquired_ms - started_ms), (unsigned long long)(util__get_timestamp_ms() - acquired_ms));
}

/**
 * @brief websocket callback for a closed connection: drops the client count and tears the client
 *        down through client_session__process_client_disconnect. an unknown socket is ignored.
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of the client that disconnected
 *
 * @note holds the clients AND channels write locks for the whole teardown.
 *
 * @return void
 */
static void _chat_websocket_internal__onclose(ws_cli_conn_t* websocket)
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

    uint64 started_ms = util__get_timestamp_ms();
    clib__write_lock(&g_clients_global_rwlock_guard);
    uint64 acquired_ms = util__get_timestamp_ms();
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

    DBG_AUTHENTICATION log_info("%s %d %s", "_chat_websocket_internal__onclose", client_index, "\n");

    // the socket log needs the address and name before the slot is cleared; a socket that never got
    // a slot (refused at open) still has its address in the library until this callback returns
    if (client_index != -1)
    {
        server_logs__socket_closed(g_clients_array[client_index].ip_address, (g_clients_array[client_index].is_authenticated == TRUE) ? g_clients_array[client_index].username : NULL_POINTER);
    }
    else
    {
        server_logs__socket_closed(ws_getaddress(websocket), NULL_POINTER);
    }

    if (client_index == -1)
    {
        goto label_onclose_end;
    }

    client_session__process_client_disconnect(client_index);

label_onclose_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
    DBG_CONNECTION_PROGRESS log_info("[connection] _chat_websocket_internal__onclose client=%lld wait_ms=%llu work_ms=%llu", (long long)client_index, (unsigned long long)(acquired_ms - started_ms), (unsigned long long)(util__get_timestamp_ms() - acquired_ms));
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
static void _chat_websocket_internal__onmessage(ws_cli_conn_t* websocket, unsigned char* base64_to_process_and_decrypt, uint64_t size, int type)
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
    uint64 started_ms = util__get_timestamp_ms();
    client_index = _chat_websocket_internal__get_client_index(websocket);
    if (client_index == -1)
    {
        return;
    }

    uint64 found_ms = util__get_timestamp_ms();

    DBG_ONMESSAGE log_info("%s %d %s", "_chat_websocket_internal__onmessage() : ", client_index, "\n");

    DBG_ONMESSAGE log_info("%s %llu %s", "_chat_websocket_internal__onmessage received websocket data size is : ", size, "\n");

    if (size > g_server_settings.websocket_message_max_length)
    {
        client_session__close_websocket_connection(client_index, TRUE);
        // ws_close_client(websocket);
        return;
    }

    if (size == 0)
    {
        client_session__close_websocket_connection(client_index, TRUE);
        // ws_close_client(websocket);
        return;
    }

    decrypted_metadata_cstring = (char*)(unsigned char*)memorymanager__allocate(size, MEMALLOC_TYPE_DECRYPT);

    if (decrypted_metadata_cstring == NULL_POINTER)
    {
        DBG_ONMESSAGE log_info("%s %d %s", "_chat_websocket_internal__onmessage decrypted_metadata_cstring is NULL", client_index, "\n");
        return;
    }

    // just a simple readlock, nothing expensive

    clib__read_lock(&g_clients_global_rwlock_guard);

    is_authenticated = g_clients_array[client_index].is_authenticated;
    is_existing = g_clients_array[client_index].is_existing;
    if (!metadata_crypto__decrypt_from_client(&g_clients_array[client_index], (const char*)base64_to_process_and_decrypt, (unsigned char*)decrypted_metadata_cstring, size))
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        memorymanager__free((nuint)decrypted_metadata_cstring);
        return;
    }

    clib__unlock(&g_clients_global_rwlock_guard);

    uint64 decrypted_ms = util__get_timestamp_ms();

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
    uint64 finished_ms = util__get_timestamp_ms();
    if (finished_ms - started_ms >= 250)
    {
        DBG_CONNECTION_PROGRESS log_info("[connection] slow request client=%lld bytes=%llu authenticated=%d lookup_ms=%llu decrypt_ms=%llu handler_ms=%llu", (long long)client_index, (unsigned long long)size, (int)is_authenticated, (unsigned long long)(found_ms - started_ms), (unsigned long long)(decrypted_ms - found_ms), (unsigned long long)(finished_ms - decrypted_ms));
    }
}

/**
 * @brief this is function used that is used as an entry point for websocket thread
 *
 * this function calls theldus internal function that handles incoming websocket connections and that takes it from here
 * @param void* argument -> unused thread argument
 * @return void* -> NULL_POINTER when the thread finishes
 *
 */
void* chat_websocket__listen_thread(void* argument)
{
    struct ws_events evs;

    (void)argument;

    if (g_server_settings.is_tailscale_funnel_enabled == TRUE && tailscale__is_ready() == TRUE)
    {
        ws_set_forwarded_ip_mode(WS_FORWARDED_IP_FUNNEL);
    }
    else if (g_server_settings.is_stunnel_enabled == TRUE)
    {
        ws_set_forwarded_ip_mode(WS_FORWARDED_IP_STUNNEL);
    }
    else
    {
        ws_set_forwarded_ip_mode(WS_FORWARDED_IP_NONE);
    }

    evs.onopen = &_chat_websocket_internal__onopen;
    evs.onclose = &_chat_websocket_internal__onclose;
    evs.onmessage = &_chat_websocket_internal__onmessage;
    if (ws_socket(&evs, g_server_settings.websocket_port, 1, 2000) != 0)
    {
        exit(EXIT_FAILURE);
    }
#ifdef __ANDROID__
    // the wrapper must not report hosting until the listener actually exists
    puts("LEMON_ANDROID_READY");
    fflush(stdout);
#endif
    return NULL_POINTER;
}

/**
 * @brief Checks whether a connection has taken too long to finish signing in or
 * has stopped sending keep-alive messages while that timeout check is enabled.
 *
 * @param client_t* client -> the client slot whose connection is being checked
 * @param uint64 now -> current time in milliseconds from util__get_timestamp_ms
 *
 * @attention The caller must hold the clients lock while reading the connection
 * and settings. Repeat the check after taking the write lock before disconnecting
 * a client, because a keep-alive message or a settings change may have arrived.
 *
 * @return boole -> TRUE when the connection has passed its time limit; FALSE otherwise
 */
static boole _chat_websocket_internal__has_connection_timed_out(client_t* client, uint64 now)
{
    uint64 maintain_limit_ms = 0;

    if (client->is_existing == FALSE && client->timestamp_connected == 0)
    {
        return FALSE;
    }

    if (client->is_authenticated == TRUE)
    {
        if (g_server_settings.is_connection_check_enforced == FALSE || client->is_music_bot == TRUE)
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
 * @param void* argument -> unused thread argument
 * @return void* -> NULL_POINTER when the thread finishes
 */
void* chat_websocket__check_connections_thread(void* argument)
{
    uint64 timestamp_now = 0;
    uint64 i = 0;
    int64* marked_client_ids_for_disconnect = 0;
    int64 number_of_marked_clients = 0;
    boole send_websocket_ping = FALSE;
    client_t* client = NULL_POINTER;

    (void)argument;

    marked_client_ids_for_disconnect = (int64*)memorymanager__allocate(sizeof(int64) * g_server_settings.max_client_count, MEMALLOC_MARKED_CLIENT_INDICES);
    if (marked_client_ids_for_disconnect == NULL_POINTER)
    {
        fprintf(stderr, "Unable to reserve heartbeat storage; reduce server_slots.\n");
        exit(EXIT_FAILURE);
    }

    for (;;)
    {
        timestamp_now = util__get_timestamp_ms();

        DBG_CONNECTION_CHECK_THREAD log_info("%s", "chat_websocket__check_connections_thread tick");

        // ws-level liveness, off by default and switched in the settings tab: a client 4 pings
        // behind gets its socket shut down (ws.c send_ping_close) and its reader thread runs _chat_websocket_internal__onclose
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
            if (_chat_websocket_internal__has_connection_timed_out(&g_clients_array[i], timestamp_now) == TRUE)
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
                if (_chat_websocket_internal__has_connection_timed_out(client, util__get_timestamp_ms()) == FALSE)
                {
                    continue;
                }

                // the socket itself: without this only the entry went away and the closed connection
                // kept its reader thread and ws slot forever (the ws ping used to reap those)
                server_logs__client_disconnect_reason(client, client->is_authenticated == TRUE ? "heartbeat timeout" : "authentication timeout");
                client_session__close_websocket_connection(marked_client_ids_for_disconnect[i], FALSE);
                client_session__process_client_disconnect(marked_client_ids_for_disconnect[i]);
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
