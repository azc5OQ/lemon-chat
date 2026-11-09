// Linux and macOS settings files, with private temporary files and atomic replacement.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "settings/settings_file.h"
#include "clib.h"

// declarations
static boole _settings_internal__path(char *path, uint64 capacity, boole create);

/**
 * @brief resolves the platform's per-user settings directory and optionally creates it.
 * @param char* path -> destination buffer
 * @param uint64 capacity -> destination capacity including the terminator
 * @param boole create -> nonzero when saving and directory creation is allowed
 * @return boole -> 1 on success; 0 on a missing home directory or a path error
 */
static boole _settings_internal__path(char *path, uint64 capacity, boole create)
{
    const char *home = NULL;
    const char *configuration = NULL;
    uint64 index = 0;
    uint64 length = 0;
    int result = 0;

    clib__null_memory(path, capacity);
    home = getenv("HOME");
#ifdef __APPLE__
    if (home && home[0] == '/')
    {
        result = clib__string_copy(path, capacity, home) && clib__string_append(path, capacity, "/Library/Application Support/LemonChat");
    }
#else
    configuration = getenv("XDG_CONFIG_HOME");
    if (configuration && configuration[0] == '/')
    {
        result = clib__string_copy(path, capacity, configuration) && clib__string_append(path, capacity, "/lemon-chat");
    }
    else if (home && home[0] == '/')
    {
        result = clib__string_copy(path, capacity, home) && clib__string_append(path, capacity, "/.config/lemon-chat");
    }
#endif
    (void)configuration;
    if (!result)
    {
        return 0;
    }
    if (create)
    {
        length = clib__strlen(path);
        for (index = 1; index <= length; index++)
        {
            if (path[index] == '/' || index == length)
            {
                path[index] = 0;
                result = mkdir(path, 0700) == 0 || errno == EEXIST;
                if (index < length)
                {
                    path[index] = '/';
                }
                if (!result)
                {
                    return 0;
                }
            }
        }
    }
    return clib__string_append(path, capacity, "/desktop.json");
}

/**
 * @brief reads a bounded regular settings file and leaves a terminating zero.
 * @param char* bytes -> destination buffer, cleared before reading
 * @param uint64 capacity -> buffer capacity including its terminator
 * @param uint64* length -> receives the number of bytes read
 * @return boole -> 1 on success; 0 if absent, oversized or unreadable
 */
boole desktop_settings_file__read(char *bytes, uint64 capacity, uint64 *length)
{
    char path[4096] = { 0 };
    struct stat information = { 0 };
    int file = -1;
    ssize_t received = 0;
    uint64 used = 0;
    boole success = 0;

    clib__null_memory(bytes, capacity);
    *length = 0;
    if (!_settings_internal__path(path, sizeof(path), 0))
    {
        return 0;
    }
    file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0)
    {
        return 0;
    }
    if (fstat(file, &information) == 0 && S_ISREG(information.st_mode) && information.st_size > 0 && (uint64)information.st_size < capacity)
    {
        while (used < (uint64)information.st_size)
        {
            received = read(file, bytes + used, (uint64)information.st_size - used);
            if (received < 0 && errno == EINTR)
            {
                continue;
            }
            if (received <= 0)
            {
                break;
            }
            used += (uint64)received;
        }
        success = used == (uint64)information.st_size;
    }
    close(file);
    if (success)
    {
        *length = used;
    }
    return success;
}

/**
 * @brief writes a private temporary file and atomically replaces the settings file.
 * @param const char* bytes -> serialized settings
 * @param uint64 length -> byte count
 * @return boole -> 1 when saved; 0 on a filesystem error
 */
boole desktop_settings_file__write(const char *bytes, uint64 length)
{
    char path[4096] = { 0 };
    char temporary[4096] = { 0 };
    int file = -1;
    ssize_t written = 0;
    uint64 used = 0;
    boole success = 0;

    if (length >= DESKTOP_SETTINGS_FILE_CAPACITY || !_settings_internal__path(path, sizeof(path), 1) || !clib__string_copy(temporary, sizeof(temporary), path) || !clib__string_append(temporary, sizeof(temporary), ".XXXXXX"))
    {
        return 0;
    }
    file = mkstemp(temporary);
    if (file < 0)
    {
        return 0;
    }
    while (used < length)
    {
        written = write(file, bytes + used, length - used);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            break;
        }
        used += (uint64)written;
    }
    success = used == length && fsync(file) == 0;
    if (close(file) != 0)
    {
        success = 0;
    }
    if (success)
    {
        success = rename(temporary, path) == 0;
    }
    if (!success)
    {
        unlink(temporary);
    }
    return success;
}
