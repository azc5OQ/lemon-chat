#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "attachments/attachment_file.h"
#include <stdlib.h>
#include "mbedtls/platform_util.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

/**
 * @brief reads only a regular, bounded file selected by the user; called on the attachment worker.
 * @param const char* path -> terminated user-selected UTF-8 path
 * @param uint64 limit -> maximum accepted byte count
 * @param uint64* size -> receives the exact count on success
 * @return unsigned char* -> owned bytes or NULL; release with free
 */
unsigned char* desktop_attachment_file__read(const char* path, uint64 limit, uint64* size)
{
    unsigned char* bytes = NULL;
    uint64 length = 0;
    uint64 offset = 0;
#ifdef _WIN32
    wchar_t wide[32768] = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER measured = {0};
    DWORD received = 0;
    DWORD chunk = 0;
#else
    int file = -1;
    struct stat measured = {0};
    ssize_t received = 0;
#endif
    *size = 0;
    if (!path || !path[0] || !limit)
    {
        return NULL;
    }
#ifdef _WIN32
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768))
    {
        goto label_finished;
    }
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &measured) || measured.QuadPart <= 0)
    {
        goto label_finished;
    }
    length = (uint64)measured.QuadPart;
#else
    file = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file < 0 || fstat(file, &measured) || !S_ISREG(measured.st_mode) || measured.st_size <= 0)
    {
        goto label_finished;
    }
    length = (uint64)measured.st_size;
#endif
    if (length > limit)
    {
        goto label_finished;
    }
    bytes = calloc(1, length);
    while (bytes && offset < length)
    {
#ifdef _WIN32
        chunk = (DWORD)(length - offset > 65536 ? 65536 : length - offset);
        if (!ReadFile(file, bytes + offset, chunk, &received, NULL) || !received)
        {
            break;
        }
#else
        received = read(file, bytes + offset, length - offset > 65536 ? 65536 : length - offset);
        if (received < 0 && errno == EINTR)
        {
            continue;
        }
        if (received <= 0)
        {
            break;
        }
#endif
        offset += (uint64)received;
    }
label_finished:
#ifdef _WIN32
    mbedtls_platform_zeroize(wide, sizeof(wide));
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
    }
#else
    if (file >= 0)
    {
        close(file);
    }
#endif
    if (!bytes || offset != length)
    {
        if (bytes)
        {
            mbedtls_platform_zeroize(bytes, length);
        }
        free(bytes);
        return NULL;
    }
    *size = length;
    return bytes;
}

/**
 * @brief saves to a user-chosen new path; an existing file is never overwritten or opened automatically.
 * @param const char* path -> exact user-selected UTF-8 destination
 * @param const unsigned char* bytes -> borrowed completed attachment
 * @param uint64 size -> byte count
 * @return boole -> TRUE when every byte was written
 */
boole desktop_attachment_file__save(const char* path, const unsigned char* bytes, uint64 size)
{
    uint64 offset = 0;
    boole complete = FALSE;
#ifdef _WIN32
    wchar_t wide[32768] = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0;
    DWORD chunk = 0;
#else
    int file = -1;
    ssize_t written = 0;
#endif
    if (!path || !path[0] || !bytes || !size)
    {
        return FALSE;
    }
#ifdef _WIN32
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768))
    {
        mbedtls_platform_zeroize(wide, sizeof(wide));
        return FALSE;
    }
    file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    mbedtls_platform_zeroize(wide, sizeof(wide));
    if (file == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    if (GetFileType(file) != FILE_TYPE_DISK)
    {
        CloseHandle(file);
        mbedtls_platform_zeroize(wide, sizeof(wide));
        return FALSE;
    }
#else
    file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0)
    {
        return FALSE;
    }
#endif
    while (offset < size)
    {
#ifdef _WIN32
        chunk = (DWORD)(size - offset > 65536 ? 65536 : size - offset);
        if (!WriteFile(file, bytes + offset, chunk, &written, NULL) || !written)
        {
            break;
        }
#else
        written = write(file, bytes + offset, size - offset > 65536 ? 65536 : size - offset);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            break;
        }
#endif
        offset += (uint64)written;
    }
#ifdef _WIN32
    complete = offset == size && FlushFileBuffers(file);
    CloseHandle(file);
#else
    complete = offset == size && fsync(file) == 0;
    close(file);
#endif
    return complete;
}

/**
 * @brief extracts a display basename, rejecting path separators and control characters from remote names.
 * @param char* name -> destination
 * @param uint64 capacity -> actual destination capacity
 * @param const char* source -> local path or untrusted remote name
 * @return void
 */
void desktop_attachment_file__name(char* name, uint64 capacity, const char* source)
{
    const char* start = source;
    uint64 index = 0;
    uint64 length = 0;
    if (!name || !capacity)
    {
        return;
    }
    clib__null_memory(name, capacity);
    if (source)
    {
        for (index = 0; source[index]; index++)
        {
            if (source[index] == '/' || source[index] == '\\')
            {
                start = source + index + 1;
            }
        }
        length = clib__strlen(start);
        if (length >= capacity)
        {
            length = capacity - 1;
            while (length && ((unsigned char)start[length] & 0xc0) == 0x80)
            {
                length--;
            }
        }
        clib__string_copy_n(name, capacity, start, length);
        for (index = 0; index < length; index++)
        {
            if ((unsigned char)name[index] < 32 || clib__strchr(":*?\"<>|", name[index]))
            {
                name[index] = '_';
            }
        }
        while (length && (name[length - 1] == '.' || name[length - 1] == ' '))
        {
            name[--length] = 0;
        }
    }
    if (!name[0])
    {
        clib__string_copy(name, capacity, "attachment");
    }
}

/**
 * @brief supplies the initial file-picker directory without changing the process working directory.
 * @param char* path -> destination
 * @param uint64 capacity -> actual destination capacity
 * @return boole -> TRUE when copied
 */
boole desktop_attachment_file__home(char* path, uint64 capacity)
{
#ifdef _WIN32
    wchar_t wide[32768] = {0};
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", wide, 32768);
    boole copied = FALSE;
    copied = length && length < 32768 && capacity <= 2147483647
        && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, path, (int)capacity, NULL, NULL) > 0;
    mbedtls_platform_zeroize(wide, sizeof(wide));
    return copied;
#else
    const char* home = getenv("HOME");
    return clib__string_copy(path, capacity, home ? home : "/");
#endif
}
