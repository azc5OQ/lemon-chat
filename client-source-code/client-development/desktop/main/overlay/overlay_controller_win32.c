#include "platform/memory.h"
// Watches one configured game and manages opt-in overlay loading and display updates.
#include <windows.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include "overlay_controller.h"
#include "overlay_ipc.h"
#include "clib.h"

struct desktop_overlay_controller_t
{
    CRITICAL_SECTION lock;
    HANDLE stop;
    HANDLE thread;
    desktop_settings_t settings;
    char active_game[DESKTOP_GAME_NAME_CAPACITY];
    desktop_overlay_snapshot_t snapshot;
    char status[256];
};

// declarations
static void _overlay_controller_internal__status(desktop_overlay_controller_t *controller, const char *message, DWORD error);
static boole _overlay_controller_internal__publish(desktop_overlay_controller_t *controller, desktop_overlay_ipc_t *ipc);
static nuint _overlay_controller_internal__module(DWORD process_id, const wchar_t *name);
static DWORD _overlay_controller_internal__find_game(const char *executable);
static boole _overlay_controller_internal__same_module_path(DWORD process_id, nuint address, const wchar_t *expected);
static DWORD _overlay_controller_internal__wait(desktop_overlay_controller_t *controller, desktop_overlay_ipc_t *ipc, HANDLE thread);
static boole _overlay_controller_internal__attach(desktop_overlay_controller_t *controller, desktop_overlay_ipc_t *ipc, DWORD process_id);
static DWORD WINAPI _overlay_controller_internal__worker(void *parameter);

/**
 * @brief updates the controller status while holding its lock.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param const char* message -> message text
 * @param DWORD error -> Windows error code, or zero
 *
 * @return void
 */
static void _overlay_controller_internal__status(desktop_overlay_controller_t *controller, const char *message, DWORD error)
{
    // set the status text
    EnterCriticalSection(&controller->lock);
    if (error)
    {
        clib__string_format(controller->status, sizeof(controller->status), "%s (Windows error %u)", message, error);
    }
    else
    {
        clib__string_copy(controller->status, sizeof(controller->status), message);
    }
    LeaveCriticalSection(&controller->lock);
}

/**
 * @brief publishes display data only for the currently selected, enabled game.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param desktop_overlay_ipc_t* ipc -> shared display mapping handle
 *
 * @return boole -> 1 when an enabled snapshot was published; 0 otherwise
 */
static boole _overlay_controller_internal__publish(desktop_overlay_controller_t *controller, desktop_overlay_ipc_t *ipc)
{
    desktop_overlay_snapshot_t snapshot = { 0 };

    // copy the snapshot
    EnterCriticalSection(&controller->lock);
    snapshot = controller->snapshot;
    snapshot.enabled = controller->settings.overlay_enabled && (snapshot.enabled || snapshot.preview) && clib__strcmp(controller->settings.game_executable, controller->active_game) == 0 && WaitForSingleObject(controller->stop, 0) == WAIT_TIMEOUT;
    LeaveCriticalSection(&controller->lock);

    // write the snapshot
    return desktop_overlay_ipc__write(ipc, &snapshot) && snapshot.enabled;
}

/**
 * @brief finds a loaded module in the selected process.
 *
 * @param DWORD process_id -> selected game process ID
 * @param const wchar_t* name -> module basename to look up
 *
 * @return nuint -> the remote module base address, or 0 if it cannot be found
 */
static nuint _overlay_controller_internal__module(DWORD process_id, const wchar_t *name)
{
    HANDLE snapshot = NULL;
    MODULEENTRY32W module = { 0 };
    nuint address = 0;

    // list the process modules
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    // find the module name
    clib__null_memory(&module, sizeof(module));
    module.dwSize = sizeof(module);
    address = 0;
    if (Module32FirstW(snapshot, &module))
    {
        do
        {
            if (CompareStringOrdinal(module.szModule, -1, name, -1, TRUE) == CSTR_EQUAL)
            {
                address = (nuint)module.modBaseAddr;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }

    // close the list
    CloseHandle(snapshot);
    return address;
}

/**
 * @brief finds the configured executable in this Windows session, excluding the client itself.
 *
 * @param const char* executable -> game executable basename
 *
 * @return DWORD -> the process ID, 0 if absent, or MAXDWORD if more than one process matches
 */
static DWORD _overlay_controller_internal__find_game(const char *executable)
{
    wchar_t name[DESKTOP_GAME_NAME_CAPACITY] = { 0 };
    HANDLE snapshot = NULL;
    DWORD found = 0;
    DWORD own_session = 0;
    PROCESSENTRY32W process = { 0 };
    DWORD session = 0;

    // convert the name
    clib__null_memory(&name, sizeof(name));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, executable, -1, name, DESKTOP_GAME_NAME_CAPACITY))
    {
        return 0;
    }

    // list the processes
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    // read the client session
    found = 0;
    own_session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &own_session);

    // find the game process
    process.dwSize = sizeof(process);
    if (Process32FirstW(snapshot, &process))
    {
        do
        {
            session = 0;
            if (process.th32ProcessID != GetCurrentProcessId() && CompareStringOrdinal(process.szExeFile, -1, name, -1, TRUE) == CSTR_EQUAL && ProcessIdToSessionId(process.th32ProcessID, &session) && session == own_session)
            {
                if (found)
                {
                    found = MAXDWORD;
                    break;
                }
                found = process.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &process));
    }

    // close the list
    CloseHandle(snapshot);
    return found;
}

/**
 * @brief checks that the remote overlay was loaded from this client installation.
 *
 * @param DWORD process_id -> selected game process ID
 * @param nuint address -> remote module base address
 * @param const wchar_t* expected -> expected full module path
 *
 * @return boole -> 1 when the module paths match; 0 otherwise
 */
static boole _overlay_controller_internal__same_module_path(DWORD process_id, nuint address, const wchar_t *expected)
{
    HANDLE snapshot = NULL;
    MODULEENTRY32W module = { 0 };
    boole matches = 0;

    // list the process modules
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    // compare the module path
    clib__null_memory(&module, sizeof(module));
    module.dwSize = sizeof(module);
    matches = 0;
    if (Module32FirstW(snapshot, &module))
    {
        do
        {
            if ((nuint)module.modBaseAddr == address)
            {
                matches = CompareStringOrdinal(module.szExePath, -1, expected, -1, TRUE) == CSTR_EQUAL;
                break;
            }
        } while (Module32NextW(snapshot, &module));
    }

    // close the list
    CloseHandle(snapshot);
    return matches;
}

// Keep the data heartbeat current during a bounded loader call. Never block the UI.
/**
 * @brief waits at most five seconds for a loader call while refreshing the display heartbeat.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param desktop_overlay_ipc_t* ipc -> shared display mapping handle
 * @param HANDLE thread -> remote loader or startup thread
 *
 * @return DWORD -> the Windows wait result
 */
static DWORD _overlay_controller_internal__wait(desktop_overlay_controller_t *controller, desktop_overlay_ipc_t *ipc, HANDLE thread)
{
    ULONGLONG deadline = 0;
    DWORD result = 0;

    // set the time limit
    deadline = GetTickCount64() + 5000;

    // wait for the thread
    do
    {
        result = WaitForSingleObject(thread, 50);
        _overlay_controller_internal__publish(controller, ipc);
    } while (result == WAIT_TIMEOUT && GetTickCount64() < deadline);
    return result;
}

/**
 * @brief loads the selected game overlay and starts it after the Windows loader finishes.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param desktop_overlay_ipc_t* ipc -> shared display mapping handle
 * @param DWORD process_id -> selected game process ID
 *
 * @return boole -> 1 when the overlay started; 0 on failure or cancellation
 */
static boole _overlay_controller_internal__attach(desktop_overlay_controller_t *controller, desktop_overlay_ipc_t *ipc, DWORD process_id)
{
    const wchar_t *dll_name = NULL_POINTER;
    wchar_t dll_path[32768] = { 0 };
    DWORD length = 0;
    uint64 index = 0;
    HANDLE process = NULL;
    BOOL own_wow = 0;
    BOOL game_wow = 0;
    boole attached = 0;
    nuint remote_dll = 0;
    FARPROC loader = NULL;
    HMODULE owner = NULL;
    wchar_t owner_path[32768] = { 0 };
    DWORD owner_length = 0;
    nuint remote_owner = 0;
    uint64 bytes = 0;
    void *remote_path = NULL_POINTER;
    SIZE_T written = 0;
    LPTHREAD_START_ROUTINE remote_loader = NULL;
    HANDLE thread = NULL;
    DWORD wait = 0;
    HMODULE local_dll = NULL;
    FARPROC start = NULL;
    nuint offset = 0;
    IMAGE_DOS_HEADER dos = { 0 };
    IMAGE_NT_HEADERS header = { 0 };
    SIZE_T read = 0;
    IMAGE_DOS_HEADER *local_dos = NULL_POINTER;
    IMAGE_NT_HEADERS *local_header = NULL_POINTER;
    boole same_image = 0;
    DWORD exit_code = 0;

    // read the client path
    dll_name = L"lemon-overlay-d3d11.dll";
    clib__null_memory(&dll_path, sizeof(dll_path));
    length = GetModuleFileNameW(NULL, dll_path, 32768);
    if (!length || length >= 32768)
    {
        return 0;
    }

    // remove the file name
    while (length && dll_path[length - 1] != L'\\')
    {
        length--;
    }

    // add the DLL name
    for (index = 0;; index++)
    {
        if (length + index >= 32768)
        {
            return 0;
        }
        dll_path[length + index] = dll_name[index];
        if (!dll_name[index])
        {
            length += (DWORD)index;
            break;
        }
    }

    // check the DLL file
    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES)
    {
        _overlay_controller_internal__status(controller, "The D3D11 overlay DLL is missing beside the client", 0);
        return 0;
    }

    // open the game process
    process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, process_id);
    if (!process)
    {
        _overlay_controller_internal__status(controller, "Cannot attach to this game", GetLastError());
        return 0;
    }

    // compare the bit widths
    own_wow = FALSE;
    game_wow = FALSE;
    attached = 0;
    if (!IsWow64Process(GetCurrentProcess(), &own_wow) || !IsWow64Process(process, &game_wow) || own_wow != game_wow)
    {
        _overlay_controller_internal__status(controller, "The game and overlay need matching 32/64-bit builds", 0);
        goto finished;
    }

    // find the loaded overlay
    remote_dll = _overlay_controller_internal__module(process_id, dll_name);
    if (!remote_dll)
    {
        // find the load function
        loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        owner = NULL;
        clib__null_memory(&owner_path, sizeof(owner_path));
        if (!loader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)loader, &owner))
        {
            goto finished;
        }

        // read the module path
        owner_length = GetModuleFileNameW(owner, owner_path, 32768);
        if (!owner_length || owner_length >= 32768)
        {
            goto finished;
        }

        // find the file name
        while (owner_length && owner_path[owner_length - 1] != L'\\')
        {
            owner_length--;
        }

        // find the remote module
        remote_owner = _overlay_controller_internal__module(process_id, owner_path + owner_length);
        if (!remote_owner || !_overlay_controller_internal__publish(controller, ipc))
        {
            goto finished;
        }

        // write the DLL path
        bytes = ((uint64)length + 1) * sizeof(wchar_t);
        remote_path = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        written = 0;
        if (!remote_path || !WriteProcessMemory(process, remote_path, dll_path, bytes, &written) || written != bytes)
        {
            if (remote_path)
            {
                VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            }
            goto finished;
        }

        // start the loader thread
        remote_loader = (LPTHREAD_START_ROUTINE)(remote_owner + (nuint)loader - (nuint)owner);
        thread = CreateRemoteThread(process, NULL, 0, remote_loader, remote_path, 0, NULL);
        if (!thread)
        {
            VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            goto finished;
        }

        // wait for the loader
        wait = _overlay_controller_internal__wait(controller, ipc, thread);
        CloseHandle(thread);
        if (wait != WAIT_OBJECT_0)
        {
            // a delayed loader may still read the path: let process teardown reclaim it
            _overlay_controller_internal__status(controller, "The game loader did not respond; no retry until the setting changes", 0);
            goto finished;
        }

        // free the path memory
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);

        // find the loaded overlay
        remote_dll = _overlay_controller_internal__module(process_id, dll_name);
    }

    // check the loaded overlay
    if (!remote_dll || !_overlay_controller_internal__publish(controller, ipc))
    {
        goto finished;
    }

    // check the overlay path
    if (!_overlay_controller_internal__same_module_path(process_id, remote_dll, dll_path))
    {
        _overlay_controller_internal__status(controller, "A different overlay build is already loaded. Restart the game to switch builds", 0);
        goto finished;
    }
    // read the export offset without running DllMain or resolving dependencies in the client
    local_dll = LoadLibraryExW(dll_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (local_dll)
    {
        // find the start function
        start = GetProcAddress(local_dll, "lemon_overlay_start");
        offset = (nuint)start - (nuint)local_dll;
        if (start)
        {
            // confirm the loaded image still matches the file used to locate this export
            clib__null_memory(&dos, sizeof(dos));
            clib__null_memory(&header, sizeof(header));
            read = 0;
            local_dos = (IMAGE_DOS_HEADER *)local_dll;
            local_header = (IMAGE_NT_HEADERS *)((ubyte *)local_dll + local_dos->e_lfanew);
            same_image = ReadProcessMemory(process, (void *)remote_dll, &dos, sizeof(dos), &read) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew == local_dos->e_lfanew && dos.e_lfanew > 0 && dos.e_lfanew < 1048576 && ReadProcessMemory(process, (void *)(remote_dll + dos.e_lfanew), &header, sizeof(header), &read) && read == sizeof(header) && header.Signature == IMAGE_NT_SIGNATURE && header.FileHeader.TimeDateStamp == local_header->FileHeader.TimeDateStamp && header.OptionalHeader.SizeOfImage == local_header->OptionalHeader.SizeOfImage && header.OptionalHeader.CheckSum == local_header->OptionalHeader.CheckSum;

            // start the overlay thread
            thread = same_image ? CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)(remote_dll + offset), NULL, 0, NULL) : NULL;
            if (thread)
            {
                exit_code = 0;
                attached = _overlay_controller_internal__wait(controller, ipc, thread) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exit_code) && exit_code == 1;
                CloseHandle(thread);
            }

            // report a different build
            if (!same_image)
            {
                _overlay_controller_internal__status(controller, "The loaded overlay differs from this build. Restart the game", 0);
            }
        }

        // free the local DLL
        FreeLibrary(local_dll);
    }
finished:
    // close the game process
    CloseHandle(process);
    return attached;
}

/**
 * @brief watches the configured game and publishes snapshots without blocking the UI thread.
 *
 * @param void* parameter -> controller passed to the worker
 *
 * @return DWORD WINAPI -> 0 when the worker stops
 */
static DWORD WINAPI _overlay_controller_internal__worker(void *parameter)
{
    desktop_overlay_controller_t *controller = NULL_POINTER;
    desktop_settings_t previous = { 0 };
    desktop_overlay_ipc_t *ipc = NULL_POINTER;
    HANDLE game = NULL;
    DWORD process_id = 0;
    boole attempted = 0;
    ULONGLONG next_search = 0;
    desktop_settings_t settings = { 0 };
    desktop_overlay_snapshot_t hidden = { 0 };
    BOOL own_wow = 0;
    BOOL game_wow = 0;
    const char *detail = NULL_POINTER;
    boole unchanged = 0;

    // set the start values
    controller = parameter;
    clib__null_memory(&previous, sizeof(previous));
    ipc = NULL;
    game = NULL;
    process_id = 0;
    attempted = 0;
    next_search = 0;

    // loop until stopped
    while (WaitForSingleObject(controller->stop, 100) == WAIT_TIMEOUT)
    {
        // copy the settings
        clib__null_memory(&settings, sizeof(settings));
        EnterCriticalSection(&controller->lock);
        settings = controller->settings;
        LeaveCriticalSection(&controller->lock);

        // reset after a change
        if (clib__memcmp(&previous, &settings, sizeof(settings)) != 0 || (game && WaitForSingleObject(game, 0) != WAIT_TIMEOUT))
        {
            // hide the overlay
            clib__null_memory(&hidden, sizeof(hidden));
            desktop_overlay_data__clear(&hidden);
            desktop_overlay_ipc__write(ipc, &hidden);
            desktop_overlay_ipc__close(ipc);
            ipc = NULL;

            // close the game handle
            if (game)
            {
                CloseHandle(game);
            }
            game = NULL;
            process_id = 0;
            attempted = 0;

            // store the new settings
            previous = settings;
            EnterCriticalSection(&controller->lock);
            clib__string_copy(controller->active_game, sizeof(controller->active_game), settings.game_executable);
            LeaveCriticalSection(&controller->lock);
            next_search = 0;
        }

        // overlay is disabled
        if (!settings.overlay_enabled)
        {
            _overlay_controller_internal__status(controller, "Game overlay disabled", 0);
            continue;
        }

        // check the game name
        if (!desktop_settings__valid_game(settings.game_executable))
        {
            _overlay_controller_internal__status(controller, "Enter a game executable, such as game.exe", 0);
            continue;
        }

        // write the snapshot
        if (ipc && !_overlay_controller_internal__publish(controller, ipc))
        {
            continue;
        }

        // search once per second
        if (attempted || GetTickCount64() < next_search)
        {
            continue;
        }
        next_search = GetTickCount64() + 1000;

        // find the game process
        if (!game)
        {
            process_id = _overlay_controller_internal__find_game(settings.game_executable);
            if (!process_id || process_id == MAXDWORD)
            {
                _overlay_controller_internal__status(controller, process_id ? "More than one matching game is running" : "Waiting for the selected game", 0);
                continue;
            }

            // open the game process
            game = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
            if (!game)
            {
                _overlay_controller_internal__status(controller, "Cannot inspect this game", GetLastError());
                continue;
            }
        }

        // compare the bit widths
        own_wow = FALSE;
        game_wow = FALSE;
        if (!IsWow64Process(GetCurrentProcess(), &own_wow) || !IsWow64Process(game, &game_wow) || own_wow != game_wow)
        {
            _overlay_controller_internal__status(controller, "The game and overlay need matching 32/64-bit builds", 0);
            attempted = 1;
            continue;
        }

        // refuse D3D12 and Vulkan
        if (_overlay_controller_internal__module(process_id, L"d3d12.dll") || _overlay_controller_internal__module(process_id, L"vulkan-1.dll"))
        {
            _overlay_controller_internal__status(controller, "D3D12/Vulkan detected; this build supports D3D11 overlays", 0);
            continue;
        }

        // wait for D3D11
        if (!_overlay_controller_internal__module(process_id, L"d3d11.dll"))
        {
            detail = "Waiting for the game renderer";
            if (_overlay_controller_internal__module(process_id, L"d3d9.dll"))
            {
                detail = "D3D9 detected; this build supports D3D11 overlays";
            }
            else if (_overlay_controller_internal__module(process_id, L"opengl32.dll"))
            {
                detail = "OpenGL detected; this build supports D3D11 overlays";
            }
            _overlay_controller_internal__status(controller, detail, 0);
            continue;
        }

        // open the shared memory
        ipc = desktop_overlay_ipc__open(process_id, 1);
        if (!ipc || !_overlay_controller_internal__publish(controller, ipc))
        {
            _overlay_controller_internal__status(controller, "Cannot publish overlay data to this game", GetLastError());
            attempted = 1;
            continue;
        }

        // attach the overlay
        _overlay_controller_internal__status(controller, "D3D11 detected; attaching overlay", 0);
        attempted = 1;
        if (_overlay_controller_internal__attach(controller, ipc, process_id))
        {
            _overlay_controller_internal__status(controller, "D3D11 overlay attached", 0);
        }
        else
        {
            EnterCriticalSection(&controller->lock);
            unchanged = clib__strcmp(controller->status, "D3D11 detected; attaching overlay") == 0;
            LeaveCriticalSection(&controller->lock);
            if (unchanged)
            {
                _overlay_controller_internal__status(controller, "The game did not load the overlay", GetLastError());
            }
        }
    }

    // hide the overlay
    clib__null_memory(&hidden, sizeof(hidden));
    desktop_overlay_data__clear(&hidden);
    desktop_overlay_ipc__write(ipc, &hidden);
    desktop_overlay_ipc__close(ipc);

    // close the game handle
    if (game)
    {
        CloseHandle(game);
    }
    return 0;
}

/**
 * @brief allocates a disabled overlay controller and starts its worker.
 *
 * @return desktop_overlay_controller_t* -> the owned controller, or NULL on failure
 */
desktop_overlay_controller_t *desktop_overlay_controller__create(void)
{
    desktop_overlay_controller_t *controller = NULL_POINTER;

    // allocate the controller
    controller = calloc(1, sizeof(*controller));
    if (!controller)
    {
        return NULL;
    }

    // set the start state
    InitializeCriticalSection(&controller->lock);
    desktop_overlay_data__clear(&controller->snapshot);
    clib__string_copy(controller->status, sizeof(controller->status), "Game overlay disabled");

    // start the worker thread
    controller->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (controller->stop)
    {
        controller->thread = CreateThread(NULL, 0, _overlay_controller_internal__worker, controller, 0, NULL);
    }

    // destroy on failure
    if (!controller->thread)
    {
        desktop_overlay_controller__destroy(controller);
        return NULL;
    }
    return controller;
}

/**
 * @brief stops the watcher, hides its overlay and releases controller resources.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 *
 * @return void
 */
void desktop_overlay_controller__destroy(desktop_overlay_controller_t *controller)
{
    // nothing to destroy
    if (!controller)
    {
        return;
    }

    // set the stop event
    if (controller->stop)
    {
        SetEvent(controller->stop);
    }

    // wait for the thread
    if (controller->thread)
    {
        WaitForSingleObject(controller->thread, INFINITE);
        CloseHandle(controller->thread);
    }

    // close the stop event
    if (controller->stop)
    {
        CloseHandle(controller->stop);
    }

    // free the controller
    DeleteCriticalSection(&controller->lock);
    desktop_memory__erase(controller, sizeof(*controller));
    free(controller);
}

/**
 * @brief copies saved settings for the watcher to apply on its next iteration.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param const desktop_settings_t* settings -> local overlay settings
 *
 * @return void
 */
void desktop_overlay_controller__configure(desktop_overlay_controller_t *controller, const desktop_settings_t *settings)
{
    // store the settings
    EnterCriticalSection(&controller->lock);
    controller->settings = *settings;
    LeaveCriticalSection(&controller->lock);
}

/**
 * @brief copies a validated display snapshot for the watcher to publish.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param const desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 *
 * @return void
 */
void desktop_overlay_controller__publish(desktop_overlay_controller_t *controller, const desktop_overlay_snapshot_t *snapshot)
{
    // check the snapshot
    if (!desktop_overlay_data__valid(snapshot))
    {
        return;
    }

    // store the snapshot
    EnterCriticalSection(&controller->lock);
    controller->snapshot = *snapshot;
    LeaveCriticalSection(&controller->lock);
}

/**
 * @brief copies the current status into the caller-owned text buffer.
 *
 * @param desktop_overlay_controller_t* controller -> desktop-owned overlay controller
 * @param char* text -> text buffer
 * @param uint64 capacity -> destination capacity including its terminator
 *
 * @return void
 */
void desktop_overlay_controller__status(desktop_overlay_controller_t *controller, char *text, uint64 capacity)
{
    // copy the status text
    EnterCriticalSection(&controller->lock);
    clib__string_copy(text, capacity, controller->status);
    LeaveCriticalSection(&controller->lock);
}
