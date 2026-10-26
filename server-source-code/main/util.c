// This file provides helpers for clocks, threads, memory usage, executable lookup, random data, hashing and JSON.
#include "definitions.h"
#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "util.h"
#include "../third-party/zhicheng/base64.h"
#include "../third-party/ITH-sha/sha256.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/entropy.h"
#include "../third-party/mbedtls-3.6.6/include/mbedtls/ctr_drbg.h"
#include "../third-party/rxi-log/log.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach-o/dyld.h>
#endif
#endif

// declarations
static boole _util_internal__check_executable(cstring candidate, char* path, uint64 capacity);

/**
 * @brief Checks one executable file and returns its absolute path.
 * @param cstring candidate File to check
 * @param char* path Buffer receiving the absolute path
 * @param uint64 capacity Buffer size in bytes
 * @return boole Whether a usable executable was found
 */
static boole _util_internal__check_executable(cstring candidate, char* path, uint64 capacity)
{
    uint64 length = 0;
#ifdef _WIN32
    DWORD attributes = 0;
    DWORD binary_type = 0;
    DWORD required = 0;
    DWORD written = 0;
#else
    struct stat information;
    char* absolute = NULL_POINTER;
#endif

#ifdef _WIN32
    length = strlen(candidate);
    if (length < 4 || _stricmp(candidate + length - 4, ".exe") != 0)
    {
        return FALSE;
    }
    attributes = GetFileAttributesA(candidate);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !GetBinaryTypeA(candidate, &binary_type))
    {
        return FALSE;
    }
    if (binary_type != SCS_32BIT_BINARY && binary_type != SCS_64BIT_BINARY)
    {
        return FALSE;
    }
    required = GetFullPathNameA(candidate, 0, NULL_POINTER, NULL_POINTER);
    if (required == 0 || (uint64)required > capacity)
    {
        return FALSE;
    }
    written = GetFullPathNameA(candidate, required, path, NULL_POINTER);
    if (written == 0 || written >= required)
    {
        path[0] = 0;
        return FALSE;
    }
#else
    (void)length;
    if (stat(candidate, &information) != 0 || !S_ISREG(information.st_mode) || access(candidate, X_OK) != 0)
    {
        return FALSE;
    }
    absolute = realpath(candidate, NULL_POINTER);
    if (absolute == NULL_POINTER)
    {
        return FALSE;
    }
    length = strlen(absolute);
    if (length >= capacity)
    {
        free(absolute);
        return FALSE;
    }
    memcpy(path, absolute, length + 1);
    free(absolute);
#endif
    return TRUE;
}

/**
 * @brief Resolves an explicit executable path or searches the PATH directories.
 * @param cstring command Executable name or path without shell arguments
 * @param char* path Buffer receiving the absolute path
 * @param uint64 capacity Buffer size in bytes
 * @return boole Whether an executable was found and fit in the buffer
 */
boole util__find_executable(cstring command, char* path, uint64 capacity)
{
    const char* environment = NULL_POINTER;
    const char* start = NULL_POINTER;
    const char* cursor = NULL_POINTER;
    const char* end = NULL_POINTER;
    const char* basename = NULL_POINTER;
    const char* suffix = "";
    char* candidate = NULL_POINTER;
    uint64 command_length = 0;
    uint64 directory_length = 0;
    uint64 candidate_capacity = 0;
    uint64 suffix_length = 0;
    boole explicit_path = FALSE;
    boole found = FALSE;
    boole quoted = FALSE;
    char separator = ':';
    char slash = '/';

    if (path == NULL_POINTER || capacity == 0)
    {
        return FALSE;
    }
    path[0] = 0;
    if (command == NULL_POINTER || command[0] == 0)
    {
        return FALSE;
    }
    command_length = strlen(command);
    explicit_path = strchr(command, '/') != NULL_POINTER;
#ifdef _WIN32
    separator = ';';
    slash = '\\';
    explicit_path = explicit_path || strchr(command, '\\') != NULL_POINTER || strchr(command, ':') != NULL_POINTER;
    basename = command;
    for (cursor = command; *cursor != 0; cursor++)
    {
        if (*cursor == '/' || *cursor == '\\')
        {
            basename = cursor + 1;
        }
    }
    if (strchr(basename, '.') == NULL_POINTER)
    {
        suffix = ".exe";
    }
#else
    (void)basename;
#endif
    suffix_length = strlen(suffix);
    if (command_length > SIZE_MAX - suffix_length - 2)
    {
        return FALSE;
    }
    if (explicit_path == TRUE)
    {
        candidate = malloc((size_t)(command_length + suffix_length + 1));
        if (candidate == NULL_POINTER)
        {
            return FALSE;
        }
        memcpy(candidate, command, command_length);
        memcpy(candidate + command_length, suffix, suffix_length + 1);
        found = _util_internal__check_executable(candidate, path, capacity);
        free(candidate);
        return found;
    }
    environment = getenv("PATH");
    if (environment == NULL_POINTER || environment[0] == 0)
    {
        return FALSE;
    }
    start = environment;
    while (*start != 0)
    {
        cursor = start;
        quoted = FALSE;
        while (*cursor != 0)
        {
#ifdef _WIN32
            if (*cursor == '"')
            {
                quoted = !quoted;
            }
#endif
            if (*cursor == separator && quoted == FALSE)
            {
                break;
            }
            cursor++;
        }
        end = cursor;
#ifdef _WIN32
        if (end > start + 1 && *start == '"' && end[-1] == '"')
        {
            start++;
            end--;
        }
#endif
        directory_length = (uint64)(end - start);
        if (directory_length != 0 && directory_length <= SIZE_MAX - command_length - suffix_length - 2)
        {
            candidate_capacity = directory_length + command_length + suffix_length + 2;
            candidate = malloc((size_t)candidate_capacity);
            if (candidate == NULL_POINTER)
            {
                return FALSE;
            }
            memcpy(candidate, start, directory_length);
            candidate[directory_length] = slash;
            memcpy(candidate + directory_length + 1, command, command_length);
            memcpy(candidate + directory_length + 1 + command_length, suffix, suffix_length + 1);
            found = _util_internal__check_executable(candidate, path, capacity);
            free(candidate);
            if (found == TRUE)
            {
                return TRUE;
            }
        }
        if (*cursor == 0)
        {
            break;
        }
        start = cursor + 1;
    }
    return FALSE;
}

/**
 * @brief Reads the RAM currently occupied by this server process, including shared pages.
 * @param uint64* bytes -> receives the resident memory size in bytes, or zero on failure
 * @return boole -> TRUE when the operating system supplied a value
 */
boole util__get_process_ram_usage(uint64* bytes)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters = { 0 };
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

    if (first >= 'a' && first <= 'z')
    {
        first = (char)(first - 32);
    }
    if (second >= 'a' && second <= 'z')
    {
        second = (char)(second - 32);
    }

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

/**
 * @brief returns timestamp in milliseconds
 *
 *        on windows this is GetTickCount64 (milliseconds since boot), everywhere else it is
 *        gettimeofday (milliseconds since the unix epoch). only differences between two readings
 *        are meaningful, the absolute value is not comparable across platforms.
 *
 * @attention should work on windows and linux
 *
 * @return uint64 -> the current timestamp in milliseconds
 */
uint64 util__get_timestamp_ms(void)
{
#ifdef WIN32
    uint64 timestamp_msec = GetTickCount64();
    return timestamp_msec;
#else
    // every non-Windows platform (Linux, macOS, BSD) has POSIX gettimeofday. this MUST have an
    // #else, not an #ifdef __linux__: on macOS __linux__ is undefined, so a linux-only branch left
    // the function with no return - it fell off the end and returned garbage, which made every
    // spam-protected request (channel create/join/delete, chat, etc.) compare against a bogus
    // "now" and get silently rejected while auth (no spam check) still worked
    struct timeval tv;
    uint64 timestamp_msec = 0;
    gettimeofday(&tv, NULL_POINTER);
    timestamp_msec = (uint64)tv.tv_sec * 1000 + (uint64)tv.tv_usec / 1000;
    return timestamp_msec;
#endif
}

/**
 * @brief Reads a clock that is unaffected by changes to the calendar time.
 * @param uint64* milliseconds -> receives the clock reading
 * @return boole -> TRUE when the clock was read successfully
 */
boole util__get_monotonic_ms(uint64* milliseconds)
{
#ifdef WIN32
    *milliseconds = GetTickCount64();
#else
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return FALSE;
    }
    *milliseconds = (uint64)now.tv_sec * 1000 + (uint64)now.tv_nsec / 1000000;
#endif
    return TRUE;
}

/**
 * @brief Pauses the calling thread for the requested number of milliseconds.
 * @param uint64 milliseconds -> requested pause length
 * @return void
 */
void util__sleep_for_milliseconds(uint64 milliseconds)
{ // cross-platform sleep function
#ifdef WIN32
    Sleep(milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL_POINTER);
#else
    if (milliseconds >= 1000)
    {
        sleep(milliseconds / 1000);
    }
    usleep((milliseconds % 1000) * 1000);
#endif
}

#ifndef WIN32
/**
 * @brief Finds the executable directory so bundled files can be opened beside it.
 * @param char* out_directory -> buffer receiving the directory, or an empty string on failure
 * @param uint64 out_directory_size -> buffer capacity in bytes
 */
void util__get_executable_directory(char* out_directory, uint64 out_directory_size)
{
    char* last_slash = 0;
#if defined(__APPLE__)
    uint image_path_size = 0; // _NSGetExecutablePath takes a uint32_t* == unsigned int* == uint*
    char image_path[4096]; // raw (possibly relative/symlinked) path from the loader
    char resolved_path[4096]; // realpath output; realpath needs a PATH_MAX-sized buffer
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
/**
 * @brief Finds the executable directory so bundled files can be opened beside it.
 * @param char* out_directory -> buffer receiving the directory, or an empty string on failure
 * @param uint64 out_directory_size -> buffer capacity in bytes
 */
void util__get_executable_directory(char* out_directory, uint64 out_directory_size)
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
#endif
/**
 * @brief Fills a buffer with unpredictable random bytes for keys and login checks.
 * @param unsigned char* out_buffer -> buffer receiving the random bytes
 * @param uint64 length -> byte count, at most MBEDTLS_CTR_DRBG_MAX_REQUEST
 * @return boole -> TRUE on success; the caller must stop the operation on failure
 */
boole util__fill_secure_random_bytes(unsigned char* out_buffer, uint64 length)
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char* pers = "lemonchat_csprng";
    int ret = 0;
    boole result = TRUE;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers));
    if (ret != 0)
    {
        DBG_ENCRYPTION log_info("%s %d", "util__fill_secure_random_bytes: ctr_drbg_seed failed ", ret);
        result = FALSE;
    }

    if (result == TRUE)
    {
        ret = mbedtls_ctr_drbg_random(&ctr_drbg, out_buffer, length);
        if (ret != 0)
        {
            DBG_ENCRYPTION log_info("%s %d", "util__fill_secure_random_bytes: ctr_drbg_random failed ", ret);
            result = FALSE;
        }
    }

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return result;
}

/**
 * @brief Fills a buffer with unpredictable printable characters for a login challenge.
 * @param char* block -> buffer receiving the characters, without a string terminator
 * @param uint64 length -> number of characters to write
 * @return boole -> TRUE on success; the caller must reject the login on failure
 */
boole util__fill_block_of_data_with_ascii_characters(char* block, uint64 length)
{
    uint64 lowerbound = 33;
    uint64 upperbound = 126;
    uint64 i = 0;

    if (util__fill_secure_random_bytes((unsigned char*)block, length) == FALSE)
    {
        return FALSE;
    }

    for (i = 0; i < length; i++)
    {
        block[i] = (char)((unsigned char)block[i] % (upperbound - lowerbound + 1)) + lowerbound;
    }

    return TRUE;
}

/**
 * @brief Deletes a JSON object and frees its printed string. Either may be NULL_POINTER.
 * @param cJSON* json_root_object1 -> JSON object to delete
 * @param char* json_root_object1_string -> printed JSON string to free
 */
void util__free_json_message(cJSON* json_root_object1, char* json_root_object1_string)
{
    if (json_root_object1 != NULL_POINTER)
    {
        cJSON_Delete(json_root_object1);
    }

    if (json_root_object1_string != NULL_POINTER)
    {
        free(json_root_object1_string);
    }
}

/**
 * @brief SHA256-hashes a password then base64-encodes it, so auth secrets can be stored without plaintext.
 *
 * @param char* plaintext -> the password to hash
 * @param char* out -> caller buffer for the base64(SHA256) text (44 chars + NUL)
 * @param int64 out_size -> size of out in bytes (must be >= 45)
 *
 * @return void
 */
void util__hash_password_to_base64(char* plaintext, char* out, int64 out_size)
{
    ITH_SHA256_CTX ctx;
    unsigned char digest[32];
    char encoded[BASE64_ENCODE_OUT_SIZE(32)];

    ith_sha256_init(&ctx);
    ith_sha256_update(&ctx, (unsigned char*)plaintext, strlen(plaintext));
    ith_sha256_final(&ctx, digest);

    clib__null_memory(encoded, sizeof(encoded));
    zchg_base64_encode(digest, 32, encoded);

    clib__null_memory(out, out_size);
    clib__copy_memory(encoded, out, clib__utf8_string_length(encoded), out_size - 1);
}

/**
 * @brief checks a submitted plaintext password against a stored base64(SHA256) hash.
 *
 * @param char* plaintext -> the submitted password
 * @param char* stored_base64_hash -> the stored base64(SHA256) to compare against
 *
 * @return boole -> TRUE when they match
 */
boole util__password_matches(char* plaintext, char* stored_base64_hash)
{
    char computed[BASE64_ENCODE_OUT_SIZE(32)];

    util__hash_password_to_base64(plaintext, computed, sizeof(computed));
    return clib__is_string_equal(computed, stored_base64_hash);
}
