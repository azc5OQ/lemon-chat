// Loads and saves local overlay preferences; changes apply only after an explicit save.
#include <windows.h>
#include <wchar.h>
#include <shlobj.h>
#include "settings/settings.h"
#include "clib.h"
#include "settings/settings_file.h"

// declarations
static boole _settings_internal__path(wchar_t *path, uint64 capacity, const wchar_t *filename);

/**
 * @brief creates the local settings directory and builds a bounded path inside it.
 *
 * @param wchar_t* path -> caller-owned path buffer
 * @param uint64 capacity -> destination capacity including its terminator
 * @param const wchar_t* filename -> settings filename suffix
 *
 * @return boole -> 1 on success; 0 on a path or filesystem error
 */
static boole _settings_internal__path(wchar_t *path, uint64 capacity, const wchar_t *filename)
{
    wchar_t local[MAX_PATH] = { 0 };
    uint64 length = 0;
    const wchar_t directory[] = L"\\LemonChat";
    uint64 index = 0;

    // find the user folder
    clib__null_memory(&local, sizeof(local));
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, local) != S_OK)
    {
        return 0;
    }

    // count the path length
    length = 0;
    while (length < MAX_PATH && local[length])
    {
        length++;
    }

    // check the buffer size
    if (length + sizeof(directory) / sizeof(*directory) >= capacity)
    {
        return 0;
    }

    // build the folder path
    clib__memcpy(path, local, length * sizeof(*path));
    clib__memcpy(path + length, directory, sizeof(directory));
    length += sizeof(directory) / sizeof(*directory) - 1;

    // create the folder
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    // add the file name
    for (index = 0;; index++)
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
boole desktop_settings_file__read(char *bytes, uint64 capacity, uint64 *length)
{
    wchar_t path[MAX_PATH] = { 0 };
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size = { 0 };
    DWORD received = 0;
    boole success = 0;

    // clear the output
    clib__null_memory(bytes, capacity);
    *length = 0;

    // build the file path
    if (!_settings_internal__path(path, MAX_PATH, L"\\desktop.json"))
    {
        return 0;
    }

    // open the file
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    // read the whole file
    success = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < capacity && ReadFile(file, bytes, (DWORD)size.QuadPart, &received, NULL) && received == size.QuadPart;
    CloseHandle(file);

    // store the length
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
boole desktop_settings_file__write(const char *bytes, uint64 length)
{
    static volatile LONG sequence = 0;
    wchar_t path[MAX_PATH] = { 0 };
    wchar_t temporary[MAX_PATH] = { 0 };
    wchar_t suffix[96];
    LARGE_INTEGER clock = { 0 };
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0;
    boole saved = 0;
    int attempt = 0;

    // check size and path
    if (length >= DESKTOP_SETTINGS_FILE_CAPACITY || !_settings_internal__path(path, MAX_PATH, L"\\desktop.json"))
    {
        return 0;
    }

    // create the temporary file
    for (attempt = 0; attempt < 32; attempt++)
    {
        // build a unique name
        QueryPerformanceCounter(&clock);
        swprintf(suffix, sizeof(suffix) / sizeof(*suffix), L"\\desktop.json.%lu.%llu.%lu.tmp", (unsigned long)GetCurrentProcessId(), (unsigned long long)clock.QuadPart, (unsigned long)InterlockedIncrement(&sequence));
        if (!_settings_internal__path(temporary, MAX_PATH, suffix))
        {
            return 0;
        }

        // create the new file
        file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            break;
        }

        // refuse other errors
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return 0;
        }
    }

    // no file was created
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    // write and close file
    saved = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, (DWORD)length, &written, NULL) && written == length && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file);
    }

    // replace the settings file
    if (saved)
    {
        saved = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    // delete on failure
    if (!saved)
    {
        DeleteFileW(temporary);
    }
    return saved;
}
