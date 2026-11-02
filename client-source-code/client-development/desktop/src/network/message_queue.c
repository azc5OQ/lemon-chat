#include "platform/memory.h"
#include <stdlib.h>
#include "network/message_queue.h"
#include "clib.h"

/**
 * @brief copies a message into bounded storage; the caller holds the queue lock.
 * @param desktop_message_queue_t* queue -> initialized queue
 * @param const char* data -> complete message bytes
 * @param uint64 size -> byte count
 * @param boole binary -> WebSocket message kind
 * @return boole -> 1 when queued; 0 when full or allocation fails
 */
boole desktop_message_queue__push(desktop_message_queue_t* queue, const char* data, uint64 size, boole binary)
{
    desktop_message_t* message = NULL;

    if (!data || queue->count == DESKTOP_QUEUE_SLOTS || size > DESKTOP_QUEUE_BYTES - queue->bytes)
    {
        return 0;
    }
    message = &queue->messages[(queue->head + queue->count) % DESKTOP_QUEUE_SLOTS];
    message->data = calloc(1, size + 1);
    if (!message->data)
    {
        return 0;
    }
    clib__memcpy(message->data, data, size);
    message->size = size;
    message->binary = binary;
    message->expires = 0;
    queue->bytes += size;
    queue->count++;
    return 1;
}

/**
 * @brief queues a copied live message with a monotonic expiry; the caller holds the queue lock.
 * @param desktop_message_queue_t* queue -> initialized queue
 * @param const char* data -> complete message bytes
 * @param uint64 size -> byte count
 * @param boole binary -> WebSocket message kind
 * @param uint64 expires -> expiry in milliseconds, zero for reliable delivery
 * @return boole -> TRUE when the queue owns a copy
 */
boole desktop_message_queue__push_until(desktop_message_queue_t* queue, const char* data, uint64 size, boole binary, uint64 expires)
{
    if (!desktop_message_queue__push(queue, data, size, binary))
    {
        return FALSE;
    }
    queue->messages[(queue->head + queue->count - 1) % DESKTOP_QUEUE_SLOTS].expires = expires;
    return TRUE;
}

/**
 * @brief discards stale live messages while preserving reliable messages and their order.
 * @param desktop_message_queue_t* queue -> initialized queue, locked by the caller
 * @param uint64 now -> current monotonic time, or zero to discard every live message
 * @return void
 */
void desktop_message_queue__expire(desktop_message_queue_t* queue, uint64 now)
{
    desktop_message_queue_t retained = {0};
    desktop_message_t message = {0};
    while (desktop_message_queue__pop(queue, &message))
    {
        if (message.expires && (!now || now >= message.expires))
        {
            desktop_message__release(&message);
        }
        else
        {
            retained.messages[retained.count++] = message;
            retained.bytes += message.size;
        }
    }
    *queue = retained;
    clib__null_memory(&retained, sizeof(retained));
}

/**
 * @brief transfers ownership of the next message; the caller holds the queue lock.
 * @param desktop_message_queue_t* queue -> initialized queue
 * @param desktop_message_t* message -> cleared output, owned by caller on success
 * @return boole -> 1 when a message was available
 */
boole desktop_message_queue__pop(desktop_message_queue_t* queue, desktop_message_t* message)
{
    clib__null_memory(message, sizeof(*message));
    if (!queue->count)
    {
        return 0;
    }
    *message = queue->messages[queue->head];
    clib__null_memory(&queue->messages[queue->head], sizeof(*message));
    queue->head = (queue->head + 1) % DESKTOP_QUEUE_SLOTS;
    queue->count--;
    queue->bytes -= message->size;
    return 1;
}

/**
 * @brief clears owned message bytes before releasing them.
 * @param desktop_message_t* message -> message transferred from a queue
 * @return void
 */
void desktop_message__release(desktop_message_t* message)
{
    if (message->data)
    {
        clib__null_memory(message->data, message->size);
        desktop_memory__erase(message->data, message->size + 1);
        free(message->data);
    }
    clib__null_memory(message, sizeof(*message));
}

/**
 * @brief releases every queued message; the caller holds the queue lock.
 * @param desktop_message_queue_t* queue -> initialized queue
 * @return void
 */
void desktop_message_queue__clear(desktop_message_queue_t* queue)
{
    desktop_message_t message = {0};

    while (desktop_message_queue__pop(queue, &message))
    {
        desktop_message__release(&message);
    }
}
