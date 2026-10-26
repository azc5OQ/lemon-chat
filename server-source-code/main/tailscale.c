// This file configures Funnel through the installed Tailscale program.
#include "tailscale.h"
#include "base.h"
#include "first_time_setup.h"
#include "util.h"
#include "../third-party/dave-g-json/cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef LEMON_TAILSCALE_TIMEOUT_MS
#define LEMON_TAILSCALE_TIMEOUT_MS 10000
#endif
#ifndef LEMON_TAILSCALE_OUTPUT_LIMIT
#define LEMON_TAILSCALE_OUTPUT_LIMIT 65536
#endif

static boole g_tailscale_ready = FALSE;
static char g_tailscale_hostname[256];

// declarations
static boole _tailscale_internal__find_executable(char* path, uint64 capacity);
static int _tailscale_internal__run(const char* const* arguments, char* output, uint64 capacity);
static void _tailscale_internal__print_error(cstring operation, int result, cstring output);
static cJSON* _tailscale_internal__read_json(const char* const* arguments, cstring operation, boole allow_empty);
static boole _tailscale_internal__read_hostname(cJSON* status);
static boole _tailscale_internal__has_foreground_port(cJSON* status, int64 public_port, uint depth);
static int _tailscale_internal__check_route(cJSON* status, int64 public_port, cstring target);
static boole _tailscale_internal__set_route(int64 public_port, cstring target);
#if defined(_WIN32) || defined(WIN32)
static boole _tailscale_internal__append_argument(char* command, uint64 capacity, cstring argument);
#elif !defined(__ANDROID__)
static uint64 _tailscale_internal__milliseconds(void);
#endif

/**
 * @brief Locates the same Tailscale executable used during startup.
 * @param char* path Buffer receiving its absolute path
 * @param uint64 capacity Buffer size in bytes
 * @return boole Whether the executable is installed
 */
static boole _tailscale_internal__find_executable(char* path, uint64 capacity)
{
#if defined(__ANDROID__)
    (void)path;
    (void)capacity;
    return FALSE;
#elif defined(LEMON_TAILSCALE_EXECUTABLE)
    return util__find_executable(LEMON_TAILSCALE_EXECUTABLE, path, capacity);
#else
#if defined(_WIN32) || defined(WIN32)
    char program_files[768];
    char installed_path[1024];
    DWORD length = 0;
    uint64 index = 0;
    const char* directories[] =
    {
        "ProgramFiles",
        "ProgramW6432",
        "ProgramFiles(x86)"
    };

    for (index = 0; index < sizeof(directories) / sizeof(directories[0]); index++)
    {
        length = GetEnvironmentVariableA(directories[index], program_files, sizeof(program_files));
        if (length > 0 && length < sizeof(program_files))
        {
            snprintf(installed_path, sizeof(installed_path), "%s\\Tailscale\\tailscale.exe", program_files);
            if (util__find_executable(installed_path, path, capacity))
            {
                return TRUE;
            }
        }
    }
#endif
    return util__find_executable("tailscale", path, capacity);
#endif
}

/**
 * @brief Checks whether the Tailscale command line program is installed.
 * @return boole Whether startup can locate an executable
 */
boole tailscale__is_available(void)
{
    char executable[1024];

    return _tailscale_internal__find_executable(executable, sizeof(executable));
}

#if defined(_WIN32) || defined(WIN32)
/**
 * @brief Quotes one argument for the Windows process API.
 * @param char* command command buffer receiving the argument
 * @param uint64 capacity buffer size in bytes
 * @param cstring argument argument to append
 * @return boole whether the argument fit
 */
static boole _tailscale_internal__append_argument(char* command, uint64 capacity, cstring argument)
{
    uint64 position = strlen(command);
    uint64 slashes = 0;
    uint64 count = 0;
    const char* cursor = argument;
    char current = 0;

    if (position + 4 >= capacity)
    {
        return FALSE;
    }
    if (position != 0)
    {
        command[position++] = ' ';
    }
    command[position++] = '"';
    for (;;)
    {
        slashes = 0;
        while (*cursor == '\\')
        {
            slashes++;
            cursor++;
        }
        current = *cursor;
        count = (current == '"' || current == 0) ? slashes * 2 : slashes;
        if (position + count + 3 >= capacity)
        {
            return FALSE;
        }
        while (count > 0)
        {
            command[position++] = '\\';
            count--;
        }
        if (current == 0)
        {
            break;
        }
        if (current == '"')
        {
            command[position++] = '\\';
        }
        command[position++] = current;
        cursor++;
    }
    command[position++] = '"';
    command[position] = 0;
    return TRUE;
}
#elif !defined(__ANDROID__)
/**
 * @brief Returns elapsed milliseconds from a monotonic clock.
 * @return uint64 elapsed milliseconds
 */
static uint64 _tailscale_internal__milliseconds(void)
{
    struct timespec current;

    clock_gettime(CLOCK_MONOTONIC, &current);
    return (uint64)current.tv_sec * 1000 + (uint64)current.tv_nsec / 1000000;
}
#endif

/**
 * @brief Runs Tailscale with bounded output and a timeout.
 * @param const char* const* arguments arguments after the executable name
 * @param char* output combined standard output and error
 * @param uint64 capacity output buffer size in bytes
 * @return int exit code or a negative execution error
 */
static int _tailscale_internal__run(const char* const* arguments, char* output, uint64 capacity)
{
#if defined(__ANDROID__)
    (void)arguments;
    (void)capacity;
    output[0] = 0;
    return -1;
#else
    uint64 used = 0;
    uint64 index = 0;
    int result = -1;
    char executable[1024];
#if defined(_WIN32) || defined(WIN32)
    char command[4096];
    SECURITY_ATTRIBUTES security;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE read_pipe = NULL_POINTER;
    HANDLE write_pipe = NULL_POINTER;
    HANDLE input = INVALID_HANDLE_VALUE;
    DWORD available = 0;
    DWORD received = 0;
    DWORD exit_code = 0;
    ULONGLONG started = 0;
    boole running = FALSE;
#else
    char* argv[16];
    int descriptors[2] = {-1, -1};
    int status = 0;
    int spawn_result = 0;
    int flags = 0;
    ssize_t received = 0;
    pid_t process = 0;
    pid_t waited = 0;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    struct timespec pause = {0, 10000000};
    uint64 started = 0;
    boole finished = FALSE;
    char overflow = 0;
#endif

    output[0] = 0;
    if (!_tailscale_internal__find_executable(executable, sizeof(executable)))
    {
        return -1;
    }
#if defined(_WIN32) || defined(WIN32)
    memset(command, 0, sizeof(command));
    memset(&security, 0, sizeof(security));
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    if (!_tailscale_internal__append_argument(command, sizeof(command), executable))
    {
        return -1;
    }
    for (index = 0; arguments[index] != NULL_POINTER; index++)
    {
        if (!_tailscale_internal__append_argument(command, sizeof(command), arguments[index]))
        {
            return -1;
        }
    }
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
    {
        return -1;
    }
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0))
    {
        goto cleanup;
    }
    input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL_POINTER);
    if (input == INVALID_HANDLE_VALUE)
    {
        goto cleanup;
    }
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    if (!CreateProcessA(executable, command, NULL_POINTER, NULL_POINTER, TRUE, CREATE_NO_WINDOW,
        NULL_POINTER, NULL_POINTER, &startup, &process))
    {
        goto cleanup;
    }
    running = TRUE;
    CloseHandle(write_pipe);
    write_pipe = NULL_POINTER;
    CloseHandle(input);
    input = INVALID_HANDLE_VALUE;
    started = GetTickCount64();
    for (;;)
    {
        if (GetTickCount64() - started >= LEMON_TAILSCALE_TIMEOUT_MS)
        {
            result = -2;
            break;
        }
        available = 0;
        if (PeekNamedPipe(read_pipe, NULL_POINTER, 0, NULL_POINTER, &available, NULL_POINTER) && available > 0)
        {
            if (used + 1 >= capacity)
            {
                result = -3;
                break;
            }
            if (available > capacity - used - 1)
            {
                available = (DWORD)(capacity - used - 1);
            }
            if (!ReadFile(read_pipe, output + used, available, &received, NULL_POINTER))
            {
                break;
            }
            used += received;
            output[used] = 0;
            continue;
        }
        if (running == FALSE)
        {
            break;
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
        {
            if (GetExitCodeProcess(process.hProcess, &exit_code))
            {
                result = (int)exit_code;
            }
            running = FALSE;
            continue;
        }
        Sleep(10);
    }
cleanup:
    if (running == TRUE)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
    }
    if (process.hThread != NULL_POINTER)
    {
        CloseHandle(process.hThread);
    }
    if (process.hProcess != NULL_POINTER)
    {
        CloseHandle(process.hProcess);
    }
    if (input != INVALID_HANDLE_VALUE)
    {
        CloseHandle(input);
    }
    if (read_pipe != NULL_POINTER)
    {
        CloseHandle(read_pipe);
    }
    if (write_pipe != NULL_POINTER)
    {
        CloseHandle(write_pipe);
    }
#else
    argv[0] = (char*)executable;
    for (index = 0; arguments[index] != NULL_POINTER; index++)
    {
        if (index + 2 >= sizeof(argv) / sizeof(argv[0]))
        {
            return -1;
        }
        argv[index + 1] = (char*)arguments[index];
    }
    argv[index + 1] = NULL_POINTER;
    if (pipe(descriptors) != 0)
    {
        return -1;
    }
    flags = fcntl(descriptors[0], F_GETFL, 0);
    if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (posix_spawn_file_actions_init(&actions) != 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&actions, descriptors[0]) != 0 ||
        posix_spawn_file_actions_addclose(&actions, descriptors[1]) != 0)
    {
        posix_spawn_file_actions_destroy(&actions);
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (posix_spawnattr_init(&attributes) != 0)
    {
        posix_spawn_file_actions_destroy(&actions);
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0 || posix_spawnattr_setpgroup(&attributes, 0) != 0)
    {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    spawn_result = posix_spawn(&process, executable, &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(descriptors[1]);
    if (spawn_result != 0)
    {
        close(descriptors[0]);
        return -1;
    }
    started = _tailscale_internal__milliseconds();
    for (;;)
    {
        if (_tailscale_internal__milliseconds() - started >= LEMON_TAILSCALE_TIMEOUT_MS)
        {
            result = -2;
            break;
        }
        if (used + 1 >= capacity)
        {
            received = read(descriptors[0], &overflow, 1);
            if (received > 0)
            {
                result = -3;
                break;
            }
        }
        else
        {
            received = read(descriptors[0], output + used, capacity - used - 1);
        }
        if (received > 0)
        {
            used += (uint64)received;
            output[used] = 0;
            continue;
        }
        if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            break;
        }
        if (finished == FALSE)
        {
            waited = waitpid(process, &status, WNOHANG);
            if (waited == process)
            {
                finished = TRUE;
            }
            else if (waited < 0 && errno != EINTR)
            {
                break;
            }
        }
        if (finished == TRUE && received == 0)
        {
            result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        nanosleep(&pause, NULL_POINTER);
    }
    if (result < 0)
    {
        kill(-process, SIGKILL);
    }
    if (finished == FALSE)
    {
        while (waitpid(process, &status, 0) < 0 && errno == EINTR)
        {
        }
    }
    close(descriptors[0]);
#endif
    return result;
#endif
}

/**
 * @brief Prints a bounded diagnostic without terminal control characters.
 * @param cstring operation operation that failed
 * @param int result process exit code or execution error
 * @param cstring output diagnostic returned by Tailscale
 */
static void _tailscale_internal__print_error(cstring operation, int result, cstring output)
{
    uint64 index = 0;
    unsigned char current = 0;

    printf("%s Tailscale %s failed", g_mark_warn, operation);
    if (result == -2)
    {
        printf(" after %d milliseconds", LEMON_TAILSCALE_TIMEOUT_MS);
    }
    else if (result == -3)
    {
        printf(" because its output exceeded %d bytes", LEMON_TAILSCALE_OUTPUT_LIMIT);
    }
    printf(". Direct WS remains available.\n");
    if (result == -1)
    {
        printf("    Install and start Tailscale, then sign in.\n");
    }
    if (output[0] != '{' && output[0] != 0)
    {
        printf("    ");
        for (index = 0; index < 2048 && output[index] != 0; index++)
        {
            current = (unsigned char)output[index];
            if (current >= 32 && current < 127)
            {
                putchar(current);
            }
            else if (current == '\n' || current == '\r' || current == '\t')
            {
                putchar(' ');
            }
        }
        putchar('\n');
    }
}

/**
 * @brief Reads one JSON response from Tailscale.
 * @param const char* const* arguments CLI arguments
 * @param cstring operation diagnostic name
 * @param boole allow_empty whether JSON null means no configured routes
 * @return cJSON* response owned by the caller or NULL_POINTER
 */
static cJSON* _tailscale_internal__read_json(const char* const* arguments, cstring operation, boole allow_empty)
{
    char* output = NULL_POINTER;
    cJSON* document = NULL_POINTER;
    int result = -1;

    output = malloc(LEMON_TAILSCALE_OUTPUT_LIMIT + 1);
    if (output == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    result = _tailscale_internal__run(arguments, output, LEMON_TAILSCALE_OUTPUT_LIMIT + 1);
    if (result == 0)
    {
        document = cJSON_Parse(output);
        if (allow_empty == TRUE && cJSON_IsNull(document))
        {
            cJSON_Delete(document);
            document = cJSON_CreateObject();
        }
        if (!cJSON_IsObject(document))
        {
            cJSON_Delete(document);
            document = NULL_POINTER;
            printf("%s Tailscale %s returned invalid JSON. Direct WS remains available.\n", g_mark_warn, operation);
        }
    }
    else
    {
        _tailscale_internal__print_error(operation, result, output);
    }
    free(output);
    return document;
}

/**
 * @brief Reads a valid Tailscale hostname from a running daemon.
 * @param cJSON* status Tailscale status response
 * @return boole whether a usable hostname was found
 */
static boole _tailscale_internal__read_hostname(cJSON* status)
{
    cJSON* state = cJSON_GetObjectItemCaseSensitive(status, "BackendState");
    cJSON* self = cJSON_GetObjectItemCaseSensitive(status, "Self");
    cJSON* name = cJSON_GetObjectItemCaseSensitive(self, "DNSName");
    uint64 length = 0;
    uint64 index = 0;
    uint64 label = 0;
    unsigned char current = 0;

    if (!cJSON_IsString(state) || strcmp(state->valuestring, "Running") != 0)
    {
        printf("%s Start Tailscale and sign in before restarting Lemon Chat. Direct WS remains available.\n", g_mark_warn);
        return FALSE;
    }
    if (!cJSON_IsString(name))
    {
        return FALSE;
    }
    length = strlen(name->valuestring);
    if (length > 0 && name->valuestring[length - 1] == '.')
    {
        length--;
    }
    if (length <= 7 || length >= sizeof(g_tailscale_hostname))
    {
        return FALSE;
    }
    for (index = 0; index < length; index++)
    {
        current = (unsigned char)name->valuestring[index];
        if (current == '.')
        {
            if (label == 0 || name->valuestring[index - 1] == '-')
            {
                return FALSE;
            }
            label = 0;
        }
        else
        {
            if (!((current >= 'a' && current <= 'z') || (current >= 'A' && current <= 'Z') ||
                (current >= '0' && current <= '9') || (current == '-' && label != 0)))
            {
                return FALSE;
            }
            label++;
            if (label > 63)
            {
                return FALSE;
            }
        }
        g_tailscale_hostname[index] = (char)tolower(current);
    }
    g_tailscale_hostname[length] = 0;
    if (label == 0 || g_tailscale_hostname[length - 1] == '-' || strcmp(g_tailscale_hostname + length - 7, ".ts.net") != 0)
    {
        g_tailscale_hostname[0] = 0;
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief Checks whether a temporary Tailscale session owns the selected port.
 * @param cJSON* status Funnel configuration
 * @param int64 public_port Port required by Lemon Chat
 * @param uint depth Number of nested configurations already checked
 * @return boole Whether the port is occupied or the configuration is invalid
 */
static boole _tailscale_internal__has_foreground_port(cJSON* status, int64 public_port, uint depth)
{
    cJSON* foreground = cJSON_GetObjectItemCaseSensitive(status, "Foreground");
    cJSON* child = NULL_POINTER;
    cJSON* entry = NULL_POINTER;
    cJSON* tcp = NULL_POINTER;
    cJSON* web = NULL_POINTER;
    char port[16];
    char suffix[20];
    uint64 name_length = 0;
    uint64 suffix_length = 0;

    if (foreground == NULL_POINTER)
    {
        return FALSE;
    }
    if (!cJSON_IsObject(foreground) || depth >= 8)
    {
        return TRUE;
    }
    snprintf(port, sizeof(port), "%lld", public_port);
    snprintf(suffix, sizeof(suffix), ":%lld", public_port);
    suffix_length = strlen(suffix);
    cJSON_ArrayForEach(child, foreground)
    {
        if (!cJSON_IsObject(child))
        {
            return TRUE;
        }
        tcp = cJSON_GetObjectItemCaseSensitive(child, "TCP");
        web = cJSON_GetObjectItemCaseSensitive(child, "Web");
        if ((tcp != NULL_POINTER && !cJSON_IsObject(tcp)) || (web != NULL_POINTER && !cJSON_IsObject(web)) ||
            cJSON_GetObjectItemCaseSensitive(tcp, port) != NULL_POINTER)
        {
            return TRUE;
        }
        cJSON_ArrayForEach(entry, web)
        {
            if (entry->string == NULL_POINTER)
            {
                return TRUE;
            }
            name_length = strlen(entry->string);
            if (name_length >= suffix_length && strcmp(entry->string + name_length - suffix_length, suffix) == 0)
            {
                return TRUE;
            }
        }
        if (_tailscale_internal__has_foreground_port(child, public_port, depth + 1))
        {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Checks that the selected port has no conflicting service.
 * @param cJSON* status Funnel configuration
 * @param int64 public_port Public HTTPS port
 * @param cstring target Expected local HTTP address
 * @return int Negative for conflict or zero if ready or one if setup is needed
 */
static int _tailscale_internal__check_route(cJSON* status, int64 public_port, cstring target)
{
    char port[16];
    char endpoint[288];
    char suffix[20];
    uint64 name_length = 0;
    uint64 suffix_length = 0;
    cJSON* tcp_map = cJSON_GetObjectItemCaseSensitive(status, "TCP");
    cJSON* web_map = cJSON_GetObjectItemCaseSensitive(status, "Web");
    cJSON* public_map = cJSON_GetObjectItemCaseSensitive(status, "AllowFunnel");
    cJSON* tcp = NULL_POINTER;
    cJSON* web = NULL_POINTER;
    cJSON* handlers = NULL_POINTER;
    cJSON* root = NULL_POINTER;
    cJSON* proxy = NULL_POINTER;
    cJSON* item = NULL_POINTER;
    boole public = FALSE;
    boole root_matches = FALSE;

    snprintf(port, sizeof(port), "%lld", public_port);
    snprintf(endpoint, sizeof(endpoint), "%s:%lld", g_tailscale_hostname, public_port);
    snprintf(suffix, sizeof(suffix), ":%lld", public_port);
    suffix_length = strlen(suffix);
    if (_tailscale_internal__has_foreground_port(status, public_port, 0))
    {
        return -1;
    }
    if ((tcp_map != NULL_POINTER && !cJSON_IsObject(tcp_map)) ||
        (web_map != NULL_POINTER && !cJSON_IsObject(web_map)) ||
        (public_map != NULL_POINTER && !cJSON_IsObject(public_map)))
    {
        return -1;
    }
    cJSON_ArrayForEach(item, web_map)
    {
        if (item->string == NULL_POINTER)
        {
            return -1;
        }
        name_length = strlen(item->string);
        if (name_length >= suffix_length && strcmp(item->string + name_length - suffix_length, suffix) == 0 &&
            strcmp(item->string, endpoint) != 0)
        {
            return -1;
        }
    }
    tcp = cJSON_GetObjectItemCaseSensitive(tcp_map, port);
    web = cJSON_GetObjectItemCaseSensitive(web_map, endpoint);
    handlers = cJSON_GetObjectItemCaseSensitive(web, "Handlers");
    public = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(public_map, endpoint));
    if (tcp != NULL_POINTER && (!cJSON_IsObject(tcp) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tcp, "HTTPS")) ||
        cJSON_GetObjectItemCaseSensitive(tcp, "TCPForward") != NULL_POINTER))
    {
        return -1;
    }
    if (web != NULL_POINTER && (!cJSON_IsObject(web) || !cJSON_IsObject(handlers)))
    {
        return -1;
    }
    cJSON_ArrayForEach(item, handlers)
    {
        if (item->string == NULL_POINTER)
        {
            return -1;
        }
        if (strcmp(item->string, "/") != 0 && public == FALSE)
        {
            return -1;
        }
    }
    root = cJSON_GetObjectItemCaseSensitive(handlers, "/");
    proxy = cJSON_GetObjectItemCaseSensitive(root, "Proxy");
    if (root != NULL_POINTER)
    {
        if (!cJSON_IsString(proxy) || strcmp(proxy->valuestring, target) != 0 || cJSON_GetArraySize(root) != 1)
        {
            return -1;
        }
        root_matches = TRUE;
    }
    return (tcp != NULL_POINTER && root_matches == TRUE && public == TRUE) ? 0 : 1;
}

/**
 * @brief Enables one root path without resetting other Funnel routes.
 * @param int64 public_port public HTTPS port
 * @param cstring target local HTTP address
 * @return boole whether Tailscale accepted the command
 */
static boole _tailscale_internal__set_route(int64 public_port, cstring target)
{
    char port_argument[32];
    const char* arguments[] = {"funnel", "--bg", "--yes", port_argument, "--set-path=/", target, NULL_POINTER};
    char* output = NULL_POINTER;
    int result = -1;

    snprintf(port_argument, sizeof(port_argument), "--https=%lld", public_port);
    output = malloc(LEMON_TAILSCALE_OUTPUT_LIMIT + 1);
    if (output == NULL_POINTER)
    {
        return FALSE;
    }
    result = _tailscale_internal__run(arguments, output, LEMON_TAILSCALE_OUTPUT_LIMIT + 1);
    if (result != 0)
    {
        _tailscale_internal__print_error("Funnel setup", result, output);
    }
    free(output);
    return result == 0;
}

/**
 * @brief Configures public page and WebSocket routes when Funnel is enabled.
 * @return boole whether both configured public routes were verified
 */
boole tailscale__configure(void)
{
    const char* status_arguments[] = {"status", "--json", "--peers=false", NULL_POINTER};
    const char* funnel_arguments[] = {"funnel", "status", "--json", NULL_POINTER};
    cJSON* status = NULL_POINTER;
    char page_target[64];
    char chat_target[64];
    int page_state = -1;
    int chat_state = -1;

    g_tailscale_ready = FALSE;
    g_tailscale_hostname[0] = 0;
    if (g_server_settings.is_tailscale_funnel_enabled == FALSE)
    {
        return FALSE;
    }
#if defined(__ANDROID__)
    printf("%s Automatic Funnel setup requires the Tailscale command line program, which is not available in the Android app. Direct WS remains available.\n", g_mark_warn);
    return FALSE;
#endif
    if (g_server_settings.is_client_http_enabled == FALSE || g_server_settings.http_port < 1 || g_server_settings.http_port > 65535 ||
        g_server_settings.websocket_port < 1 || g_server_settings.websocket_port > 65535 ||
        (g_server_settings.wss_port != 8443 && g_server_settings.wss_port != 10000))
    {
        printf("%s Funnel needs the HTTP server enabled and public WSS port 8443 or 10000. Check server_settings.json.\n", g_mark_warn);
        return FALSE;
    }
    status = _tailscale_internal__read_json(status_arguments, "status", FALSE);
    if (status == NULL_POINTER)
    {
        return FALSE;
    }
    if (!_tailscale_internal__read_hostname(status))
    {
        cJSON_Delete(status);
        printf("%s A running Tailscale connection with a valid ts.net address is required for Funnel.\n", g_mark_warn);
        return FALSE;
    }
    cJSON_Delete(status);
    status = _tailscale_internal__read_json(funnel_arguments, "Funnel status", TRUE);
    if (status == NULL_POINTER)
    {
        return FALSE;
    }
    snprintf(page_target, sizeof(page_target), "http://127.0.0.1:%lld", g_server_settings.http_port);
    snprintf(chat_target, sizeof(chat_target), "http://127.0.0.1:%lld", g_server_settings.websocket_port);
    page_state = _tailscale_internal__check_route(status, 443, page_target);
    chat_state = _tailscale_internal__check_route(status, g_server_settings.wss_port, chat_target);
    cJSON_Delete(status);
    if (page_state < 0 || chat_state < 0)
    {
        printf("%s Funnel port %lld already has a conflicting service or private paths. Check tailscale funnel status and tailscale serve status. No routes were changed.\n",
            g_mark_warn, page_state < 0 ? (int64)443 : g_server_settings.wss_port);
        return FALSE;
    }
    if (page_state > 0 && !_tailscale_internal__set_route(443, page_target))
    {
        return FALSE;
    }
    if (chat_state > 0 && !_tailscale_internal__set_route(g_server_settings.wss_port, chat_target))
    {
        printf("%s The page route is configured but the chat route failed. Check tailscale funnel status.\n", g_mark_warn);
        return FALSE;
    }
    status = _tailscale_internal__read_json(funnel_arguments, "Funnel verification", TRUE);
    if (status != NULL_POINTER)
    {
        g_tailscale_ready = _tailscale_internal__check_route(status, 443, page_target) == 0 &&
            _tailscale_internal__check_route(status, g_server_settings.wss_port, chat_target) == 0;
        cJSON_Delete(status);
    }
    if (g_tailscale_ready == FALSE)
    {
        printf("%s Funnel routes could not be verified. Direct WS remains available.\n", g_mark_warn);
    }
    return g_tailscale_ready;
}

/**
 * @brief Reports whether both Funnel routes were verified at startup.
 * @return boole whether the public addresses are ready to print
 */
boole tailscale__is_ready(void)
{
    return g_tailscale_ready;
}

/**
 * @brief Prints the verified public page and WebSocket addresses.
 */
void tailscale__print_addresses(void)
{
    if (g_tailscale_ready == TRUE)
    {
        printf("  %s  public page        https://%s/\n", g_mark_info, g_tailscale_hostname);
        printf("  %s  public chat        wss://%s:%lld/\n", g_mark_info, g_tailscale_hostname, g_server_settings.wss_port);
    }
}
