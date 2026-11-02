#include "platform/memory.h"
#include "clib.h"
#include <pthread.h>
#include <stdlib.h>
#include "platform/thread.h"

struct desktop_thread_t
{
    pthread_t handle;
    desktop_thread_function_t function;
    void* context;
};

struct desktop_mutex_t
{
    pthread_mutex_t lock;
};

// declarations
static void* _thread_internal__run(void* context);

/**
 * @brief adapts the shared worker entry point to POSIX.
 * @param void* context -> owned descriptor retained until join
 * @return void* -> unused worker result
 */
static void* _thread_internal__run(void* context)
{
    desktop_thread_t* thread = context;

    thread->function(thread->context);
    return NULL;
}

/**
 * @brief starts a worker without blocking the UI.
 * @param desktop_thread_function_t function -> worker entry point
 * @param void* context -> caller-owned state retained until join
 * @return desktop_thread_t* -> owned descriptor, or NULL
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
    if (pthread_create(&thread->handle, NULL, _thread_internal__run, thread) != 0)
    {
        desktop_memory__erase(thread, sizeof(*thread));
        free(thread);
        return NULL;
    }
    return thread;
}

/**
 * @brief waits for the worker before releasing its descriptor.
 * @param desktop_thread_t* thread -> owned descriptor, or NULL
 * @return void
 */
void desktop_thread__join(desktop_thread_t* thread)
{
    if (thread)
    {
        pthread_join(thread->handle, NULL);
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

    if (mutex && pthread_mutex_init(&mutex->lock, NULL) != 0)
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
        pthread_mutex_destroy(&mutex->lock);
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
    pthread_mutex_lock(&mutex->lock);
}

/**
 * @brief releases a shared-state lock.
 * @param desktop_mutex_t* mutex -> locked mutex
 * @return void
 */
void desktop_mutex__unlock(desktop_mutex_t* mutex)
{
    pthread_mutex_unlock(&mutex->lock);
}

/**
 * @brief acquires a short lock without waiting on the audio callback thread.
 * @param desktop_mutex_t* mutex -> initialized mutex
 * @return boole -> 1 when acquired; the caller must unlock
 */
boole desktop_mutex__try_lock(desktop_mutex_t* mutex)
{
    return pthread_mutex_trylock(&mutex->lock) == 0;
}
