#include "definitions.h"

/* use forward slashes "/" when specifying paths, not backward slashes "\" linux environment has trouble finding files that way
   windows compiler will work with both */

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h" /* needed by base.h */
#include "base.h"

#include "../third-party/ITH-sha/sha256.h"

#include "../third-party/libviolet-0.5.4/src/options.h"
#include "../third-party/libviolet-0.5.4/src/utils.h"

#include "../third-party/rxi-log/log.h"

#include "memory_manager.h"
#include "audio_channel.h"
#include "http/http_server.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   /* SetConsoleMode, for enabling ANSI color on the Windows console */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#endif

/* terminal status markers ([*] info, [+] ok, [-] off, [!] warn, [?] prompt) plus the banner color. they
   default to plain ASCII and are upgraded to ANSI-colored versions by _main_internal__init_terminal (called
   once at startup) when stdout is an interactive, color-capable console. */
static const char* g_mark_info = "[*]";
static const char* g_mark_ok = "[+]";
static const char* g_mark_off = "[-]";
static const char* g_mark_warn = "[!]";
static const char* g_mark_ask = "[?]";
static const char* g_color_banner = "";
static const char* g_color_reset = "";

#ifndef WIN32
#include <unistd.h>     /* fork, execl, access, readlink, _exit */
#include <dirent.h>     /* opendir / readdir (Let's Encrypt cert detection) */
#include <signal.h>
#include <sys/wait.h>   /* waitpid (certbot) */
#include <sys/prctl.h>  /* PR_SET_PDEATHSIG (stunnel dies with the server) */
#endif

static int g_stunnel_pid = 0; /* pid of the optional bundled stunnel child, 0 = none */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER; /* serializes console log output across threads */

static void _main_internal__init_channel_list(void);
static void _main_internal__init_tags_and_icons(void);
static void _main_internal__set_server_settings(void);
static void _main_internal__prompt_stunnel_setup(void);
static void _main_internal__launch_stunnel(void);
static void _main_internal__prompt_client_html_placement(void);
static int64 _main_internal__get_client_index_by_ws_client_pointer(ws_cli_conn_t* p_ws_connection);
static void _main_internal__print_debug_information(void);
static void _main_internal__start_stun_turn_listener_for_webrtc_datachannel(void);
static void _main_internal__print_startup_summary(void);
static void _main_internal__log_lock(bool lock, void* udata);

uint64 g_thread_id0 = 0;
uint64 g_thread_id1 = 0;
uint64 g_thread_id2 = 0;
uint64 g_thread_id3 = 0;

boole g_is_server_running = TRUE;

/**
 * @brief see title
 *
 * @param ws_cli_conn_t* p_ws_connection -> websocket connection pointer to look up in the clients array
 *
 * @return int client count
 *
 */
int64 _main_internal__get_client_index_by_ws_client_pointer(ws_cli_conn_t* p_ws_connection)
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
    boole ip_address_already_in_use = FALSE;
    int64 index = 0;

    /*
     * mutex is needed,, in case onopen is called too fast (each onopen is called by different thread)
     * (each onopen is ran within its own thread)
     * */

    clib__write_lock(&g_clients_global_rwlock_guard);

    g_server_settings.client_count = g_server_settings.client_count + 1;

    /* log_info("%s %d", "client_count , ", g_server_settings.client_count); */

    DBG_AUTHENTICATION log_info("%s %p %s", "client connected , ", client, "\n");

    index = base__get_new_index_for_client();

    if ((g_server_settings.client_count + 1) >= g_server_settings.max_client_count)
    {
        DBG_AUTHENTICATION log_info("%s", "max client reached. Closing connection with client");

        ws_close_client(client);
        goto label_onopen_end;
    }

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

    /* drop banned ips right away, before any client slot is set up */
    if (base__is_ip_banned(ip_address) == TRUE)
    {
        DBG_AUTHENTICATION log_info("%s", "ip address is banned, closing socket");
        ws_close_client(client);
        goto label_onopen_end;
    }

    if (g_server_settings.is_same_ip_address_allowed == FALSE)
    {
        ip_address_already_in_use = base__is_there_a_client_with_same_ip_address(ip_address);

        if (ip_address_already_in_use == TRUE)
        {
            DBG_AUTHENTICATION log_info("%s", "ip address already in use, closing socket");
            ws_close_client(client);
            goto label_onopen_end;
        }
    }

    DBG_AUTHENTICATION log_info("%s%lld", "[i] onopen : new client id: ", index);

    g_clients_array[index].is_authenticated = FALSE;
    g_clients_array[index].timestamp_connected = base__get_timestamp_ms();
    g_clients_array[index].p_ws_connection = client;
    g_clients_array[index].is_existing = TRUE;
    g_clients_array[index].client_id = index;
    g_clients_array[index].audio_state = AUDIO_STATE__AUDIO_COMPLETELY_DISABLED;
    clib__copy_memory(ip_address, g_clients_array[index].ip_address, clib__utf8_string_length(ip_address), 45);

label_onopen_end:
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of the client that disconnected
 *
 * @return void
 * */
void onclose(ws_cli_conn_t* websocket)
{
    uint64 i = 0;
    int64 client_index = -1;

    clib__write_lock(&g_clients_global_rwlock_guard);
    clib__write_lock(&g_channels_global_rwlock_guard);

    g_server_settings.client_count = g_server_settings.client_count - 1;

    /* log_info("%s %d", "client_count , ", g_server_settings.client_count); */

    for (i = 0; i < g_server_settings.max_client_count; i++)
    {
        if (g_clients_array[i].p_ws_connection == websocket)
        {
            client_index = i;
            break;
        }
    }

    DBG_AUTHENTICATION log_info("%s %d %s", "onclose", client_index, "\n");

    if (client_index == -1)
    {
        goto label_onclose_end;
    }

    base__process_client_disconnect(client_index);

label_onclose_end:
    clib__unlock(&g_channels_global_rwlock_guard);
    clib__unlock(&g_clients_global_rwlock_guard);
}

/**
 * @brief self explanatory
 *
 * @param ws_cli_conn_t* websocket -> websocket connection of the client that sent the message
 * @param unsigned char* base64_to_process_and_decrypt -> received base64 payload to decode and decrypt
 * @param uint64_t size -> length in bytes of the received payload
 * @param int type -> websocket frame type of the received message
 *
 * @return void
 * */
void onmessage(ws_cli_conn_t* websocket, unsigned char* base64_to_process_and_decrypt, uint64_t size, int type)
{
    boole is_authenticated = FALSE;
    boole is_existing = FALSE;
    int64 client_index = 0;
    char* decrypted_metadata_cstring = 0;

    /* will this affect negatively */
    client_index = _main_internal__get_client_index_by_ws_client_pointer(websocket);
    if (client_index == -1)
    {
        return;
    }

    DBG_ONMESSAGE log_info("%s %d %s", "onmessage() : ", client_index, "\n");

    DBG_ONMESSAGE log_info("%s %llu %s", "onmessage received websocket data size is : ", size, "\n");

    if (size > g_server_settings.websocket_message_max_length)
    {
        base__close_websocket_connection(client_index, TRUE);
        /* ws_close_client(websocket); */
        return;
    }

    if (size == 0)
    {
        base__close_websocket_connection(client_index, TRUE);
        /* ws_close_client(websocket); */
        return;
    }

    decrypted_metadata_cstring = (char* )(unsigned char* )memorymanager__allocate(size, MEMALLOC_TYPE_DECRYPT);

    if (decrypted_metadata_cstring == NULL_POINTER)
    {
        DBG_ONMESSAGE log_info("%s %d %s", "onmessage decrypted_metadata_cstring is NULL", client_index, "\n");
        return;
    }

    /* just a simple readlock, nothing expensive */

    clib__read_lock(&g_clients_global_rwlock_guard);

    is_authenticated = g_clients_array[client_index].is_authenticated;
    is_existing = g_clients_array[client_index].is_existing;
    base__get_data_from_base64_and_decrypt_it(client_index, (char* )base64_to_process_and_decrypt, decrypted_metadata_cstring, size);

    clib__unlock(&g_clients_global_rwlock_guard);

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
}

/**
 * @brief this is function used that is used as an entry point for websocket thread
 *
 * this function calls theldus internal function that handles incoming websocket connections and that takes it from here
 * @return void
 *
 * */
void websocket_thread(void)
{
    struct ws_events evs;

    evs.onopen = &onopen;
    evs.onclose = &onclose;
    evs.onmessage = &onmessage;
    ws_socket(&evs, g_server_settings.websocket_port, 1, 2000); /* Never returns. */
}

/**
 * @brief this is function used as entry point function of a thread that checks clients connectivity
 * *
 * @return void
 *
 * */
void websocket_connection_check_thread(void)
{
    static uint64 timestamp_now = 0;
    uint64 i = 0;
    int64 size_of_allocated_message_buffer = 0;
    int64* marked_client_ids_for_disconnect = 0;
    char* msg = 0;
    int64 number_of_marked_clients = 0;

    marked_client_ids_for_disconnect = (int64*)memorymanager__allocate(sizeof(int64) * g_server_settings.max_client_count, MEMALLOC_MARKED_CLIENT_INDICES);

    while (g_is_server_running)
    {
        timestamp_now = base__get_timestamp_ms();

        /* clib__null_memory(marked_client_ids_for_disconnect, sizeof(int) * g_server_settings.max_client_count); */
        number_of_marked_clients = 0;

        clib__read_lock(&g_clients_global_rwlock_guard);

        for (i = 0; i < g_server_settings.max_client_count; i++)
        {
            if (g_clients_array[i].is_existing == FALSE && g_clients_array[i].timestamp_connected == 0)
            {
                continue;
            }

            if (g_clients_array[i].is_authenticated == TRUE)
            {
                if (g_clients_array[i].is_music_bot == TRUE)
                {
                    continue;
                }

                timestamp_now = base__get_timestamp_ms();

                /* disconnect client who has not sent maintain_connection_message in given time limit */
                if (g_clients_array[i].timestamp_last_maintain_connection_message_received + 180000 < timestamp_now)
                {
                    DBG_CONNECTION_CHECK_THREAD log_info("%s %p %s", "trying to disconnect client. did not receive maintain connection message : ", g_clients_array[i].p_ws_connection, "\n");

                    marked_client_ids_for_disconnect[number_of_marked_clients] = i;
                    number_of_marked_clients++;
                }
            }

            /* remove client who does not authenticate within given time limit */
            else
            {
                if (g_clients_array[i].timestamp_connected + 60000 < timestamp_now)
                {
                    DBG_CONNECTION_CHECK_THREAD log_info("%s %p %s", "trying to disconnect client : ", g_clients_array[i].p_ws_connection, "\n");

                    marked_client_ids_for_disconnect[number_of_marked_clients] = i;
                    number_of_marked_clients++;
                }
            }
        }

        clib__unlock(&g_clients_global_rwlock_guard);

        if (number_of_marked_clients > 0)
        {
            clib__write_lock(&g_clients_global_rwlock_guard);
            clib__write_lock(&g_channels_global_rwlock_guard);

            for (i = 0; i < number_of_marked_clients; i++)
            {
                base__process_client_disconnect(marked_client_ids_for_disconnect[i]);
            }

            clib__unlock(&g_channels_global_rwlock_guard);
            clib__unlock(&g_clients_global_rwlock_guard);
        }

        sleep(60); /* 60 seconds, same in windows and linux */
    }
}

/**
 * @brief self explanatory
 * *
 * @return void
 * */
static void _main_internal__init_channel_list(void)
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
}

/**
 * @brief if server_settings.json holds a saved channel layout and/or tags+icons, rebuilds them into the
 *        (already allocated) global arrays. each channel/icon/tag is written into the slot matching its
 *        saved id, so id references stay valid (ids are array indices, not opaque handles). runtime-only
 *        fields are reset to their empty defaults. the channel root and the admin tag/icon (id 0) are
 *        seeded separately before this runs and are left in place (this only fills id >= 1 for tags/icons).
 *        a missing or unparseable file is a no-op. must run after the arrays are allocated and before any
 *        client can connect.
 *
 * @return void
 */
static void _main_internal__load_persisted_state(void)
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
    cJSON* json_field = NULL_POINTER;
    channel_t* channel_in_loop = NULL_POINTER;
    icon_t* icon_in_loop = NULL_POINTER;
    tag_t* tag_in_loop = NULL_POINTER;
    ban_entry_t* ban_in_loop = NULL_POINTER;
    int64 channel_id = 0;
    int64 icon_id = 0;
    int64 tag_id = 0;
    uint64 loaded_channels = 0;
    uint64 loaded_icons = 0;
    uint64 loaded_tags = 0;
    uint64 loaded_bans = 0;
    uint64 ban_slot = 0;

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

    /* channels */
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

            /* write into the exact saved slot, never a first-free slot, so channel_id stays stable and
               parent_channel_id references remain valid across the restart */
            channel_in_loop = &g_channel_array[channel_id];
            clib__null_memory(channel_in_loop, sizeof(channel_t));

            channel_in_loop->channel_id = channel_id;
            channel_in_loop->is_existing = TRUE;
            channel_in_loop->maintainer_id = -1;
            channel_in_loop->is_channel_maintainer_present = FALSE;
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

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &channel_in_loop->name[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_NAME_MAX_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "password");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &channel_in_loop->password[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_PASSWORD_MAX_LENGTH - 1); }

            json_field = cJSON_GetObjectItemCaseSensitive(json_channel, "description");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &channel_in_loop->description[0], clib__utf8_string_length(json_field->valuestring), CHANNEL_DESCRIPTION_MAX_LENGTH - 1); }

            loaded_channels++;
        }
    }

    /* icons (id is the array index; the admin icon id 0 is seeded separately, so skip it) */
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

    /* tags (id is the array index; the admin tag id 0 is seeded separately, so skip it) */
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

            json_field = cJSON_GetObjectItemCaseSensitive(json_tag, "name");
            if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &tag_in_loop->name[0], clib__utf8_string_length(json_field->valuestring), TAG_MAX_NAME_LENGTH - 1); }

            loaded_tags++;
        }
    }

    /* load the ban list (filled sequentially; ip is required, the rest is optional metadata) */
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

    cJSON_Delete(json_root);

    if (loaded_channels > 0 || loaded_icons > 0 || loaded_tags > 0 || loaded_bans > 0)
    {
        printf("%s %s%llu%s%llu%s%llu%s%llu%s\n", g_mark_ok, "restored from server_settings.json (", loaded_channels, " channels, ", loaded_icons, " icons, ", loaded_tags, " tags, ", loaded_bans, " bans)");
    }
}

/**
 * @brief self explanatory
 * *
 * @return void
 * */
static void _main_internal__init_tags_and_icons(void)
{
    char tag_name[] = "admin";
    char base64_icon[] = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAACXBIWXMAAAsSAAALEgHS3X78AAACU0lEQVQ4jX2TX0hTYRjGf0eOicqUydEES9L+iHQRnNafu0ASYayrXWiS0S4HFQQhCQMzDEUGQUzIGyOJbrr03LSI/hFY5DBLhyut3CkkN9eZ7sx5bHVx9LgD0nv1vc/3PA/Py/d+AmYJLV3+fC67BsBGLst2bRo5CmszpwPw8fkzAUAAhI5AMF/RcNJGnBn02vobj6MAVJWXAnDlWjsRRXEIHYHg393Eo6Nu8iX1OJ0i0Q+fCfQ84erYe/ZJEmoiQVlpOb2BSxRlM9p/xQDNxw7TP9DG3Ysu1ESCaHzJ4hcVinV1GtklAeB0injdIbzukHUvuyTGw+MATMbmARC3hQDGyyCX+85QWduI1x2i4UIvAF53H+GJIfSsztj9IF+4vpNgXV/D3+mjpeUcajxjG8ff6cPf6bNhajxj8a0ED15NWoTK2kYcVfUAdLsP2MSNTUeB1za+aOR0dHUaR1k5sksiLzjQUinCE0M28eL8AgtzM8guieWv73YMAA41nyB6r52bg11oKylWV5aZWzOXKp02X0ksLqa6uoazrQaPHoao840Rm3phGhw/cpCnW/PHPk2hpdOMDM/aEnT3nLaM1HiGvdovs/+d+GGRlha/AjAyPEvrbcVMkDNXeeiWl/6BNpLJpM24COC7GrcALZ0GQCrbA0BFSQli3gAgXsBbSZrLJADFssezAVA69xaA83fCJPQNAPStDwbwpr8DgGzTKQAiiuIQASOiKAJQIXs8GkDs2zxVNfst8fpqCiO7WigUtk2tQ+FYssfzZxeciKLUAT8LsX+oaO/ttIYBtAAAAABJRU5ErkJggg==";
    tag_t* admin_tag = NULL_POINTER;
    icon_t* admin_icon = NULL_POINTER;

    admin_tag = &g_tags_array[0];
    admin_tag->id = ADMIN_TAG_ID;
    admin_tag->icon_id = 0;
    admin_tag->is_existing = TRUE;
    clib__copy_memory((void*)&tag_name, (void*)&admin_tag->name, strlen(tag_name), TAG_MAX_NAME_LENGTH);

    admin_icon = &g_icons_array[0];
    admin_icon->id = 0;
    admin_icon->is_existing = TRUE;
    clib__copy_memory((void*)&base64_icon, (void*)&admin_icon->base64, strlen(base64_icon), ICON_MAX_LENGTH);
}

/**
 * @brief writes the current settings back to server_settings.json so the next start
 *        is non-interactive
 *
 * @param char[][256] plaintext_keys -> the channel keys as entered (g_server_settings keeps only their
 *        hashes, but the file stores plaintext keys, which are re-hashed on load)
 * @param uint64 keys_count -> number of valid entries in plaintext_keys
 *
 * @return void
 */
static void _main_internal__save_server_settings(char plaintext_keys[][256], uint64 keys_count)
{
    cJSON* json_root = 0;
    cJSON* json_keys = 0;
    char* json_text = 0;
    uint64 i = 0;

    json_root = cJSON_CreateObject();
    if (json_root == NULL_POINTER)
    {
        return;
    }

    cJSON_AddNumberToObject(json_root, "websocket_port", g_server_settings.websocket_port);
    cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]);

    json_keys = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "keys", json_keys);
    for (i = 0; i < keys_count; i++)
    {
        cJSON_AddItemToArray(json_keys, cJSON_CreateString(&plaintext_keys[i][0]));
    }

    cJSON_AddItemToObject(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_idle_mode_allowed", cJSON_CreateBool(g_server_settings.is_idle_mode_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "restart_on_crash", cJSON_CreateBool(g_server_settings.restart_on_crash == TRUE));

    cJSON_AddItemToObject(json_root, "use_stunnel", cJSON_CreateBool(g_server_settings.use_stunnel == TRUE));
    cJSON_AddNumberToObject(json_root, "wss_port", g_server_settings.wss_port);
    cJSON_AddStringToObject(json_root, "stunnel_domain", &g_server_settings.stunnel_domain[0]);
    cJSON_AddStringToObject(json_root, "stunnel_cert_fullchain", &g_server_settings.stunnel_cert_fullchain[0]);
    cJSON_AddStringToObject(json_root, "stunnel_cert_privkey", &g_server_settings.stunnel_cert_privkey[0]);
    cJSON_AddStringToObject(json_root, "client_html_dest", &g_server_settings.client_html_dest[0]);

    cJSON_AddItemToObject(json_root, "serve_client_http", cJSON_CreateBool(g_server_settings.serve_client_http == TRUE));
    cJSON_AddNumberToObject(json_root, "http_port", g_server_settings.http_port);
    cJSON_AddStringToObject(json_root, "http_webroot", &g_server_settings.http_webroot[0]);

    json_text = cJSON_Print(json_root);
    if (json_text != NULL_POINTER)
    {
        if (base__write_file_atomically("server_settings.json", json_text) == TRUE)
        {
            printf("%s %s\n", g_mark_ok, "settings saved to server_settings.json (next start skips these prompts)");
        }
        else
        {
            printf("%s %s\n", g_mark_warn, "could not write server_settings.json (settings not persisted)");
        }
        cJSON_free(json_text);
    }

    cJSON_Delete(json_root);
}

/**
 * @brief self explanatory
 * *
 * @return void
 * */
static void _main_internal__set_server_settings(void)
{
    char input[256];
    uint64 i = 0;
    char plaintext_keys[100][256];
    char verification_message[] = "welcome";
    char default_client_name[30] = "user";

    /* initialization vector must match iv defined in client.html */
    ITH_SHA256_CTX ctx;
    unsigned char custom_iv[16] = { 90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10 };
    FILE* settings_file = 0;
    int64 file_length = 0;
    char* file_buffer = NULL_POINTER;
    cJSON* json_root = NULL_POINTER;
    uint64 bytes_read = 0;
    cJSON* json_field = NULL_POINTER;
    cJSON* json_key = NULL_POINTER;
    int64 key_index = 0;
    int64 requested_key_count = 0;

    clib__null_memory(&g_server_settings, sizeof(server_settings_t));
    clib__null_memory(plaintext_keys, sizeof(plaintext_keys));
    clib__null_memory(input, sizeof(input));
    /* clib__copy_memory(verification_message, g_server_settings.client_verificaton_message_cleartext, strlen(verification_message), 1024); */
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
    g_server_settings.is_display_admin_tag_active = TRUE;
    g_server_settings.is_idle_mode_allowed = TRUE;

    /* set the max client/channel counts here too; the JSON path below returns early, so without this the arrays would allocate at size 0 */
    g_server_settings.max_client_count = MAX_CLIENTS;
    g_server_settings.max_channel_count = MAX_CHANNELS;

    clib__copy_memory(default_client_name, g_server_settings.default_client_name, strlen(default_client_name), 100);

    /* optional non-interactive setup: if server_settings.json exists, load it and skip the prompts below; any omitted field keeps the default set above */
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
                            clib__copy_memory(custom_iv, &g_server_settings.keys[key_index].key_iv, 16, 16);
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
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_hide_clients_in_password_protected_channels_active");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_hide_clients_in_password_protected_channels_active = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_temp_channel_creation_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_temp_channel_creation_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_idle_mode_allowed");
                if (cJSON_IsBool(json_field)) { g_server_settings.is_idle_mode_allowed = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "restart_on_crash");
                if (cJSON_IsBool(json_field)) { g_server_settings.restart_on_crash = cJSON_IsTrue(json_field); }

                /* optional bundled-stunnel front-end (wss) */
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

                /* optional bundled http server that serves the client (mainly for LAN testing) */
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "serve_client_http");
                if (cJSON_IsBool(json_field)) { g_server_settings.serve_client_http = cJSON_IsTrue(json_field); }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_port");
                if (cJSON_IsNumber(json_field) == TRUE) { g_server_settings.http_port = json_field->valueint; }
                json_field = cJSON_GetObjectItemCaseSensitive(json_root, "http_webroot");
                if (cJSON_IsString(json_field) && (json_field->valuestring != NULL_POINTER)) { clib__copy_memory(json_field->valuestring, &g_server_settings.http_webroot[0], clib__utf8_string_length(json_field->valuestring), 511); }

                cJSON_Delete(json_root);

                printf("%s%lld%s%llu%s%llu%s", "loaded settings from server_settings.json (websocket_port=", g_server_settings.websocket_port, ", keys_count=", g_server_settings.keys_count, ", max_clients=", g_server_settings.max_client_count, ")\n");
                return;
            }

            printf("%s", "server_settings.json found but could not be parsed; using interactive setup\n");
        }
    }

    printf("\n%s %s\n\n", g_mark_info, "First-time setup (answers are saved to server_settings.json; delete that file to redo)");

    printf("%s %s", g_mark_ask, "WebSocket port: ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    g_server_settings.websocket_port = strtol(input, 0, 10);
    clib__null_memory(input, sizeof(input));

    printf("%s %s", g_mark_ask, "Add extra metadata encryption keys? A shared password clients must know to connect that also encrypts traffic on top of the existing per-client encryption (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);

    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y") == TRUE))
    {
        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "How many keys? (1-100): ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);

        /* clamp into range with a signed temporary: at least 1 (the operator already opted in) and
           never past the 100-slot keys / plaintext_keys arrays. doing the comparison signed avoids a
           negative atoi result wrapping to a huge uint64 */
        requested_key_count = atoi(input);
        if (requested_key_count < 1)
        {
            requested_key_count = 1;
        }
        if (requested_key_count > 100)
        {
            requested_key_count = 100;
        }
        g_server_settings.keys_count = requested_key_count;

        for (i = 0; i < g_server_settings.keys_count; i++)
        {
            clib__null_memory(input, sizeof(input));
            printf("%s%s%llu%s", g_mark_ask, " key ", i + 1, ": ");
            fgets(input, sizeof(input), stdin);
            clib__sanitize_stdin(input);

            clib__copy_memory(input, &plaintext_keys[i][0], clib__utf8_string_length(input), 255);

            ith_sha256_init(&ctx);
            ith_sha256_update(&ctx, (unsigned char* )input, strlen(input));
            ith_sha256_final(&ctx, g_server_settings.keys[i].key_value);

            /* destination, source, length */
            clib__copy_memory(custom_iv, &g_server_settings.keys[i].key_iv, 16, 16);
        }

        printf("%s %llu %s\n", g_mark_ok, g_server_settings.keys_count, "extra metadata key(s) set");
    }
    else
    {
        g_server_settings.keys_count = 0;
        printf("%s %s\n", g_mark_info, "no extra metadata keys (no connect password; traffic still uses the per-client encryption layer)");
    }

    clib__null_memory(input, sizeof(input));

    /* clib__null_memory(input, sizeof(input));
       printf("%s", "max allowed number of clients {from 1 to 499} : ");
       fgets(input, sizeof(input), stdin);
       clib__sanitize_stdin(input); */

    g_server_settings.max_client_count = MAX_CLIENTS;
    g_server_settings.max_channel_count = MAX_CHANNELS;

    /* g_server_settings.max_client_count = atoi(input);
       if(g_server_settings.max_client_count > 499)
       {
           printf("SETUP FAIL");
           return;
       } */

    /* clib__null_memory(input, sizeof(input));
       printf("%s", "max allowed number of channels {from 1 to 99} : ");
       fgets(input, sizeof(input), stdin);
       clib__sanitize_stdin(input);
       g_server_settings.max_channel_count = atoi(input); */

    /* if(g_server_settings.max_client_count > 99)
       {
           printf("SETUP FAIL");
           return;
       } */

    printf("%s %s", g_mark_ask, "Admin password (max 50 chars): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    clib__copy_memory(input, &g_server_settings.admin_password[0], clib__utf8_string_length(input), 50);
    clib__null_memory(input, sizeof(input));

    printf("%s %s", g_mark_ask, "Disable voice chat? (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
    {
        g_server_settings.is_voice_chat_active = FALSE;
        printf("%s %s\n", g_mark_off, "voice chat: off");
    }
    clib__null_memory(input, sizeof(input));

    printf("%s %s", g_mark_ask, "Block multiple clients from the same IP address? (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
    {
        g_server_settings.is_same_ip_address_allowed = FALSE;
        printf("%s %s\n", g_mark_ok, "same-IP clients: blocked");
    }
    clib__null_memory(input, sizeof(input));

    printf("%s %s", g_mark_ask, "Show country flags next to clients? (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
    {
        g_server_settings.is_display_country_flags_active = TRUE;
        printf("%s %s\n", g_mark_ok, "country flags: on");
    }
    clib__null_memory(input, sizeof(input));

    printf("%s %s", g_mark_ask, "Disable idle clients? (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
    {
        g_server_settings.is_idle_mode_allowed = FALSE;
        printf("%s %s\n", g_mark_off, "idle clients: off");
    }
    clib__null_memory(input, sizeof(input));

    printf("%s %s", g_mark_ask, "Auto-restart the server on crash? (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y")) == TRUE)
    {
        g_server_settings.restart_on_crash = TRUE;
        printf("%s %s\n", g_mark_ok, "auto-restart: on (relaunches on crash; times logged to crashes.txt)");
    }
    clib__null_memory(input, sizeof(input));

    _main_internal__prompt_stunnel_setup();

    if (g_server_settings.use_stunnel == TRUE)
    {
        _main_internal__prompt_client_html_placement();
    }

    printf("%s %s", g_mark_ask, "Also serve the client over plain HTTP, for LAN testing (no local copy needed)? (y/n): ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "Y") == TRUE))
    {
        g_server_settings.serve_client_http = TRUE;

        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "HTTP port (80 is standard; choose another like 8080 if 80 is taken): ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        g_server_settings.http_port = strtol(input, 0, 10);
        if (g_server_settings.http_port <= 0 || g_server_settings.http_port > 65535)
        {
            g_server_settings.http_port = 80;
            printf("%s %s\n", g_mark_warn, "invalid port, defaulting to 80");
        }

        printf("%s %s%lld%s\n", g_mark_info, "HTTP server: serving the client on port ", g_server_settings.http_port, " (port 80 may need admin/root; voice needs HTTPS, so mainly for LAN testing)");
    }
    clib__null_memory(input, sizeof(input));

    _main_internal__save_server_settings(plaintext_keys, g_server_settings.keys_count);
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
    int64 link_length = 0;
    char* last_slash = 0;



    link_length = readlink("/proc/self/exe", out_directory, out_directory_size - 1);
    if (link_length <= 0)
    {
        out_directory[0] = 0;
        return;
    }

    out_directory[link_length] = 0;
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
#endif

/**
 * @brief interactive deployment prompt (reached only in the interactive setup path;
 *        the JSON path fills these fields directly). Asks whether to front the server
 *        with the bundled stunnel for wss, then picks or obtains a TLS certificate,
 *        detecting Let's Encrypt certs under /etc/letsencrypt/live/ first
 *
 * @return void
 */
static void _main_internal__prompt_stunnel_setup(void)
{
#ifndef WIN32
    char input[600];
    char found_domains[32][256];
    char domain[256];
    DIR* cert_dir = 0;
    struct dirent* entry = 0;
    uint64 found_count = 0;
    uint64 i = 0;
    int64 selection = 0;
    pid_t certbot_pid = 0;
    int status = 0; /* waitpid writes an int through &status */
    char probe[700];

    clib__null_memory(input, sizeof(input));
    clib__null_memory(found_domains, sizeof(found_domains));
    clib__null_memory(domain, sizeof(domain));

    printf("%s", "Enable HTTPS (wss) via the bundled stunnel, for use on a live website? (y/n) ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if (input[0] != 'y' && input[0] != 'Y')
    {
        g_server_settings.use_stunnel = FALSE;
        return;
    }
    g_server_settings.use_stunnel = TRUE;
    if (g_server_settings.wss_port == 0)
    {
        g_server_settings.wss_port = 1112;
    }

    /* detect existing Let's Encrypt certificates */
    cert_dir = opendir("/etc/letsencrypt/live");
    if (cert_dir != NULL_POINTER)
    {
        while ((entry = readdir(cert_dir)) != NULL_POINTER && found_count < 32)
        {

            clib__null_memory(probe, sizeof(probe));
            if (entry->d_name[0] == '.')
            {
                continue;
            }
            snprintf(probe, sizeof(probe), "/etc/letsencrypt/live/%s/fullchain.pem", entry->d_name);
            if (access(probe, R_OK) == 0)
            {
                snprintf(found_domains[found_count], 256, "%s", entry->d_name);
                found_count++;
            }
        }
        closedir(cert_dir);
    }

    if (found_count > 0)
    {
        printf("%s", "Found existing Let's Encrypt certificate(s) under /etc/letsencrypt/live/:\n");
        for (i = 0; i < found_count; i++)
        {
            printf("%s%llu%s%s%s", "  [", (i + 1), "] ", found_domains[i], "\n");
        }
        printf("%s", "Pick a number to use it, or 'm' for manual cert paths: ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        selection = atoi(input);
        if (selection >= 1 && (uint64)selection <= found_count)
        {
            snprintf(g_server_settings.stunnel_domain, 256, "%s", found_domains[selection - 1]);
            snprintf(g_server_settings.stunnel_cert_fullchain, 512, "/etc/letsencrypt/live/%s/fullchain.pem", found_domains[selection - 1]);
            snprintf(g_server_settings.stunnel_cert_privkey, 512, "/etc/letsencrypt/live/%s/privkey.pem", found_domains[selection - 1]);
            return;
        }
    }
    else
    {
        printf("%s", "No certificates found under /etc/letsencrypt/live/.\n");
        printf("%s", "Run certbot now to obtain one (needs certbot installed, port 80 free)? (y/n) ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        if (input[0] == 'y' || input[0] == 'Y')
        {
            printf("%s", "Domain name (e.g. chat.example.com): ");
            fgets(domain, sizeof(domain), stdin);
            clib__sanitize_stdin(domain);

            certbot_pid = fork();
            if (certbot_pid == 0)
            {
                /* exec certbot directly (no shell) so the domain cannot inject commands */
                execlp("certbot", "certbot", "certonly", "--standalone", "-d", domain, (char* )NULL_POINTER);
                _exit(127);
            }
            else if (certbot_pid > 0)
            {
                waitpid(certbot_pid, &status, 0);
            }

            snprintf(g_server_settings.stunnel_domain, 256, "%s", domain);
            snprintf(g_server_settings.stunnel_cert_fullchain, 512, "/etc/letsencrypt/live/%s/fullchain.pem", domain);
            snprintf(g_server_settings.stunnel_cert_privkey, 512, "/etc/letsencrypt/live/%s/privkey.pem", domain);
            return;
        }
    }

    /* manual entry */
    printf("%s", "Domain name: ");
    fgets(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain), stdin);
    clib__sanitize_stdin(g_server_settings.stunnel_domain);
    printf("%s", "Path to fullchain.pem: ");
    fgets(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain), stdin);
    clib__sanitize_stdin(g_server_settings.stunnel_cert_fullchain);
    printf("%s", "Path to privkey.pem: ");
    fgets(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey), stdin);
    clib__sanitize_stdin(g_server_settings.stunnel_cert_privkey);
#endif
}

/**
 * @brief if wss is enabled, optionally copies client.html into a web root so the page
 *        is served over https alongside the wss endpoint; looks for client.html next to
 *        the server and in the repo's client/ dir, then asks for a destination directory
 *        (default /var/www/html). Only called when use_stunnel is TRUE.
 *
 * @return void
 */
static void _main_internal__prompt_client_html_placement(void)
{
#ifndef WIN32
    char input[600];
    char exe_dir[600];
    char candidates[3][700];
    char dest_dir[480];
    char dest_path[512];
    char copy_buffer[8192];
    const char* source = NULL_POINTER;
    FILE* in_file = 0;
    FILE* out_file = 0;
    uint64 bytes_read = 0;
    uint64 i = 0;

    clib__null_memory(input, sizeof(input));
    clib__null_memory(exe_dir, sizeof(exe_dir));
    clib__null_memory(candidates, sizeof(candidates));
    clib__null_memory(dest_dir, sizeof(dest_dir));
    clib__null_memory(dest_path, sizeof(dest_path));
    clib__null_memory(copy_buffer, sizeof(copy_buffer));

    _main_internal__executable_dir(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        snprintf(exe_dir, sizeof(exe_dir), ".");
    }
    snprintf(candidates[0], sizeof(candidates[0]), "%s/client.html", exe_dir);
    snprintf(candidates[1], sizeof(candidates[1]), "%s/../client/client.html", exe_dir);
    snprintf(candidates[2], sizeof(candidates[2]), "client.html");

    for (i = 0; i < 3; i++)
    {
        if (access(candidates[i], R_OK) == 0)
        {
            source = candidates[i];
            break;
        }
    }

    if (source == NULL_POINTER)
    {
        printf("%s", "client.html not found near the server - copy it to your web root manually.\n");
        return;
    }

    printf("%s%s%s", "found client.html at ", source, "\n");
    printf("%s", "copy it to a web root so it is served over https? (y/n) ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if (input[0] != 'y' && input[0] != 'Y')
    {
        return;
    }

    printf("%s", "destination directory (default /var/www/html): ");
    fgets(dest_dir, sizeof(dest_dir), stdin);
    clib__sanitize_stdin(dest_dir);
    if (dest_dir[0] == 0)
    {
        snprintf(dest_dir, sizeof(dest_dir), "%s", "/var/www/html");
    }
    snprintf(dest_path, sizeof(dest_path), "%s/client.html", dest_dir);

    in_file = fopen(source, "rb");
    if (in_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not read ", source, "\n");
        return;
    }
    out_file = fopen(dest_path, "wb");
    if (out_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", dest_path, " (need root for /var/www/html?)\n");
        fclose(in_file);
        return;
    }
    while ((bytes_read = fread(copy_buffer, 1, sizeof(copy_buffer), in_file)) > 0)
    {
        fwrite(copy_buffer, 1, bytes_read, out_file);
    }
    fclose(in_file);
    fclose(out_file);

    snprintf(g_server_settings.client_html_dest, sizeof(g_server_settings.client_html_dest), "%s", dest_path);
    printf("%s%s%s", "copied client.html -> ", dest_path, "\n");
#endif
}

/**
 * @brief if wss is enabled, writes stunnel.conf next to the executable and launches
 *        the bundled stunnel so the server is reachable over wss as well as ws
 *
 * @attention the launched child is set to die with this server (PR_SET_PDEATHSIG)
 *
 * @return void
 */
static void _main_internal__launch_stunnel(void)
{
#ifndef WIN32
    char exe_dir[600];
    char conf_path[700];
    char stunnel_path[700];
    FILE* conf_file = 0;
    pid_t stunnel_pid = 0;

    clib__null_memory(exe_dir, sizeof(exe_dir));
    clib__null_memory(conf_path, sizeof(conf_path));
    clib__null_memory(stunnel_path, sizeof(stunnel_path));

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
    /* the patched-stunnel option key is still "xforwardedfor"; it injects the renamed X-Stunnel-Client-IP header */
    fprintf(conf_file, "# inject the real client IP as the X-Stunnel-Client-IP header\n");
    fprintf(conf_file, "xforwardedfor = yes\n");
    fclose(conf_file);

    if (access(stunnel_path, X_OK) != 0)
    {
        printf("%s%s%s", "stunnel binary not found / not executable at ", stunnel_path, "; not started\n");
        return;
    }

    stunnel_pid = fork();
    if (stunnel_pid == 0)
    {
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* die with the parent server */
        execl(stunnel_path, stunnel_path, conf_path, (char* )NULL_POINTER);
        _exit(127); /* exec failed */
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

    printf("\n");
    printf("  %s%s%s\n", g_color_banner, "lemon-chat is running - services and ports", g_color_reset);
    printf("\n");

    /* the websocket listener is always running */
    printf("  %s  %-18s port %lld\n", g_mark_ok, "websocket", g_server_settings.websocket_port);

    /* the webrtc datachannel + bundled libviolet STUN/TURN listener (udp 3478) are always up; this is just
       the transport. whether the server actually forwards audio over it is gated separately for client
       voice and for music bots, shown on the next two lines */
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

    /* optional bundled http server that serves the client */
    if (g_server_settings.serve_client_http == TRUE)
    {
        printf("  %s  %-18s port %lld\n", g_mark_ok, "http server", g_server_settings.http_port);
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "http server");
    }

    /* optional bundled stunnel wss front-end; g_stunnel_pid is only set when it actually launched
       (it does not launch on windows), so this reflects what is really running */
    if (g_stunnel_pid > 0)
    {
        printf("  %s  %-18s port %lld (wss -> ws %lld)\n", g_mark_ok, "stunnel (wss)", g_server_settings.wss_port, g_server_settings.websocket_port);
    }
    else
    {
        printf("  %s  %-18s not enabled\n", g_mark_off, "stunnel (wss)");
    }

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

    /* printf("%s", "[important] start_stun_turn_listener_for_webrtc_datachannel started \n"); */
    /* char* argv[] = {
           "violet",
           "--credentials=usweger123:pw1wegweg23Q --log-level=verbose",
           0
       }; */


    /* char* argv[] = { "violet", "--log-level=error", 0 }; */

    /* char* argv[] = { "violet", "--log-level=warn", 0 }; */
    /* char* argv[] = { "violet", "--log-level=info", 0 }; */
    /* char* argv[] = { "violet", "--log-level=verbose", 0 }; */

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

    /* juice_server_destroy(server); */
    /* violet_options_destroy(&vopts); */

error:

    /* violet_options_destroy(&vopts); */
    return;
}

/**
 * @brief prints out debug information at start
 *
 */
void _main_internal__print_debug_information(void)
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
    DBG_AUDIOCHANNEL_WEBRTC printf("%s", "DBG_AUDIOCHANNEL_WEBRTC active \n");
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

        /* GetConsoleMode only succeeds for a real console (not a redirected file/pipe), which also keeps
           escape codes out of redirected output. ENABLE_VIRTUAL_TERMINAL_PROCESSING turns on ANSI. */
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
        g_mark_info = "\033[1;36m[*]\033[0m";   /* cyan */
        g_mark_ok = "\033[1;32m[+]\033[0m";     /* green */
        g_mark_off = "\033[1;31m[-]\033[0m";    /* red */
        g_mark_warn = "\033[1;33m[!]\033[0m";   /* yellow */
        g_mark_ask = "\033[1;36m[?]\033[0m";    /* cyan */
        g_color_banner = "\033[1;33m";          /* bold yellow */
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
int main(void)
{
    char input[50];
    char http_webroot_resolved[512];
    char client_html_path[640];
    FILE* client_html_file = 0;

    /* flush logs line-by-line even when stdout/stderr are piped (tee / file); */
    /* without this, output is block-buffered and only appears in delayed chunks */
    setvbuf(stdout, NULL_POINTER, _IOLBF, 0);
    setvbuf(stderr, NULL_POINTER, _IOLBF, 0);

    /* serialize log output so threads cannot interleave their lines on the console */
    log_set_lock(_main_internal__log_lock, NULL_POINTER);

    _main_internal__init_terminal();
    _main_internal__print_banner();

#ifdef DEBUG_ACTIVE
    printf("%s", "this is debug build \n");
    _main_internal__print_debug_information();
#endif

    /* run this so rand() gives random output every time */
    srand(time(0));

    clib__rwlock_init(&g_clients_global_rwlock_guard);
    clib__rwlock_init(&g_webrtc_muggles_rwlock_guard);
    clib__rwlock_init(&g_channels_global_rwlock_guard);
    clib__rwlock_init(&g_tags_global_rwlock_guard);
    clib__rwlock_init(&g_icons_global_rwlock_guard);

    if (pthread_mutex_init(&g_chat_message_id_mutex, NULL_POINTER))
    {
        log_info("%s", "pthread_rwlock_init chat_message_id_mutex init failed", 100, "\n");
        exit(0);
    }

    memorymanager__init();

    _main_internal__set_server_settings();
    g_clients_array = (client_t*)memorymanager__allocate(sizeof(client_t) * g_server_settings.max_client_count, MEMALLOC_CLIENTS_ARRAY);
    g_channel_array = (channel_t*)memorymanager__allocate(sizeof(channel_t) * g_server_settings.max_channel_count, MEMALLOC_CHANNELS_ARRAY);
    g_client_stored_data = (client_stored_data_t*)memorymanager__allocate(sizeof(client_stored_data_t) * MAX_CLIENT_STORED_DATA, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
    g_icons_array = (icon_t*)memorymanager__allocate(sizeof(icon_t) * MAX_ICONS, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
    g_tags_array = (tag_t*)memorymanager__allocate(sizeof(tag_t) * MAX_TAGS, MEMALLOC_CLIENT_STORED_DATA_ARRAY);
    g_ban_array = (ban_entry_t*)memorymanager__allocate(sizeof(ban_entry_t) * MAX_BANS, MEMALLOC_BANS_ARRAY);
    clib__null_memory(g_ban_array, sizeof(ban_entry_t) * MAX_BANS);
    g_webrtc_muggles_array = (webrtc_peer_t*)memorymanager__allocate(sizeof(webrtc_peer_t) * g_server_settings.max_client_count, MEMALLOC_WEBRTC_PEERS);

    _main_internal__init_channel_list();
    _main_internal__init_tags_and_icons();
    _main_internal__load_persisted_state();

    /* resolve the http webroot and warn about a missing client.html now (main thread), so this and the
       startup summary print before any worker thread is launched and cannot interleave on the console */
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

        /* if nothing resolved (no webroot configured on windows, or the executable-dir lookup failed),
           serve from the current working directory - the same place server_settings.json lives. without
           this the served path would be "/client.html", which on windows means the drive root, not the cwd */
        if (http_webroot_resolved[0] == 0)
        {
            http_webroot_resolved[0] = '.';
            http_webroot_resolved[1] = 0;
        }

        /* warn loudly if client.html is not where the http server will look for it, so the operator does
           not get a silent 404 in the browser */
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

    /* launch stunnel (synchronous, main thread; sets g_stunnel_pid) and print the startup summary BEFORE
       any worker thread is started, so they cannot interleave on the console with thread log output */
    _main_internal__launch_stunnel();

    _main_internal__print_startup_summary();

    pthread_create((pthread_t*)&g_thread_id0, 0, (void*)&websocket_thread, 0);
    pthread_create((pthread_t*)&g_thread_id1, 0, (void*)&websocket_connection_check_thread, 0);

    /* http_server__start spawns its own thread and returns immediately (webroot already resolved above) */
    if (g_server_settings.serve_client_http == TRUE)
    {
        http_server__start(g_server_settings.http_port, http_webroot_resolved);
    }

    /* the STUN/TURN + webrtc datachannel listener is always started and kept up; is_voice_chat_active
       only controls whether the server re-transmits audio between clients (gated in audio_channel.c) */
    pthread_create((pthread_t*)&g_thread_id2, 0, (void*)&_main_internal__start_stun_turn_listener_for_webrtc_datachannel, 0);

    for (;;)
    {
        clib__null_memory(input, sizeof(input));
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
    }

    return 0;
}
