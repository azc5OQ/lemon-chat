#ifndef LEMON_DESKTOP_THREAD_H
#define LEMON_DESKTOP_THREAD_H

#include "clib.h"

typedef struct desktop_thread_t desktop_thread_t;
typedef struct desktop_mutex_t desktop_mutex_t;
typedef void (*desktop_thread_function_t)(void *context);

desktop_thread_t *desktop_thread__start(desktop_thread_function_t function, void *context);
void desktop_thread__join(desktop_thread_t *thread);
desktop_mutex_t *desktop_mutex__create(void);
void desktop_mutex__destroy(desktop_mutex_t *mutex);
void desktop_mutex__lock(desktop_mutex_t *mutex);
boole desktop_mutex__try_lock(desktop_mutex_t *mutex);
void desktop_mutex__unlock(desktop_mutex_t *mutex);

#endif
