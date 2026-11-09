// Linux/macOS clock and executable-directory access; string operations use Lemon clib.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "platform/system.h"
#include "platform/memory.h"
#include "clib.h"

/**
 * @brief reads monotonic elapsed time for display updates and timeouts.
 * @return uint64 -> monotonic time in milliseconds, or 0 on failure
 */
uint64 desktop_system__milliseconds(void)
{
    struct timespec now = { 0 };

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }
    return (uint64)now.tv_sec * 1000 + (uint64)now.tv_nsec / 1000000;
}

/**
 * @brief pauses a worker and resumes its remaining delay after a signal.
 * @param int64 milliseconds -> requested delay
 * @return void
 */
void desktop_system__sleep(int64 milliseconds)
{
    struct timespec delay = { 0 };

    if (milliseconds <= 0)
    {
        return;
    }
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    {
    }
}

/**
 * @brief resolves assets beside the actual executable, including symlink launches.
 * @return boole -> 1 on success; 0 on a path or directory error
 */
boole desktop_system__asset_directory(void)
{
    char path[4096] = { 0 };
    uint64 length = 0;
#ifdef __APPLE__
    char *resolved = NULL_POINTER;
    uint capacity = sizeof(path);
#else
    ssize_t received = 0;
#endif

#ifdef __APPLE__
    if (_NSGetExecutablePath(path, &capacity) != 0)
    {
        return 0;
    }
    resolved = realpath(path, NULL);
    if (!resolved)
    {
        return 0;
    }
    length = clib__string_copy(path, sizeof(path), resolved);
    desktop_memory__erase(resolved, clib__strlen(resolved) + 1);
    free(resolved);
    if (!length)
    {
        return 0;
    }
    length = clib__strlen(path);
#else
    received = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (received <= 0 || received >= sizeof(path) - 1)
    {
        return 0;
    }
    length = (uint64)received;
    path[length] = 0;
#endif
    while (length && path[length - 1] != '/')
    {
        length--;
    }
    if (!length)
    {
        return 0;
    }
    path[length] = 0;
    return chdir(path) == 0;
}

/**
 * @brief selects an installed desktop font, leaving the caller to supply a bundled fallback.
 * @param char* path -> destination path buffer
 * @param uint64 capacity -> destination capacity including the terminator
 * @param boole bold -> requested font weight
 * @return boole -> TRUE when a readable native font is available
 */
boole desktop_system__font_path(char *path, uint64 capacity, boole bold)
{
    const char *candidate = NULL_POINTER;
#ifdef __APPLE__
    candidate = bold ? "/System/Library/Fonts/Supplemental/Arial Bold.ttf" : "/System/Library/Fonts/Supplemental/Arial.ttf";
#else
    candidate = bold ? "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
    clib__null_memory(path, capacity);
    return access(candidate, R_OK) == 0 && clib__string_copy(path, capacity, candidate);
}

/**
 * @brief leaves native icon registration to the non-Windows application launcher.
 * @return void
 */
void desktop_system__window_icon(void)
{
}
