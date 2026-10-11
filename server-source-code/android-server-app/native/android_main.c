#include "../../main/definitions.h"

#include "../../third-party/dave-g-json/cJSON.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>

int lemon_server_main(void);

static void _android_main_internal__print_memory_information(void);
static boole _android_main_internal__are_settings_valid(void);

/**
 * @brief reports the startup array sizes for this Android ABI without allocating the arrays or
 *        starting any listeners. the app uses this to explain the slot count before starting.
 *
 * @note active connection buffers, threads and optional offline messages need additional memory.
 *
 * @return void
 */
static void _android_main_internal__print_memory_information(void)
{
    uint64 bytes_per_slot = 0;
    uint64 fixed_bytes = 0;

    // Include the chat, audio and video sockets reserved for every client.
    bytes_per_slot = sizeof(client_t) + sizeof(webrtc_peer_t)
        + WEBSOCKET_CONNECTIONS_PER_CLIENT * ws_client_slot_size() + sizeof(int64);
    fixed_bytes = sizeof(channel_t) * MAX_CHANNELS
        + sizeof(client_stored_data_t) * MAX_CLIENT_STORED_DATA
        + sizeof(icon_t) * MAX_ICONS
        + sizeof(tag_t) * MAX_TAGS
        + sizeof(ban_entry_t) * MAX_BANS;

    printf("{\"slot_bytes\":%llu,\"fixed_bytes\":%llu}\n", bytes_per_slot, fixed_bytes);
}

/**
 * @brief checks the required startup settings before entering the desktop server. an Android service
 *        has no console for the interactive first-time setup, so a bad file must fail immediately.
 *
 * @return boole -> TRUE when the required startup fields are valid
 */
static boole _android_main_internal__are_settings_valid(void)
{
    FILE* settings_file = NULL_POINTER;
    int64 file_length = 0;
    uint64 bytes_read = 0;
    char* file_buffer = NULL_POINTER;
    cJSON* json_root = NULL_POINTER;
    cJSON* json_port = NULL_POINTER;
    cJSON* json_slots = NULL_POINTER;
    cJSON* json_admin_password = NULL_POINTER;
    boole are_settings_valid = FALSE;

    settings_file = fopen("server_settings.json", "rb");
    if (settings_file == NULL_POINTER)
    {
        return FALSE;
    }
    if (fseek(settings_file, 0, SEEK_END) != 0)
    {
        fclose(settings_file);
        return FALSE;
    }

    file_length = ftell(settings_file);
    // allow persisted avatars and icons as well as the small initial settings object
    if (file_length <= 0 || file_length > 128 * 1024 * 1024 || fseek(settings_file, 0, SEEK_SET) != 0)
    {
        fclose(settings_file);
        return FALSE;
    }
    file_buffer = (char*)calloc((size_t)file_length + 1, 1);
    if (file_buffer == NULL_POINTER)
    {
        fclose(settings_file);
        return FALSE;
    }

    bytes_read = fread(file_buffer, 1, (size_t)file_length, settings_file);
    fclose(settings_file);
    if (bytes_read == (uint64)file_length)
    {
        json_root = cJSON_Parse(file_buffer);
    }
    free(file_buffer);
    file_buffer = NULL_POINTER;

    json_port = cJSON_GetObjectItemCaseSensitive(json_root, "websocket_port");
    json_slots = cJSON_GetObjectItemCaseSensitive(json_root, "server_slots");
    json_admin_password = cJSON_GetObjectItemCaseSensitive(json_root, "admin_password");
    if (cJSON_IsObject(json_root)
        && cJSON_IsNumber(json_port) && json_port->valuedouble == json_port->valueint
        && json_port->valueint >= 1024 && json_port->valueint <= 65535
        && cJSON_IsNumber(json_slots) && json_slots->valuedouble == json_slots->valueint
        && json_slots->valueint >= 1 && json_slots->valueint <= MAX_SERVER_SLOTS
        && cJSON_IsString(json_admin_password) && strlen(json_admin_password->valuestring) == 44)
    {
        are_settings_valid = TRUE;
    }

    cJSON_Delete(json_root);
    return are_settings_valid;
}

/**
 * @brief Android entry point. handles the read-only size/settings queries, ties the child lifetime to
 *        its owning app, and prevents overlapping server processes from using the same settings file.
 *
 * @param int argument_count -> number of command-line arguments
 * @param char** arguments -> optional --memory-info or --check-settings query
 *
 * @return int -> server exit code, or EXIT_FAILURE when startup is not possible
 */
int main(int argument_count, char** arguments)
{
    pid_t parent_process_id = 0;
    int lock_file_descriptor = -1;
    int server_exit_code = EXIT_FAILURE;

    parent_process_id = getppid();
    if (parent_process_id <= 1 || prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent_process_id)
    {
        return EXIT_FAILURE;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--memory-info") == 0)
    {
        _android_main_internal__print_memory_information();
        return EXIT_SUCCESS;
    }
    if (_android_main_internal__are_settings_valid() == FALSE)
    {
        fprintf(stderr, "Invalid or missing server_settings.json. Open Settings in the server app before starting.\n");
        return EXIT_FAILURE;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--check-settings") == 0)
    {
        return EXIT_SUCCESS;
    }
    if (argument_count != 1)
    {
        return EXIT_FAILURE;
    }

    lock_file_descriptor = open("server.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_file_descriptor < 0 || flock(lock_file_descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        fprintf(stderr, "Another server process is still stopping. Wait a moment and try again.\n");
        if (lock_file_descriptor >= 0)
        {
            close(lock_file_descriptor);
        }
        return EXIT_FAILURE;
    }

    server_exit_code = lemon_server_main();
    close(lock_file_descriptor);
    return server_exit_code;
}
