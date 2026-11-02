// Loads and saves local overlay preferences; changes apply only after an explicit save.
#include <windows.h>
#include <shlobj.h>
#include "settings/settings.h"
#include "clib.h"
#include "settings/settings_file.h"

// declarations
static boole _settings_internal__path(wchar_t* path, uint64 capacity, const wchar_t* filename);

/**
 * @brief creates the local settings directory and builds a bounded path inside it.
 *
 * @param wchar_t* path -> caller-owned path buffer
 * @param uint64 capacity -> destination capacity including its terminator
 * @param const wchar_t* filename -> settings filename suffix
 *
 * @return boole -> 1 on success; 0 on a path or filesystem error
 */
static boole _settings_internal__path(wchar_t* path, uint64 capacity, const wchar_t* filename)
{
    wchar_t local[MAX_PATH] = {0};
    uint64 length = 0;
    const wchar_t directory[] = L"\\LemonChat";
    uint64 index = 0;

    clib__null_memory(&local, sizeof(local));
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, local) != S_OK)
    {
        return 0;
    }
    length = 0;
    while (length < MAX_PATH && local[length])
    {
        length++;
    }

    if (length + sizeof(directory) / sizeof(*directory) >= capacity)
    {
        return 0;
    }
    clib__memcpy(path, local, length * sizeof(*path));
    clib__memcpy(path + length, directory, sizeof(directory));
    length += sizeof(directory) / sizeof(*directory) - 1;
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return 0;
    }
    for (index = 0; ; index++)
    {
        if (length + index >= capacity)
        {
            return 0;
        }
        path[length + index] = filename[index];
        if (!filename[index])
        {
            return 1;
        }
    }
}

/**
 * @brief reads one bounded settings file through the Windows filesystem API.
 * @param char* bytes -> caller-owned buffer, cleared before use
 * @param uint64 capacity -> buffer capacity including its terminator
 * @param uint64* length -> receives the number of file bytes
 * @return boole -> 1 on success; 0 if missing, too large or unreadable
 */
boole desktop_settings_file__read(char* bytes, uint64 capacity, uint64* length)
{
    wchar_t path[MAX_PATH] = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size = {0};
    DWORD received = 0;
    boole success = 0;

    clib__null_memory(bytes, capacity);
    *length = 0;
    if (!_settings_internal__path(path, MAX_PATH, L"\\desktop.json"))
    {
        return 0;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    success = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < capacity
        && ReadFile(file, bytes, (DWORD)size.QuadPart, &received, NULL) && received == size.QuadPart;
    CloseHandle(file);
    if (success)
    {
        *length = received;
    }
    return success;
}

/**
 * @brief replaces saved settings only after a complete write and flush.
 * @param const char* bytes -> serialized settings
 * @param uint64 length -> byte count
 * @return boole -> 1 when saved; 0 on a filesystem error
 */
boole desktop_settings_file__write(const char* bytes, uint64 length)
{
    wchar_t path[MAX_PATH] = {0};
    wchar_t temporary[MAX_PATH] = {0};
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0;
    boole saved = 0;

    if (length >= DESKTOP_SETTINGS_FILE_CAPACITY || !_settings_internal__path(path, MAX_PATH, L"\\desktop.json")
        || !_settings_internal__path(temporary, MAX_PATH, L"\\desktop.json.tmp"))
    {
        return 0;
    }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    saved = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, (DWORD)length, &written, NULL)
        && written == length && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
    }
    if (saved)
    {
        saved = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!saved)
    {
        DeleteFileW(temporary);
    }
    return saved;
}
