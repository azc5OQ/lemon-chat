// Starter of the portable Windows apps client-electron-amd64.exe, client-electron-arm64.exe and
// client-electron-i386.exe.
//
// package_electron.py appends the files of the Electron app folder to this program. At the first start of a
// package version the starter unpacks them to %LOCALAPPDATA%\LemonChat\electron-<build id> and shows a small
// window meanwhile. Later starts of the same version use that folder directly. The starter then runs
// lemon-chat.exe from that folder with the same command line arguments, waits for it and returns its exit code.
// After a successful unpack, the folders of older versions are removed unless their program is running.
//
// The appended data uses little-endian numbers. Each file is stored as a u32 path length, the path (UTF-8 with '/'
// separators), a u64 size, a u64 packed size and the packed bytes (raw deflate). The last 40 bytes are "LEMONPAK",
// the u64 offset of the first file, the u64 file count and the 16-character build id.
//
// package_electron.py builds it with the following command, with -target aarch64-windows-gnu for 64-bit ARM and
// -target x86-windows-gnu for 32-bit x86.
// zig cc -target x86_64-windows-gnu -O2 launcher.c miniz_tinfl.c -I <miniz folder> -DMINIZ_NO_STDIO
// -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_DEFLATE_APIS launcher.rc -o launcher.exe
// -Wl,--subsystem,windows -luser32 -lgdi32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "miniz.h"
#include "../definitions.h"

#define LAUNCHER_TRAILER_SIZE 40
#define LAUNCHER_PATH_SIZE 1024
#define LAUNCHER_COMMAND_SIZE 32768

// state shared by the unpack thread and the progress window
static HWND progress_window = NULL;
static volatile LONG is_unpack_successful = 0;

// the mapped program file and the unpack job
static const ubyte *package_data = NULL;
static uint64 package_offset = 0;
static uint64 package_count = 0;
static uint64 package_end = 0;
static wchar_t unpack_folder[LAUNCHER_PATH_SIZE];

/**
 * @brief reads a little-endian number of up to 8 bytes
 *
 * @param data -> first byte
 * @param size -> number of bytes
 *
 * @return uint64 -> the number
 */
static uint64 launcher__read_number(const ubyte *data, int size)
{
    uint64 value = 0;
    int index;

    // little-endian bytes
    for (index = size - 1; index >= 0; index--)
    {
        value = (value << 8) | data[index];
    }
    return value;
}

/**
 * @brief shows an error in a message box
 *
 * @param text -> the message
 *
 * @return void
 */
static void launcher__error(const wchar_t *text)
{
    // message box
    MessageBoxW(NULL, text, L"Lemon Chat", MB_OK | MB_ICONERROR);
}

/**
 * @brief joins two path parts with a backslash
 *
 * @param result -> receives the path, LAUNCHER_PATH_SIZE characters
 * @param first -> first part
 * @param second -> second part
 *
 * @return void
 */
static void launcher__join(wchar_t *result, const wchar_t *first, const wchar_t *second)
{
    // first\second
    _snwprintf(result, LAUNCHER_PATH_SIZE, L"%ls\\%ls", first, second);
    result[LAUNCHER_PATH_SIZE - 1] = L'\0';
}

/**
 * @brief creates every missing folder of a path
 *
 * @param path -> folder path with backslashes
 *
 * @return void
 */
static void launcher__create_folders(const wchar_t *path)
{
    wchar_t part[LAUNCHER_PATH_SIZE];
    nuint index;

    // each level
    wcsncpy(part, path, LAUNCHER_PATH_SIZE - 1);
    part[LAUNCHER_PATH_SIZE - 1] = L'\0';
    for (index = 3; part[index] != L'\0'; index++)
    {
        if (part[index] == L'\\')
        {
            part[index] = L'\0';
            CreateDirectoryW(part, NULL);
            part[index] = L'\\';
        }
    }
    CreateDirectoryW(part, NULL);
}

/**
 * @brief deletes a folder with all its content
 *
 * @param path -> folder path
 *
 * @return void
 */
static void launcher__delete_folder(const wchar_t *path)
{
    wchar_t pattern[LAUNCHER_PATH_SIZE];
    WIN32_FIND_DATAW entry;
    HANDLE search;

    // folder content
    launcher__join(pattern, path, L"*");
    search = FindFirstFileW(pattern, &entry);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            wchar_t child[LAUNCHER_PATH_SIZE];
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0)
            {
                continue;
            }
            launcher__join(child, path, entry.cFileName);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                launcher__delete_folder(child);
            }
            else
            {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }

    // the folder
    RemoveDirectoryW(path);
}

/**
 * @brief removes the folders of other package versions. A version whose lemon-chat.exe cannot be opened for
 *        exclusive access is still running and is kept.
 *
 * @param parent -> %LOCALAPPDATA%\LemonChat
 * @param current -> folder name of this version
 *
 * @return void
 */
static void launcher__remove_old_versions(const wchar_t *parent, const wchar_t *current)
{
    wchar_t pattern[LAUNCHER_PATH_SIZE];
    WIN32_FIND_DATAW entry;
    HANDLE search;

    // other version folders
    launcher__join(pattern, parent, L"electron-*");
    search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        wchar_t folder[LAUNCHER_PATH_SIZE];
        wchar_t program[LAUNCHER_PATH_SIZE];
        HANDLE file;

        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcscmp(entry.cFileName, current) == 0)
        {
            continue;
        }
        launcher__join(folder, parent, entry.cFileName);
        launcher__join(program, folder, L"lemon-chat.exe");

        // running versions stay
        file = CreateFileW(program, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
        {
            continue;
        }
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
        launcher__delete_folder(folder);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

/**
 * @brief writes inflated bytes to the open output file. tinfl calls it for each block.
 *
 * @param data -> inflated bytes
 * @param size -> number of bytes
 * @param user -> pointer to the output file handle
 *
 * @return int -> 1 to continue, 0 when writing failed
 */
static int launcher__write_block(const void *data, int size, void *user)
{
    DWORD written = 0;

    // block to file
    return WriteFile(*(HANDLE *)user, data, (DWORD)size, &written, NULL) && written == (DWORD)size;
}

/**
 * @brief unpack thread that inflates every appended file into unpack_folder and then closes the progress window
 *
 * @param unused -> thread parameter, not used
 *
 * @return DWORD -> 0
 */
static DWORD WINAPI launcher__unpack(LPVOID unused)
{
    uint64 position = package_offset;
    uint64 index;
    (void)unused;

    // every file
    for (index = 0; index < package_count; index++)
    {
        uint64 name_size;
        uint64 size;
        uint64 packed_size;
        char name[LAUNCHER_PATH_SIZE];
        wchar_t relative[LAUNCHER_PATH_SIZE];
        wchar_t path[LAUNCHER_PATH_SIZE];
        wchar_t *character;
        HANDLE file;
        LARGE_INTEGER length;
        size_t input_size;
        boole is_written;

        // file record
        if (position + 4 > package_end)
        {
            goto finished;
        }
        name_size = launcher__read_number(package_data + position, 4);
        position += 4;
        if (name_size == 0 || name_size >= sizeof(name) || position + name_size + 16 > package_end)
        {
            goto finished;
        }
        memcpy(name, package_data + position, (nuint)name_size);
        name[name_size] = '\0';
        position += name_size;
        size = launcher__read_number(package_data + position, 8);
        packed_size = launcher__read_number(package_data + position + 8, 8);
        position += 16;
        if (position + packed_size > package_end)
        {
            goto finished;
        }

        // output path
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, relative, LAUNCHER_PATH_SIZE) == 0 || wcsstr(relative, L"..") != NULL)
        {
            goto finished;
        }
        for (character = relative; *character != L'\0'; character++)
        {
            if (*character == L'/')
            {
                *character = L'\\';
            }
        }
        launcher__join(path, unpack_folder, relative);
        character = wcsrchr(path, L'\\');
        *character = L'\0';
        launcher__create_folders(path);
        *character = L'\\';

        // inflate to file
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            goto finished;
        }
        input_size = (size_t)packed_size;
        is_written = size == 0 || tinfl_decompress_mem_to_callback(package_data + position, &input_size, launcher__write_block, &file, 0) != 0;
        is_written = is_written && GetFileSizeEx(file, &length) && (uint64)length.QuadPart == size;
        CloseHandle(file);
        if (!is_written)
        {
            goto finished;
        }
        position += packed_size;
    }
    InterlockedExchange(&is_unpack_successful, 1);

    // close the window
finished:
    PostMessageW(progress_window, WM_CLOSE, 0, 0);
    return 0;
}

/**
 * @brief window procedure of the progress window
 *
 * @param window -> the window
 * @param message -> the message
 * @param wparam -> message parameter
 * @param lparam -> message parameter
 *
 * @return LRESULT -> message result
 */
static LRESULT CALLBACK launcher__window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    // close and quit
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/**
 * @brief unpacks the files while a small window says that the first start is being prepared
 *
 * @param instance -> program instance
 *
 * @return int -> 1 when every file was unpacked
 */
static int launcher__unpack_with_window(HINSTANCE instance)
{
    WNDCLASSW window_class;
    RECT area;
    HWND label;
    HANDLE thread;
    MSG message;

    // window class
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = launcher__window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_WAIT);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"LemonChatUnpack";
    RegisterClassW(&window_class);

    // centered window
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    progress_window = CreateWindowExW(WS_EX_TOPMOST, L"LemonChatUnpack", L"Lemon Chat", WS_CAPTION | WS_VISIBLE, (area.left + area.right - 380) / 2, (area.top + area.bottom - 120) / 2, 380, 120, NULL, NULL, instance, NULL);
    if (progress_window == NULL)
    {
        return 0;
    }
    label = CreateWindowW(L"STATIC", L"Preparing the first start of this version...", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 30, 350, 40, progress_window, NULL, instance, NULL);
    SendMessageW(label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    // unpack meanwhile
    thread = CreateThread(NULL, 0, launcher__unpack, NULL, 0, NULL);
    if (thread == NULL)
    {
        DestroyWindow(progress_window);
        return 0;
    }
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return is_unpack_successful != 0;
}

/**
 * @brief the command line arguments of this program after its own name
 *
 * @return const wchar_t* -> the arguments, possibly empty
 */
static const wchar_t *launcher__arguments(void)
{
    const wchar_t *text = GetCommandLineW();

    // skip the program name
    if (*text == L'"')
    {
        text++;
        while (*text != L'\0' && *text != L'"')
        {
            text++;
        }
        if (*text == L'"')
        {
            text++;
        }
    }
    else
    {
        while (*text != L'\0' && *text != L' ' && *text != L'\t')
        {
            text++;
        }
    }
    while (*text == L' ' || *text == L'\t')
    {
        text++;
    }
    return text;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
    static wchar_t command[LAUNCHER_COMMAND_SIZE];
    wchar_t own_path[LAUNCHER_PATH_SIZE];
    wchar_t local_folder[LAUNCHER_PATH_SIZE];
    wchar_t parent[LAUNCHER_PATH_SIZE];
    wchar_t folder_name[64];
    wchar_t folder[LAUNCHER_PATH_SIZE];
    wchar_t marker[LAUNCHER_PATH_SIZE];
    wchar_t partial[LAUNCHER_PATH_SIZE];
    wchar_t program[LAUNCHER_PATH_SIZE];
    const ubyte *trailer;
    HANDLE file;
    HANDLE mapping;
    LARGE_INTEGER file_size;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;
    int index;
    (void)previous;
    (void)command_line;
    (void)show;

    // own file mapped
    if (GetModuleFileNameW(NULL, own_path, LAUNCHER_PATH_SIZE) == 0)
    {
        launcher__error(L"The program cannot read its own file name.");
        return 1;
    }
    file = CreateFileW(own_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &file_size) || file_size.QuadPart < LAUNCHER_TRAILER_SIZE)
    {
        launcher__error(L"The program cannot read its own file.");
        return 1;
    }
    mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    package_data = mapping != NULL ? (const ubyte *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (package_data == NULL)
    {
        launcher__error(L"The program cannot read its own file.");
        return 1;
    }

    // package trailer
    trailer = package_data + file_size.QuadPart - LAUNCHER_TRAILER_SIZE;
    package_end = (uint64)file_size.QuadPart - LAUNCHER_TRAILER_SIZE;
    package_offset = launcher__read_number(trailer + 8, 8);
    package_count = launcher__read_number(trailer + 16, 8);
    if (memcmp(trailer, "LEMONPAK", 8) != 0 || package_offset >= package_end || package_count == 0)
    {
        launcher__error(L"This program contains no app files. Build it with package_electron.py.");
        return 1;
    }
    wcscpy(folder_name, L"electron-");
    for (index = 0; index < 16; index++)
    {
        char letter = (char)trailer[24 + index];
        if (!((letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f')))
        {
            launcher__error(L"The app files of this program are damaged.");
            return 1;
        }
        folder_name[9 + index] = (wchar_t)letter;
    }
    folder_name[25] = L'\0';

    // version folder
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_folder, LAUNCHER_PATH_SIZE) == 0)
    {
        GetTempPathW(LAUNCHER_PATH_SIZE, local_folder);
    }
    launcher__join(parent, local_folder, L"LemonChat");
    launcher__join(folder, parent, folder_name);
    launcher__join(marker, folder, L".complete");

    // first start unpacks
    if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES)
    {
        HANDLE marker_file;
        wchar_t partial_name[96];

        _snwprintf(partial_name, 96, L"%ls.partial-%lu", folder_name, GetCurrentProcessId());
        partial_name[95] = L'\0';
        launcher__join(partial, parent, partial_name);
        launcher__delete_folder(partial);
        launcher__create_folders(partial);
        wcsncpy(unpack_folder, partial, LAUNCHER_PATH_SIZE - 1);
        if (!launcher__unpack_with_window(instance))
        {
            launcher__delete_folder(partial);
            launcher__error(L"Lemon Chat could not be unpacked to %LOCALAPPDATA%\\LemonChat. Check the free disk space.");
            return 1;
        }
        launcher__join(program, partial, L".complete");
        marker_file = CreateFileW(program, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (marker_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(marker_file);
        }

        // finished folder in place
        if (!MoveFileExW(partial, folder, 0))
        {
            if (GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES)
            {
                launcher__delete_folder(folder);
                MoveFileExW(partial, folder, 0);
            }
            launcher__delete_folder(partial);
        }
        launcher__remove_old_versions(parent, folder_name);
    }
    UnmapViewOfFile(package_data);
    CloseHandle(mapping);
    CloseHandle(file);

    // start the app
    launcher__join(program, folder, L"lemon-chat.exe");
    _snwprintf(command, LAUNCHER_COMMAND_SIZE, L"\"%ls\" %ls", program, launcher__arguments());
    command[LAUNCHER_COMMAND_SIZE - 1] = L'\0';
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(program, command, NULL, NULL, FALSE, 0, NULL, folder, &startup, &process))
    {
        launcher__error(L"Lemon Chat could not be started from %LOCALAPPDATA%\\LemonChat.");
        return 1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)exit_code;
}
