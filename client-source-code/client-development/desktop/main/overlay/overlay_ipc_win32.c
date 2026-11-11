#include "platform/memory.h"
// Exchanges display-only snapshots between the desktop and selected game process.
#include <windows.h>
#include <stdlib.h>
#include "overlay_ipc.h"
#include "clib.h"

struct desktop_overlay_ipc_t
{
    HANDLE mapping;
    HANDLE mutex;
    desktop_overlay_snapshot_t *view;
    boole writer;
};

/**
 * @brief opens the bounded shared display mapping and its mutex for one game process.
 *
 * @param uint game_pid -> game process ID used in the mapping name
 * @param boole writer -> nonzero for the desktop writer; zero for a game reader
 *
 * @return desktop_overlay_ipc_t* -> the owned IPC handle, or NULL on failure
 */
desktop_overlay_ipc_t *desktop_overlay_ipc__open(uint game_pid, boole writer)
{
    char name[96] = { 0 };
    desktop_overlay_ipc_t *ipc = NULL_POINTER;

    // allocate the handle
    clib__null_memory(&name, sizeof(name));
    ipc = calloc(1, sizeof(*ipc));
    if (!ipc)
    {
        return NULL;
    }

    // open the mutex
    ipc->writer = writer;
    clib__string_format(name, sizeof(name), "Local\\LemonChat.Overlay.%u.Lock", game_pid);
    ipc->mutex = writer ? CreateMutexA(NULL, FALSE, name) : OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);

    // open the shared memory
    clib__string_format(name, sizeof(name), "Local\\LemonChat.Overlay.%u.Data", game_pid);
    ipc->mapping = writer ? CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*ipc->view), name) : OpenFileMappingA(FILE_MAP_READ, FALSE, name);

    // map the shared memory
    if (ipc->mapping && ipc->mutex)
    {
        ipc->view = MapViewOfFile(ipc->mapping, writer ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, sizeof(*ipc->view));
    }

    // close on failure
    if (!ipc->view)
    {
        desktop_overlay_ipc__close(ipc);
        return NULL;
    }
    return ipc;
}

/**
 * @brief releases the mapping, mutex and local IPC handle.
 *
 * @param desktop_overlay_ipc_t* ipc -> shared display mapping handle
 *
 * @return void
 */
void desktop_overlay_ipc__close(desktop_overlay_ipc_t *ipc)
{
    // nothing to close
    if (!ipc)
    {
        return;
    }

    // unmap the shared memory
    if (ipc->view)
    {
        UnmapViewOfFile(ipc->view);
    }

    // close the shared memory
    if (ipc->mapping)
    {
        CloseHandle(ipc->mapping);
    }

    // close the mutex
    if (ipc->mutex)
    {
        CloseHandle(ipc->mutex);
    }

    // free the handle
    desktop_memory__erase(ipc, sizeof(*ipc));
    free(ipc);
}

/**
 * @brief copies a valid snapshot under a bounded mutex wait and refreshes its owner and heartbeat.
 *
 * @param desktop_overlay_ipc_t* ipc -> shared display mapping handle
 * @param const desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 *
 * @return boole -> 1 when written; 0 if busy, invalid or owned by another running client
 */
boole desktop_overlay_ipc__write(desktop_overlay_ipc_t *ipc, const desktop_overlay_snapshot_t *snapshot)
{
    DWORD wait = 0;
    boole permitted = 0;
    HANDLE owner = NULL;

    // check arguments
    if (!ipc || !ipc->writer || !desktop_overlay_data__valid(snapshot))
    {
        return 0;
    }

    // lock the mutex
    wait = WaitForSingleObject(ipc->mutex, 10);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    {
        return 0;
    }

    // check the owner process
    permitted = 1;
    if (desktop_overlay_data__valid(ipc->view) && ipc->view->owner_pid && ipc->view->owner_pid != GetCurrentProcessId())
    {
        owner = OpenProcess(SYNCHRONIZE, FALSE, ipc->view->owner_pid);
        if (owner)
        {
            permitted = WaitForSingleObject(owner, 0) == WAIT_OBJECT_0;
            CloseHandle(owner);
        }
        else
        {
            permitted = GetLastError() == ERROR_INVALID_PARAMETER;
        }
    }

    // copy the snapshot
    if (permitted)
    {
        clib__memcpy(ipc->view, snapshot, sizeof(*snapshot));
        ipc->view->owner_pid = GetCurrentProcessId();
        ipc->view->heartbeat_ms = GetTickCount64();
    }

    // unlock the mutex
    ReleaseMutex(ipc->mutex);
    return permitted;
}

/**
 * @brief copies a validated snapshot without making the game render thread wait for the mutex.
 *
 * @param desktop_overlay_ipc_t* ipc -> shared display mapping handle
 * @param desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 *
 * @return boole -> 1 when copied; 0 if busy or invalid
 */
boole desktop_overlay_ipc__read(desktop_overlay_ipc_t *ipc, desktop_overlay_snapshot_t *snapshot)
{
    DWORD wait = 0;
    boole valid = 0;

    // check arguments
    if (!ipc || !snapshot)
    {
        return 0;
    }

    // lock the mutex
    wait = WaitForSingleObject(ipc->mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    {
        return 0;
    }

    // copy the valid snapshot
    valid = desktop_overlay_data__valid(ipc->view);
    if (valid)
    {
        clib__memcpy(snapshot, ipc->view, sizeof(*snapshot));
    }

    // unlock the mutex
    ReleaseMutex(ipc->mutex);
    return valid;
}
