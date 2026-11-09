// TLS = thread local storage; FIFO = first in, first out.

#ifndef CUSTOM_RWLOCK_H
#define CUSTOM_RWLOCK_H 1

// Each waiting thread owns its queue node on its stack.
typedef struct fifo_rwlock_node_t
{
    boole is_writer;
    uint64 ticket;
    struct fifo_rwlock_node_t *next;
} fifo_rwlock_node_t;

typedef struct custom_rwlock_t
{
    pthread_mutex_t mutex; // Protects the fields below.
    pthread_cond_t wait_cv;
    uint64 read_count_slot;

    // Unsigned tickets wrap after 2^64 values; the queue compares them for equality.
    uint64 next_ticket;
    uint64 serving_ticket;

    // A nonzero recursion count identifies the thread that holds write access.
    pthread_t writer_thread;
    uint64 write_recursion;

    // Includes recursive reads, but excludes reads taken by the current writer.
    uint64 num_readers;

    fifo_rwlock_node_t *queue_head;
    fifo_rwlock_node_t *queue_tail;

    // An upgrading reader blocks new readers while waiting for existing readers.
    boole upgrade_pending;
    pthread_cond_t upgrade_cv;
} custom_rwlock_t;

void clib__rwlock_init(custom_rwlock_t *lock);
void clib__rwlock_destroy(custom_rwlock_t *lock);
void clib__read_lock(custom_rwlock_t *lock);
void clib__write_lock(custom_rwlock_t *lock);
void clib__unlock(custom_rwlock_t *lock);

#endif
