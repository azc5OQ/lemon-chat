//TLS = thread local storage
//FIFO = first in first out

/*
 * custom_rwlock_t — reentrant, FIFO-fair, upgradeable reader-writer lock
 *
 * WHY THIS EXISTS INSTEAD OF pthread_rwlock_t
 * --------------------------------------------
 *  1. No reentrancy in pthread_rwlock_t.
 *     Calling pthread_rwlock_rdlock twice from the same thread is undefined
 *     behaviour (deadlocks on Linux/glibc). This implementation uses TLS to
 *     track per-thread read counts, so clib__read_lock can be called
 *     recursively without deadlocking. Same applies to write locks.
 *
 *  2. Writer starvation in pthread_rwlock_t.
 *     POSIX does not specify fairness. On Linux, a continuous stream of
 *     readers can starve writers indefinitely. Here every caller takes a
 *     FIFO ticket, so once a writer is queued, new readers queue behind it.
 *
 *  3. Write→read nesting is undefined in pthread_rwlock_t.
 *     Calling pthread_rwlock_rdlock while holding the write lock is
 *     undefined behaviour. This implementation allows it (fast-path 1).
 *
 *  4. Read→write upgrade is unsupported in pthread_rwlock_t.
 *     Calling clib__write_lock while already holding a read lock performs
 *     an automatic in-place upgrade — no separate call needed. Internally
 *     this waits for all other readers to drain and then atomically converts
 *     the read lock to a write lock.
 *
 * COST vs pthread_rwlock_t
 * -------------------------
 * pthread_rwlock_t is implemented with futexes and makes no heap allocations.
 * This implementation calls calloc() on every first-time lock acquisition to
 * allocate a FIFO queue node, and pthread_cond_init/destroy per call. This
 * is acceptable for coarse-grained global locks held for milliseconds, but
 * would be inappropriate on a hot inner loop.
 *
 * UNLOCK ORDER FOR WRITER WITH NESTED READS
 * ------------------------------------------
 * If a writer acquires nested read locks (write_lock then read_lock), the
 * implementation automatically releases them in LIFO order — the first
 * clib__unlock always releases the innermost (read) lock, and the final
 * one releases the write lock. The caller just needs one clib__unlock per
 * clib__read_lock / clib__write_lock call.
 *
 * UPGRADE CONFLICT BEHAVIOUR
 * ---------------------------
 * Only one thread can be mid-upgrade at a time. If a second thread calls
 * clib__write_lock while holding a read lock AND an upgrade is already in
 * progress, it surrenders its read lock and re-enters as a normal write-lock
 * contender via the FIFO queue. Another writer may acquire before it does,
 * but this is unavoidable and safe — it prevents mutual deadlock between two
 * simultaneous upgraders.
 *
 * KNOWN LIMITATION
 * -----------------
 * There is no try-lock variant. All acquisitions block until granted.
 */

#ifndef CUSTOM_RWLOCK_H
#define CUSTOM_RWLOCK_H 1

/* Per-thread entry tracking how many times this thread has read-locked one
   specific lock. Stored in a per-thread linked list (thread_lock_map_t). */
typedef struct thread_lock_entry_t
{
	void *lock;
	int read_count;
	struct thread_lock_entry_t *next;
} thread_lock_entry_t;

/* Head of the per-thread linked list of thread_lock_entry_t nodes.
   Stored in TLS — one instance per thread, allocated on first use. */
typedef struct thread_lock_map_t
{
	thread_lock_entry_t *head;
} thread_lock_map_t;

/* One node in the FIFO wait queue. Each thread that cannot acquire
   immediately allocates one, enqueues it, and sleeps on cv. */
typedef struct fifo_rwlock_node_t
{
	boole is_writer;
	pthread_cond_t cv;
	unsigned int ticket;
	struct fifo_rwlock_node_t *next;
} fifo_rwlock_node_t;

typedef struct custom_rwlock_t
{
	pthread_mutex_t mutex; /* guards all fields below */

	/* FIFO ticket system — every caller takes a ticket from next_ticket,
	   then waits until serving_ticket reaches their ticket value.
	   unsigned int gives well-defined wrap-around after 2^32 cycles. */
	unsigned int next_ticket;
	unsigned int serving_ticket;

	/* Writer identity and recursion depth.
	   write_recursion > 0 means a writer holds the lock.
	   writer_thread is the pthread_t of that writer.
	   Nested write_lock calls increment write_recursion; each clib__unlock
	   decrements it; the lock is released when it reaches 0. */
	pthread_t writer_thread;
	int write_recursion;

	/* Total number of active reader acquisitions, including recursive ones.
	   Does NOT count nested reads acquired by the current writer (those go
	   through fast-path 1 and only increment the per-thread TLS counter). */
	int num_readers;

	fifo_rwlock_node_t *queue_head;
	fifo_rwlock_node_t *queue_tail;

	/* Read-to-write upgrade state.
	   upgrade_pending is set to 1 while a thread is waiting to upgrade its
	   read lock to a write lock. During this window, new slow-path readers
	   are blocked so the upgrader cannot be indefinitely delayed. upgrade_cv
	   is the condition the upgrader sleeps on while waiting for other readers
	   to drain. */
	int upgrade_pending;
	pthread_cond_t upgrade_cv;
} custom_rwlock_t;

void clib__rwlock_init(custom_rwlock_t *lock);
void clib__rwlock_destroy(custom_rwlock_t *lock);
void clib__read_lock(custom_rwlock_t *lock);
void clib__write_lock(custom_rwlock_t *lock); /* also upgrades automatically if called while holding a read lock */
void clib__unlock(custom_rwlock_t *lock);

#endif
