// Windows clock, executable directory and installed-font access.
#include <windows.h>
#include "platform/system.h"
#include "clib.h"

// declarations
static BOOL CALLBACK _system_internal__window_icon(HWND window, LPARAM context);

/**
 * @brief applies process-shared icon resources to the application's windows on the UI thread.
 * @param HWND window -> window owned by this thread
 * @param LPARAM context -> unused callback context
 * @return BOOL -> TRUE to continue enumeration
 */
static BOOL CALLBACK _system_internal__window_icon(HWND window, LPARAM context)
{
    HINSTANCE module = GetModuleHandleW(NULL);
    HICON small = LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    HICON large = LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    (void)context;
    if (small)
    {
        SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)small);
    }
    if (large)
    {
        SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)large);
    }
    return TRUE;
}

/**
 * @brief loads the executable's embedded lemon icon after the native window is created.
 * @return void
 */
void desktop_system__window_icon(void)
{
    EnumThreadWindows(GetCurrentThreadId(), _system_internal__window_icon, 0);
}

/**
 * @brief reads monotonic elapsed time for display updates and timeouts.
 * @return uint64 -> milliseconds since system startup
 */
uint64 desktop_system__milliseconds(void)
{
    return GetTickCount64();
}

/**
 * @brief pauses a development probe or worker without spinning.
 * @param int64 milliseconds -> requested delay
 * @return void
 */
void desktop_system__sleep(int64 milliseconds)
{
    DWORD chunk = 0;
    while (milliseconds > 0)
    {
        chunk = milliseconds > 4294967294LL ? 4294967294UL : (DWORD)milliseconds;
        Sleep(chunk);
        milliseconds -= chunk;
    }
}

/**
 * @brief resolves assets beside the executable regardless of its launch directory.
 * @return boole -> 1 on success; 0 on a path or directory error
 */
boole desktop_system__asset_directory(void)
{
    wchar_t path[32768] = { 0 };
    DWORD length = 0;

    length = GetModuleFileNameW(NULL, path, sizeof(path) / sizeof(path[0]));
    if (!length || length >= sizeof(path) / sizeof(path[0]))
    {
        return 0;
    }
    while (length && path[length - 1] != L'\\' && path[length - 1] != L'/')
    {
        length--;
    }
    if (!length)
    {
        return 0;
    }
    path[length] = 0;
    return SetCurrentDirectoryW(path) != 0;
}

/**
 * @brief finds installed Segoe UI without redistributing Windows font files.
 * @param char* path -> destination path buffer
 * @param uint64 capacity -> destination capacity including the terminator
 * @param boole bold -> nonzero for the bold font
 * @return boole -> 1 when a complete path was written; 0 otherwise
 */
boole desktop_system__font_path(char *path, uint64 capacity, boole bold)
{
    wchar_t windows_directory[MAX_PATH] = { 0 };
    char directory[MAX_PATH * 3] = { 0 };
    UINT length = 0;

    clib__null_memory(path, capacity);
    length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (!length || length >= MAX_PATH || !WideCharToMultiByte(CP_UTF8, 0, windows_directory, -1, directory, sizeof(directory), NULL, NULL))
    {
        return 0;
    }
    return clib__string_copy(path, capacity, directory) && clib__string_append(path, capacity, "/Fonts/") && clib__string_append(path, capacity, bold ? "segoeuib.ttf" : "segoeui.ttf");
}
