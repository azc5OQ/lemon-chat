#ifndef LEMON_DESKTOP_MESSAGE_QUEUE_H
#define LEMON_DESKTOP_MESSAGE_QUEUE_H

#include "clib.h"

#define DESKTOP_QUEUE_SLOTS 64
#define DESKTOP_QUEUE_BYTES (8U * 1024U * 1024U)

typedef struct desktop_message_t
{
    char *data;
    uint64 size;
    boole binary;
    uint64 expires;
} desktop_message_t;

typedef struct desktop_message_queue_t
{
    desktop_message_t messages[DESKTOP_QUEUE_SLOTS];
    uint64 head;
    uint64 count;
    uint64 bytes;
} desktop_message_queue_t;

boole desktop_message_queue__has_space(const desktop_message_queue_t *queue, uint64 size);
boole desktop_message_queue__push(desktop_message_queue_t *queue, const char *data, uint64 size, boole binary);
boole desktop_message_queue__push_until(desktop_message_queue_t *queue, const char *data, uint64 size, boole binary, uint64 expires);
void desktop_message_queue__expire(desktop_message_queue_t *queue, uint64 now);
boole desktop_message_queue__pop(desktop_message_queue_t *queue, desktop_message_t *message);
void desktop_message_queue__clear(desktop_message_queue_t *queue);
void desktop_message__release(desktop_message_t *message);

#endif
