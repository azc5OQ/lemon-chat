// This file provides helpers for clocks, threads, memory usage, executable lookup, random data, hashing and JSON.
#include "definitions.h"
#include "clib/clib.h"
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
static boole _util_internal__check_executable(cstring candidate, char *path, uint64 capacity);

/**
 * @brief Checks one executable file and returns its absolute path.
 * @param cstring candidate File to check
 * @param char* path Buffer receiving the absolute path
 * @param uint64 capacity Buffer size in bytes
 * @return boole Whether a usable executable was found
 */
static boole _util_internal__check_executable(cstring candidate, char *path, uint64 capacity)
{
    uint64 length = 0;
#ifdef _WIN32
    uint64 attributes = 0;
    DWORD binary_type = 0; // GetBinaryTypeA writes a DWORD
    uint64 required = 0;
    uint64 written = 0;
#else
    struct stat information = { 0 };
    char *absolute = NULL_POINTER;
#endif

#ifdef _WIN32
    // check the exe ending
    length = clib__strlen(candidate);
    if (length < 4 || _stricmp(candidate + length - 4, ".exe") != 0)
    {
        return FALSE;
    }

    // check the file type
    attributes = GetFileAttributesA(candidate);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !GetBinaryTypeA(candidate, &binary_type))
    {
        return FALSE;
    }
    if (binary_type != SCS_32BIT_BINARY && binary_type != SCS_64BIT_BINARY)
    {
        return FALSE;
    }

    // write the absolute path
    required = GetFullPathNameA(candidate, 0, NULL_POINTER, NULL_POINTER);
    if (required == 0 || (uint64)required > capacity)
    {
        return FALSE;
    }
    written = GetFullPathNameA(candidate, (DWORD)required, path, NULL_POINTER);
    if (written == 0 || written >= required)
    {
        path[0] = 0;
        return FALSE;
    }
#else
    (void)length;

    // check file is executable
    if (stat(candidate, &information) != 0 || !S_ISREG(information.st_mode) || access(candidate, X_OK) != 0)
    {
        return FALSE;
    }

    // find the absolute path
    absolute = realpath(candidate, NULL_POINTER);
    if (absolute == NULL_POINTER)
    {
        return FALSE;
    }

    // copy the absolute path
    length = clib__strlen(absolute);
    if (length >= capacity)
    {
        free(absolute);
        return FALSE;
    }
    clib__memcpy(path, absolute, length + 1);
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
boole util__find_executable(cstring command, char *path, uint64 capacity)
{
    const char *environment = NULL_POINTER;
    const char *start = NULL_POINTER;
    const char *cursor = NULL_POINTER;
    const char *end = NULL_POINTER;
    const char *basename = NULL_POINTER;
    const char *suffix = "";
    char *candidate = NULL_POINTER;
    uint64 command_length = 0;
    uint64 directory_length = 0;
    uint64 candidate_capacity = 0;
    uint64 suffix_length = 0;
    boole explicit_path = FALSE;
    boole found = FALSE;
    boole quoted = FALSE;
    char separator = ':';
    char slash = '/';

    // check the buffer
    if (path == NULL_POINTER || capacity == 0)
    {
        return FALSE;
    }
    path[0] = 0;

    // check the command
    if (command == NULL_POINTER || command[0] == 0)
    {
        return FALSE;
    }

    // check for explicit path
    command_length = clib__strlen(command);
    explicit_path = clib__strchr(command, '/') != NULL_POINTER;
#ifdef _WIN32
    // use Windows separators
    separator = ';';
    slash = '\\';
    explicit_path = explicit_path || clib__strchr(command, '\\') != NULL_POINTER || clib__strchr(command, ':') != NULL_POINTER;

    // find the file name
    basename = command;
    for (cursor = command; *cursor != 0; cursor++)
    {
        if (*cursor == '/' || *cursor == '\\')
        {
            basename = cursor + 1;
        }
    }

    // add the exe ending
    if (clib__strchr(basename, '.') == NULL_POINTER)
    {
        suffix = ".exe";
    }
#else
    (void)basename;
#endif

    // check the total length
    suffix_length = clib__strlen(suffix);
    if (command_length > SIZE_MAX - suffix_length - 2)
    {
        return FALSE;
    }

    // check the explicit path
    if (explicit_path == TRUE)
    {
        candidate = malloc((uint64)(command_length + suffix_length + 1));
        if (candidate == NULL_POINTER)
        {
            return FALSE;
        }
        clib__memcpy(candidate, command, command_length);
        clib__memcpy(candidate + command_length, suffix, suffix_length + 1);
        found = _util_internal__check_executable(candidate, path, capacity);
        free(candidate);
        return found;
    }

    // read the PATH variable
    environment = getenv("PATH");
    if (environment == NULL_POINTER || environment[0] == 0)
    {
        return FALSE;
    }

    // search every PATH directory
    start = environment;
    while (*start != 0)
    {
        // find the directory end
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
        // remove surrounding quotes
        if (end > start + 1 && *start == '"' && end[-1] == '"')
        {
            start++;
            end--;
        }
#endif

        // check this directory
        directory_length = (uint64)(end - start);
        if (directory_length != 0 && directory_length <= SIZE_MAX - command_length - suffix_length - 2)
        {
            // allocate the candidate path
            candidate_capacity = directory_length + command_length + suffix_length + 2;
            candidate = malloc((uint64)candidate_capacity);
            if (candidate == NULL_POINTER)
            {
                return FALSE;
            }

            // build the candidate path
            clib__memcpy(candidate, start, directory_length);
            candidate[directory_length] = slash;
            clib__memcpy(candidate + directory_length + 1, command, command_length);
            clib__memcpy(candidate + directory_length + 1 + command_length, suffix, suffix_length + 1);

            // check the candidate file
            found = _util_internal__check_executable(candidate, path, capacity);
            free(candidate);
            if (found == TRUE)
            {
                return TRUE;
            }
        }

        // go to next directory
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
boole util__get_process_ram_usage(uint64 *bytes)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters = { 0 };
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info = { 0 };
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
#elif defined(__linux__)
    FILE *file = NULL_POINTER;
    uint64 virtual_pages = 0;
    uint64 resident_pages = 0;
    int64 page_size = 0;
    int64 fields = 0;
#endif

    // check arguments
    if (bytes == NULL_POINTER)
    {
        return FALSE;
    }
    *bytes = 0;
#ifdef _WIN32
    // read the Windows counters
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
        return FALSE;
    }
    *bytes = (uint64)counters.WorkingSetSize;
#elif defined(__APPLE__)
    // read the task info
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
    {
        return FALSE;
    }
    *bytes = (uint64)info.resident_size;
#elif defined(__linux__)
    // read the page counts
    page_size = sysconf(_SC_PAGESIZE);
    file = fopen("/proc/self/statm", "r");
    if (file == NULL_POINTER)
    {
        return FALSE;
    }
    fields = fscanf(file, "%llu %llu", &virtual_pages, &resident_pages);
    fclose(file);

    // convert pages to bytes
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
boole util__start_detached_thread(void *(*entry)(void *), void *argument)
{
    pthread_attr_t attributes = { 0 };
    pthread_t thread = { 0 };
    int64 result = 0;

    // create the thread attributes
    if (pthread_attr_init(&attributes) != 0)
    {
        return FALSE;
    }

    // start the detached thread
    result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (result == 0)
    {
        result = pthread_create(&thread, &attributes, entry, argument);
    }

    // free the attributes
    pthread_attr_destroy(&attributes);

    // log the failure
    if (result != 0)
    {
        DBG_UTIL log_info("%s %lld", "detached thread could not be started, error", result);
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
boole util__normalize_country_code(cJSON *json_entry, char *out_code)
{
    char first = 0;
    char second = 0;

    // check the JSON entry
    if (json_entry == NULL_POINTER || cJSON_IsString(json_entry) == FALSE || json_entry->valuestring == NULL_POINTER)
    {
        return FALSE;
    }

    // refuse wrong length
    if (clib__utf8_string_length(json_entry->valuestring) != 2)
    {
        DBG_UTIL log_info("%s %s", "country code rejected, not two characters:", json_entry->valuestring);
        return FALSE;
    }

    // read both letters
    first = json_entry->valuestring[0];
    second = json_entry->valuestring[1];

    // convert to upper case
    if (first >= 'a' && first <= 'z')
    {
        first = (char)(first - 32);
    }
    if (second >= 'a' && second <= 'z')
    {
        second = (char)(second - 32);
    }

    // allow letters only
    if (first < 'A' || first > 'Z' || second < 'A' || second > 'Z')
    {
        DBG_UTIL log_info("%s %s", "country code rejected, not two letters:", json_entry->valuestring);
        return FALSE;
    }

    // write the country code
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
    struct timeval tv = { 0 };
    uint64 timestamp_msec = 0;

    // read the system time
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
boole util__get_monotonic_ms(uint64 *milliseconds)
{
#ifdef WIN32
    *milliseconds = GetTickCount64();
#else
    struct timespec now = { 0 };

    // read the monotonic clock
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
    const uint64 maximum_delay = (uint64)INFINITE - 1;

    // keep waits finite
    while (milliseconds > maximum_delay)
    {
        Sleep((DWORD)maximum_delay);
        milliseconds -= maximum_delay;
    }

    // wait the remaining time
    Sleep((DWORD)milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts = { 0 };

    // convert and wait
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL_POINTER);
#else
    // wait the whole seconds
    if (milliseconds >= 1000)
    {
        sleep(milliseconds / 1000);
    }

    // wait the remaining part
    usleep((milliseconds % 1000) * 1000);
#endif
}

#ifndef WIN32
/**
 * @brief Finds the executable directory so bundled files can be opened beside it.
 * @param char* out_directory -> buffer receiving the directory, or an empty string on failure
 * @param uint64 out_directory_size -> buffer capacity in bytes
 */
void util__get_executable_directory(char *out_directory, uint64 out_directory_size)
{
    const char *last_slash = NULL_POINTER;
#if defined(__APPLE__)
    uint image_path_size = 0; // _NSGetExecutablePath takes a pointer to an unsigned 32-bit capacity
    char image_path[4096] = { 0 }; // raw (possibly relative/symlinked) path from the loader
    char resolved_path[4096] = { 0 }; // realpath output; realpath needs a PATH_MAX-sized buffer
#else
    int64 link_length = 0;
#endif

    // check arguments
    if (out_directory == NULL_POINTER || out_directory_size == 0)
    {
        return;
    }

    // refuse too large size
    if (out_directory_size > SIZE_MAX)
    {
        out_directory[0] = 0;
        return;
    }

#if defined(__APPLE__)
    // macOS has no /proc; the dynamic loader hands back the executable's path, then realpath
    // canonicalizes it to an absolute path so sibling files (stunnel.conf, the binary) resolve
    image_path_size = (uint)sizeof(image_path);
    if (_NSGetExecutablePath(image_path, &image_path_size) != 0)
    {
        out_directory[0] = 0;
        return;
    }

    // convert to absolute path
    if (realpath(image_path, resolved_path) == NULL_POINTER)
    {
        out_directory[0] = 0;
        return;
    }

    // copy the full path
    clib__string_format(out_directory, out_directory_size, "%s", resolved_path);
#else
    // read the executable link
    link_length = readlink("/proc/self/exe", out_directory, out_directory_size - 1);
    if (link_length <= 0)
    {
        out_directory[0] = 0;
        return;
    }

    // add the null terminator
    out_directory[link_length] = 0;
#endif

    // remove the file name
    last_slash = clib__strrchr(out_directory, '/');
    if (last_slash != NULL_POINTER)
    {
        out_directory[last_slash - out_directory] = 0;
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
void util__get_executable_directory(char *out_directory, uint64 out_directory_size)
{
    char *last_separator = NULL_POINTER;
    uint64 length = 0;
    uint64 capacity = 0;

    // check arguments
    if (out_directory == NULL_POINTER || out_directory_size == 0)
    {
        return;
    }

    // read the executable path
    capacity = out_directory_size > MAXDWORD ? MAXDWORD : out_directory_size;
    length = GetModuleFileNameA(NULL_POINTER, out_directory, (DWORD)capacity);
    if (length == 0 || length >= capacity)
    {
        out_directory[0] = 0;
        return;
    }

    // remove the file name
    last_separator = (char *)clib__strrchr(out_directory, '\\');
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
 * @param ubyte* out_buffer -> buffer receiving the random bytes
 * @param uint64 length -> byte count, at most MBEDTLS_CTR_DRBG_MAX_REQUEST
 * @return boole -> TRUE on success; the caller must stop the operation on failure
 */
boole util__fill_secure_random_bytes(ubyte *out_buffer, uint64 length)
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "lemonchat_csprng";
    int64 ret = 0;
    boole result = TRUE;

    // check arguments
    if (length > MBEDTLS_CTR_DRBG_MAX_REQUEST || (out_buffer == NULL_POINTER && length != 0))
    {
        return FALSE;
    }

    // create the random generator
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    // seed the generator
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const ubyte *)pers, clib__strlen(pers));
    if (ret != 0)
    {
        DBG_ENCRYPTION log_info("%s %lld", "util__fill_secure_random_bytes: ctr_drbg_seed failed ", ret);
        result = FALSE;
    }

    // write the random bytes
    if (result == TRUE)
    {
        ret = mbedtls_ctr_drbg_random(&ctr_drbg, out_buffer, length);
        if (ret != 0)
        {
            DBG_ENCRYPTION log_info("%s %lld", "util__fill_secure_random_bytes: ctr_drbg_random failed ", ret);
            result = FALSE;
        }
    }

    // free the generator
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
boole util__fill_block_of_data_with_ascii_characters(char *block, uint64 length)
{
    uint64 lowerbound = 33;
    uint64 upperbound = 126;
    uint64 i = 0;

    // fill with random bytes
    if (util__fill_secure_random_bytes((ubyte *)block, length) == FALSE)
    {
        return FALSE;
    }

    // convert to printable characters
    for (i = 0; i < length; i++)
    {
        block[i] = (char)((ubyte)block[i] % (upperbound - lowerbound + 1)) + lowerbound;
    }

    return TRUE;
}

/**
 * @brief Deletes a JSON object and frees its printed string. Either may be NULL_POINTER.
 * @param cJSON* json_root_object1 -> JSON object to delete
 * @param char* json_root_object1_string -> printed JSON string to free
 */
void util__free_json_message(cJSON *json_root_object1, char *json_root_object1_string)
{
    // delete the JSON object
    if (json_root_object1 != NULL_POINTER)
    {
        util__json_delete_secure(json_root_object1);
    }

    // free the printed string
    if (json_root_object1_string != NULL_POINTER)
    {
        clib__null_memory(json_root_object1_string, strlen(json_root_object1_string));
        cJSON_free(json_root_object1_string);
    }
}

/**
 * @brief SHA256-hashes a password then base64-encodes it, so auth secrets can be stored without
 *        plaintext.
 *
 * @param char* plaintext -> the password to hash
 * @param char* out -> caller buffer for the base64(SHA256) text (44 chars + NUL)
 * @param int64 out_size -> size of out in bytes (must be >= 45)
 *
 * @return void
 */
void util__hash_password_to_base64(cstring plaintext, char *out, int64 out_size)
{
    ITH_SHA256_CTX ctx;
    ubyte digest[32] = { 0 };
    char encoded[BASE64_ENCODE_OUT_SIZE(32)] = { 0 };

    // hash the password
    ith_sha256_init(&ctx);
    ith_sha256_update(&ctx, (ubyte *)plaintext, clib__strlen(plaintext));
    ith_sha256_final(&ctx, digest);

    // convert to base64
    clib__null_memory(encoded, sizeof(encoded));
    zchg_base64_encode(digest, 32, encoded);

    // write the result
    clib__null_memory(out, out_size);
    clib__string_copy(out, out_size, encoded);
}

/**
 * @brief checks a submitted plaintext password against a stored base64(SHA256) hash.
 *
 * @param char* plaintext -> the submitted password
 * @param char* stored_base64_hash -> the stored base64(SHA256) to compare against
 *
 * @return boole -> TRUE when they match
 */
boole util__password_matches(char *plaintext, char *stored_base64_hash)
{
    char computed[BASE64_ENCODE_OUT_SIZE(32)] = { 0 };

    util__hash_password_to_base64(plaintext, computed, sizeof(computed));
    return clib__is_string_equal(computed, stored_base64_hash);
}

/**
 * @brief Reads a JSON integer only after validating its type and conversion range.
 * @attention A failed read leaves the destination unchanged.
 */
boole util__json_read_int64(const cJSON *value, int64 minimum, int64 maximum, int64 *result)
{
    double number = 0;
    int64 parsed = 0;

    // check arguments
    if (result == NULL_POINTER || !cJSON_IsNumber(value) || minimum > maximum)
    {
        return FALSE;
    }

    // check the number range
    number = value->valuedouble;
    if (!(number >= -9223372036854775808.0 && number < 9223372036854775808.0))
    {
        return FALSE;
    }

    // check fraction and limits
    parsed = (int64)number;
    if ((double)parsed != number || parsed < minimum || parsed > maximum)
    {
        return FALSE;
    }

    // write the result
    *result = parsed;
    return TRUE;
}

/**
 * @brief Reads an unsigned JSON integer without fractional or out-of-range casts.
 * @attention A failed read leaves the destination unchanged.
 */
boole util__json_read_uint64(const cJSON *value, uint64 maximum, uint64 *result)
{
    double number = 0;
    uint64 parsed = 0;

    // check arguments
    if (result == NULL_POINTER || !cJSON_IsNumber(value))
    {
        return FALSE;
    }

    // check the number range
    number = value->valuedouble;
    if (!(number >= 0.0 && number < 18446744073709551616.0))
    {
        return FALSE;
    }

    // check fraction and limits
    parsed = (uint64)number;
    if ((double)parsed != number || parsed > maximum)
    {
        return FALSE;
    }

    // write the result
    *result = parsed;
    return TRUE;
}

/**
 * @brief Clears owned JSON strings before releasing a tree.
 * @attention Borrowed strings and referenced child trees remain owned by their caller.
 */
void util__json_delete_secure(cJSON *root)
{
    cJSON *child = NULL_POINTER;

    // nothing to delete
    if (root == NULL_POINTER)
    {
        return;
    }

    // clear the owned content
    if (!(root->type & cJSON_IsReference))
    {
        while (root->child != NULL_POINTER)
        {
            child = cJSON_DetachItemViaPointer(root, root->child);
            util__json_delete_secure(child);
        }
        if (root->valuestring != NULL_POINTER)
        {
            clib__null_memory(root->valuestring, strlen(root->valuestring));
        }
    }

    // clear the item name
    if (!(root->type & cJSON_StringIsConst) && root->string != NULL_POINTER)
    {
        clib__null_memory(root->string, strlen(root->string));
    }

    // free the item
    cJSON_Delete(root);
}

/** @brief Transfers ownership to the object, or releases the child on failure. */
boole util__json_add_owned_object(cJSON *object, const char *name, cJSON *child)
{
    if (!cJSON_AddItemToObject(object, name, child))
    {
        util__json_delete_secure(child);
        return FALSE;
    }
    return TRUE;
}

/** @brief Transfers ownership to the array, or releases the child on failure. */
boole util__json_add_owned_array(cJSON *array, cJSON *child)
{
    if (!cJSON_AddItemToArray(array, child))
    {
        util__json_delete_secure(child);
        return FALSE;
    }
    return TRUE;
}

/** @brief Clears the owned contents of a removed JSON property. */
void util__json_delete_item_secure(cJSON *object, const char *name)
{
    util__json_delete_secure(cJSON_DetachItemFromObjectCaseSensitive(object, name));
}
