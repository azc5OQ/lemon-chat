#include "platform/memory.h"
#include "clib.h"
#include <windows.h>
#include <stdlib.h>
#include "platform/thread.h"

struct desktop_thread_t
{
    HANDLE handle;
    desktop_thread_function_t function;
    void* context;
};

struct desktop_mutex_t
{
    CRITICAL_SECTION lock;
};

// declarations
static DWORD WINAPI _thread_internal__run(void* context);

/**
 * @brief adapts the shared worker entry point to Windows.
 * @param void* context -> owned thread descriptor, retained until join
 * @return DWORD WINAPI -> worker exit code
 */
static DWORD WINAPI _thread_internal__run(void* context)
{
    desktop_thread_t* thread = context;

    thread->function(thread->context);
    return 0;
}

/**
 * @brief starts a worker without blocking the UI.
 * @param desktop_thread_function_t function -> worker entry point
 * @param void* context -> caller-owned state retained until join
 * @return desktop_thread_t* -> owned handle, or NULL on failure
 */
desktop_thread_t* desktop_thread__start(desktop_thread_function_t function, void* context)
{
    desktop_thread_t* thread = calloc(1, sizeof(*thread));

    if (!thread)
    {
        return NULL;
    }
    thread->function = function;
    thread->context = context;
    thread->handle = CreateThread(NULL, 0, _thread_internal__run, thread, 0, NULL);
    if (!thread->handle)
    {
        desktop_memory__erase(thread, sizeof(*thread));
        free(thread);
        return NULL;
    }
    return thread;
}

/**
 * @brief waits for the worker before releasing its descriptor.
 * @param desktop_thread_t* thread -> owned handle, or NULL
 * @return void
 */
void desktop_thread__join(desktop_thread_t* thread)
{
    if (thread)
    {
        WaitForSingleObject(thread->handle, INFINITE);
        CloseHandle(thread->handle);
        desktop_memory__erase(thread, sizeof(*thread));
        free(thread);
    }
}

/**
 * @brief allocates and initializes a shared-state mutex.
 * @return desktop_mutex_t* -> owned mutex, or NULL
 */
desktop_mutex_t* desktop_mutex__create(void)
{
    desktop_mutex_t* mutex = calloc(1, sizeof(*mutex));

    if (mutex && !InitializeCriticalSectionEx(&mutex->lock, 0, 0))
    {
        desktop_memory__erase(mutex, sizeof(*mutex));
        free(mutex);
        mutex = NULL;
    }
    return mutex;
}

/**
 * @brief releases an unused mutex after all workers have stopped.
 * @param desktop_mutex_t* mutex -> owned mutex, or NULL
 * @return void
 */
void desktop_mutex__destroy(desktop_mutex_t* mutex)
{
    if (mutex)
    {
        DeleteCriticalSection(&mutex->lock);
        desktop_memory__erase(mutex, sizeof(*mutex));
        free(mutex);
    }
}

/**
 * @brief acquires a short-lived shared-state lock.
 * @param desktop_mutex_t* mutex -> initialized mutex
 * @return void
 */
void desktop_mutex__lock(desktop_mutex_t* mutex)
{
    EnterCriticalSection(&mutex->lock);
}

/**
 * @brief releases a shared-state lock.
 * @param desktop_mutex_t* mutex -> locked mutex
 * @return void
 */
void desktop_mutex__unlock(desktop_mutex_t* mutex)
{
    LeaveCriticalSection(&mutex->lock);
}

/**
 * @brief acquires a short lock only when it is immediately available.
 * @param desktop_mutex_t* mutex -> initialized mutex
 * @return boole -> 1 when acquired; the caller must unlock
 */
boole desktop_mutex__try_lock(desktop_mutex_t* mutex)
{
    return TryEnterCriticalSection(&mutex->lock) != 0;
}
