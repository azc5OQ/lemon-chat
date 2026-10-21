// This file helps the server stay responsive when many clients connect at once.
// It limits simultaneous login key calculations. Connections waiting for a turn
// sleep on their existing threads until a login slot becomes free.
#include "login_admission.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef struct login_waiter_t
{
    struct login_waiter_t* previous;
    struct login_waiter_t* next;
    pthread_cond_t ready;
} login_waiter_t;

static pthread_mutex_t g_login_mutex = PTHREAD_MUTEX_INITIALIZER;
static login_waiter_t* g_first_waiter = NULL;
static login_waiter_t* g_last_waiter = NULL;
static uint g_active_logins = 0;
static uint g_login_limit = 4;

// declarations
static uint64 _login_admission_internal__now_ms(void);
static void _login_admission_internal__wake_next(void);
static void _login_admission_internal__remove_waiter(login_waiter_t* waiter);

/**
 * @brief Reads elapsed time without being affected by changes to the system clock.
 *
 * @return uint64 -> clock value in milliseconds
 */
static uint64 _login_admission_internal__now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64)now.tv_sec * 1000 + (uint64)now.tv_nsec / 1000000;
}

/**
 * @brief Wakes the first waiting connection when another login can begin.
 *
 * @attention The caller must hold g_login_mutex while checking the queue.
 *
 * @return void
 */
static void _login_admission_internal__wake_next(void)
{
    if (g_first_waiter != NULL && g_active_logins < g_login_limit)
    {
        pthread_cond_signal(&g_first_waiter->ready);
    }
}

/**
 * @brief Removes a waiting connection and wakes the next one if a slot is free.
 *
 * @param login_waiter_t* waiter -> the entry currently linked into the waiting list
 *
 * @attention The caller holds g_login_mutex and owns the entry.
 *
 * @return void
 */
static void _login_admission_internal__remove_waiter(login_waiter_t* waiter)
{
    if (waiter->previous != NULL)
    {
        waiter->previous->next = waiter->next;
    }
    else
    {
        g_first_waiter = waiter->next;
    }
    if (waiter->next != NULL)
    {
        waiter->next->previous = waiter->previous;
    }
    else
    {
        g_last_waiter = waiter->previous;
    }
    _login_admission_internal__wake_next();
}

/**
 * @brief Changes the login limit without interrupting calculations already running.
 *
 * @param uint limit -> simultaneous logins allowed, clamped to 1 through 64
 *
 * @return void
 */
void login_admission__set_limit(uint limit)
{
    if (limit < 1)
    {
        limit = 1;
    }
    if (limit > 64)
    {
        limit = 64;
    }

    pthread_mutex_lock(&g_login_mutex);
    g_login_limit = limit;
    _login_admission_internal__wake_next();
    pthread_mutex_unlock(&g_login_mutex);
}

/**
 * @brief Waits in arrival order for a free login slot, using the connection's reader thread.
 *
 * @param int (*)(void*) connection_is_valid -> returns nonzero while the connection may log in
 * @param void* connection -> callback data, kept alive by the caller until this returns
 * @param uint timeout_ms -> maximum wait in milliseconds
 *
 * @attention Call without the clients lock. After success, release the slot exactly
 * once when key calculations finish, including on failure.
 *
 * @return int -> 1 when a slot was acquired; 0 on cancellation, timeout or error
 */
int login_admission__acquire(int (*connection_is_valid)(void*), void* connection, uint timeout_ms)
{
    login_waiter_t waiter;
    uint64 deadline_ms = _login_admission_internal__now_ms() + timeout_ms;
    int acquired = 0;
    struct timespec wake_time;
    uint64 now_ms;
    uint64 wait_ms;
    int wait_status;
    int valid;

    if (connection_is_valid == NULL || timeout_ms == 0
        || pthread_cond_init(&waiter.ready, NULL) != 0)
    {
        return 0;
    }

    // Each waiting reader keeps its own small queue entry on its stack. There
    // cannot be more entries than existing connection threads, and finishing a
    // login wakes only the next waiter instead of waking hundreds of threads.
    pthread_mutex_lock(&g_login_mutex);
    waiter.previous = g_last_waiter;
    waiter.next = NULL;
    if (g_last_waiter != NULL)
    {
        g_last_waiter->next = &waiter;
    }
    else
    {
        g_first_waiter = &waiter;
    }
    g_last_waiter = &waiter;

    for (;;)
    {
        // The connection check may need the server's client lock. Never hold
        // this mutex while asking for it: saving settings uses the other order.
        pthread_mutex_unlock(&g_login_mutex);
        valid = connection_is_valid(connection);
        pthread_mutex_lock(&g_login_mutex);

        now_ms = _login_admission_internal__now_ms();
        if (valid == 0 || now_ms >= deadline_ms)
        {
            break;
        }
        if (g_first_waiter == &waiter && g_active_logins < g_login_limit)
        {
            g_active_logins++;
            acquired = 1;
            break;
        }

        // A disconnected connection is noticed within a second even if every
        // login slot is busy. The overall queue deadline uses a monotonic clock.
        wait_ms = deadline_ms - now_ms;
        if (wait_ms > 1000)
        {
            wait_ms = 1000;
        }
        clock_gettime(CLOCK_REALTIME, &wake_time);
        wake_time.tv_sec += (time_t)(wait_ms / 1000);
        wake_time.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (wake_time.tv_nsec >= 1000000000L)
        {
            wake_time.tv_sec++;
            wake_time.tv_nsec -= 1000000000L;
        }
        wait_status = pthread_cond_timedwait(&waiter.ready, &g_login_mutex, &wake_time);
        if (wait_status != 0 && wait_status != ETIMEDOUT)
        {
            break;
        }
    }

    _login_admission_internal__remove_waiter(&waiter);
    pthread_mutex_unlock(&g_login_mutex);
    pthread_cond_destroy(&waiter.ready);
    return acquired;
}

/**
 * @brief Returns a login slot and wakes the next waiting connection if one may begin.
 *
 * @attention Call exactly once for each successful login_admission__acquire call.
 *
 * @return void
 */
void login_admission__release(void)
{
    pthread_mutex_lock(&g_login_mutex);
    if (g_active_logins != 0)
    {
        g_active_logins--;
    }
    _login_admission_internal__wake_next();
    pthread_mutex_unlock(&g_login_mutex);
}
