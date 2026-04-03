/*
 * clib_rwlock.c — reentrant, FIFO-fair, upgradeable reader-writer lock
 *
 * See clib_rwlock.h for the full design rationale and usage notes.
 *
 * TICKET SYSTEM MECHANICS
 * -----------------------
 * Every caller that reaches the slow path takes a ticket (next_ticket++) and
 * enqueues a node. They then sleep until serving_ticket equals their ticket
 * AND any remaining exclusion condition is clear (write_recursion, num_readers,
 * upgrade_pending depending on caller type). After acquiring, the caller
 * dequeues its node, increments serving_ticket, and signals the new queue_head
 * so the next waiter can re-check its condition. This gives FIFO ordering with
 * concurrent readers: a reader signals the next waiter immediately after
 * acquiring, so a run of consecutive reader tickets all acquire in parallel
 * before the first queued writer gets to go.
 *
 * KEY INVARIANTS
 * --------------
 * 1. num_readers == 0 whenever write_recursion > 0 (writer has exclusive access).
 * 2. write_recursion == 0 whenever num_readers > 0.
 * 3. num_readers does NOT count reads acquired via fast-path 1 (writer's nested
 *    reads). Those only update the per-thread TLS counter.
 * 4. upgrade_pending can only be 1 while write_recursion == 0 (the upgrader is
 *    still a reader, not yet a writer).
 */

#include "../definitions.h"
#include "clib_rwlock.h"
#include "../../third-party/rxi-log/log.h"

/* =============================================================================
   TLS — per-thread read count map
   =============================================================================
   One thread_lock_map_t per thread (stored in TLS), containing a linked list
   of thread_lock_entry_t — one entry per lock this thread has ever touched.
   The list stays alive for the thread's lifetime (freed by tls_destructor).
   Entries are allocated on first touch and never removed, but the list is
   bounded by (number of live threads × number of distinct locks), which is
   small in practice.
   ============================================================================= */

static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;

/* Called by pthreads when a thread exits — frees this thread's entire map. */
static void tls_destructor(void *data)
{
	thread_lock_map_t *map = data;
	thread_lock_entry_t *e = map->head;
	while (e)
	{
		thread_lock_entry_t *tmp = e;
		e = e->next;
		free(tmp);
	}
	free(map);
}

/* pthread_once callback: creates the TLS key exactly once for the process. */
static void make_tls_key(void)
{
	pthread_key_create(&tls_key, tls_destructor);
}

/* Returns the lock map for the calling thread, allocating it on first call. */
static thread_lock_map_t *get_thread_map(void)
{
	pthread_once(&tls_once, make_tls_key);
	thread_lock_map_t *map = pthread_getspecific(tls_key);
	if (!map)
	{
		map = calloc(1, sizeof(*map));
		if (!map)
		{
			abort();
		}
		pthread_setspecific(tls_key, map);
	}
	return map;
}

/*
 * Returns a pointer to the read-count integer for the calling thread and the
 * given lock pointer. Allocates a new entry (count = 0) on first use.
 * Linear scan is fine: the list has at most one entry per distinct lock held.
 */
static int *get_read_count(custom_rwlock_t *lock)
{
	thread_lock_map_t *map = get_thread_map();
	thread_lock_entry_t *e = map->head;

	while (e)
	{
		if (e->lock == lock)
		{
			return &e->read_count;
		}
		e = e->next;
	}

	e = calloc(1, sizeof(*e));
	if (!e)
	{
		abort();
	}
	e->lock = lock;
	e->read_count = 0;
	e->next = map->head;
	map->head = e;
	return &e->read_count;
}

/* =============================================================================
   FIFO queue helpers
   ============================================================================= */

/* Append node to the tail of the queue. Called with lock->mutex held. */
static void enqueue(custom_rwlock_t *lock, fifo_rwlock_node_t *node)
{
	node->next = NULL;
	if (!lock->queue_tail)
	{
		lock->queue_head = lock->queue_tail = node;
	}
	else
	{
		lock->queue_tail->next = node;
		lock->queue_tail = node;
	}
}

/* Remove and return the head of the queue. Returns NULL if empty.
   Called with lock->mutex held. */
static fifo_rwlock_node_t *dequeue(custom_rwlock_t *lock)
{
	fifo_rwlock_node_t *node = lock->queue_head;
	if (node)
	{
		lock->queue_head = node->next;
		if (!lock->queue_head)
		{
			lock->queue_tail = NULL;
		}
	}
	return node;
}

/* =============================================================================
   Public API
   ============================================================================= */

void clib__rwlock_init(custom_rwlock_t *lock)
{
	pthread_mutex_init(&lock->mutex, NULL);
	lock->next_ticket = 0;
	lock->serving_ticket = 0;
	lock->writer_thread = 0;
	lock->write_recursion = 0;
	lock->num_readers = 0;
	lock->queue_head = NULL;
	lock->queue_tail = NULL;
	lock->upgrade_pending = 0;
	pthread_cond_init(&lock->upgrade_cv, NULL);
}

/*
 * Destroy the lock. The caller MUST ensure no threads are waiting or holding
 * the lock at the time of this call. Drains any leftover queue nodes defensively
 * (there should be none in a correct shutdown) to avoid leaking condvars.
 */
void clib__rwlock_destroy(custom_rwlock_t *lock)
{
	pthread_mutex_lock(&lock->mutex);

	fifo_rwlock_node_t *node = lock->queue_head;
	while (node)
	{
		fifo_rwlock_node_t *tmp = node;
		node = node->next;
		pthread_cond_destroy(&tmp->cv);
		free(tmp);
	}
	lock->queue_head = lock->queue_tail = NULL;

	pthread_cond_destroy(&lock->upgrade_cv);
	pthread_mutex_unlock(&lock->mutex);
	pthread_mutex_destroy(&lock->mutex);
}

/* -----------------------------------------------------------------------------
   clib__read_lock

   Acquisition order:
     Fast-path 1  — this thread already holds the WRITE lock (write→read nesting)
     Fast-path 2  — this thread already holds a READ lock (recursive read)
     Slow path    — first acquisition: take FIFO ticket and wait
   ----------------------------------------------------------------------------- */
void clib__read_lock(custom_rwlock_t *lock)
{
	DBG_RWLOCKS log_info("%s", "clib__read_lock ");

	pthread_mutex_lock(&lock->mutex);
	pthread_t self = pthread_self();

	int *my_read_count = get_read_count(lock);

	/*
	 * Fast-path 1: this thread already holds the write lock.
	 *
	 * Allow the nested read without queuing. num_readers is intentionally NOT
	 * incremented: the writer already has exclusive access, and incrementing
	 * would leave num_readers > 0 after the write lock is released, which would
	 * block subsequent writers from acquiring. The TLS counter is incremented
	 * so clib__unlock can distinguish these nested reads from the write lock
	 * itself and release them in LIFO order.
	 */
	if (lock->write_recursion > 0 && pthread_equal(lock->writer_thread, self))
	{
		(*my_read_count)++;
		pthread_mutex_unlock(&lock->mutex);
		return;
	}

	/*
	 * Fast-path 2: this thread already holds a read lock on this same lock.
	 *
	 * A naïve implementation would take a new FIFO ticket and wait. This
	 * deadlocks if a writer is already queued: the thread would wait for
	 * serving_ticket to advance past the writer's position, but serving_ticket
	 * cannot advance until all current readers release — including this thread,
	 * which is now blocked. Bypassing the queue here breaks the cycle.
	 */
	if (*my_read_count > 0)
	{
		(*my_read_count)++;
		lock->num_readers++;
		pthread_mutex_unlock(&lock->mutex);
		return;
	}

	/*
	 * Slow path: first acquisition for this thread on this lock.
	 *
	 * Take a FIFO ticket and wait until ALL three conditions are clear:
	 *   (a) our ticket is at the front (FIFO ordering)
	 *   (b) no writer currently holds the lock (write_recursion == 0)
	 *   (c) no thread is mid-upgrade (upgrade_pending == 0)
	 *       — this prevents new readers from sneaking in and delaying an
	 *         upgrader that is waiting for the reader count to drain.
	 *
	 * After acquiring, we signal the next waiter immediately. If it is also
	 * a reader, it will acquire concurrently (parallel readers). If it is a
	 * writer, it sees num_readers > 0 and goes back to sleep.
	 */
	unsigned int ticket = lock->next_ticket++;

	fifo_rwlock_node_t *node = calloc(1, sizeof(*node));
	if (!node)
	{
		pthread_mutex_unlock(&lock->mutex);
		abort();
	}
	pthread_cond_init(&node->cv, NULL);
	node->is_writer = false;
	node->ticket = ticket;

	enqueue(lock, node);

	while (node->ticket != lock->serving_ticket || lock->write_recursion > 0 || lock->upgrade_pending)
	{
		pthread_cond_wait(&node->cv, &lock->mutex);
	}

	(*my_read_count)++;
	lock->num_readers++;

	dequeue(lock);
	lock->serving_ticket++;

	if (lock->queue_head)
	{
		pthread_cond_signal(&lock->queue_head->cv);
	}

	pthread_cond_destroy(&node->cv);
	free(node);

	pthread_mutex_unlock(&lock->mutex);
}

/* -----------------------------------------------------------------------------
   clib__write_lock

   Three cases handled internally:

     Case 1 — recursive write: same thread already owns the write lock.
              Increment write_recursion and return.

     Case 2 — upgrade: this thread already holds a read lock.
              Cannot take a normal FIFO ticket (would deadlock — see comment
              inside). Instead waits for other readers to drain, then converts
              atomically. If another upgrade is already in progress, surrenders
              reads and falls through to the normal ticket path.

     Case 3 — normal slow path: take a FIFO ticket and wait until the lock is
              fully free (num_readers == 0 AND write_recursion == 0).
   ----------------------------------------------------------------------------- */
void clib__write_lock(custom_rwlock_t *lock)
{
	DBG_RWLOCKS log_info("%s", "clib__write_lock ");

	pthread_mutex_lock(&lock->mutex);
	pthread_t self = pthread_self();

	/* ---- Case 1: recursive write ------------------------------------------ */
	if (lock->write_recursion > 0 && pthread_equal(lock->writer_thread, self))
	{
		lock->write_recursion++;
		pthread_mutex_unlock(&lock->mutex);
		return;
	}

	int *my_read_count = get_read_count(lock);

	/* ---- Case 2: read-to-write upgrade ------------------------------------- */
	if (*my_read_count > 0)
	{
		if (!lock->upgrade_pending)
		{
			/*
			 * We got the upgrade slot. Setting upgrade_pending = 1 prevents new
			 * slow-path readers from acquiring, so we cannot be starved.
			 * We then wait for all OTHER readers to release. Our own reads are
			 * still counted in num_readers, so the condition is
			 * num_readers > *my_read_count (other readers still active).
			 * clib__unlock signals upgrade_cv each time a reader releases.
			 */
			lock->upgrade_pending = 1;

			while (lock->num_readers > *my_read_count)
			{
				pthread_cond_wait(&lock->upgrade_cv, &lock->mutex);
			}

			/*
			 * We are now the sole reader. Atomically convert: subtract our
			 * reads from num_readers, zero the TLS counter, and become the
			 * writer. All N read acquisitions collapse into write_recursion = 1.
			 * One clib__unlock will release the write lock.
			 */
			lock->num_readers -= *my_read_count;
			*my_read_count = 0;
			lock->writer_thread = self;
			lock->write_recursion = 1;
			lock->upgrade_pending = 0;

			/*
			 * Wake the queue head. Any readers stalled by upgrade_pending will
			 * re-check conditions, see write_recursion > 0, and go back to sleep
			 * until we release. Writers in the queue also wait for
			 * write_recursion == 0 and will sleep too.
			 */
			if (lock->queue_head)
			{
				pthread_cond_signal(&lock->queue_head->cv);
			}

			pthread_mutex_unlock(&lock->mutex);
			return;
		}
		else
		{
			/*
			 * Another thread is already mid-upgrade. If we also wait for our
			 * reads to drain we deadlock — each waiter holds a read that the
			 * other needs to release before it can proceed. Surrender our reads
			 * now and re-enter as a normal write-lock contender via the FIFO
			 * queue. Another writer may acquire before we do, but that is
			 * unavoidable and safe.
			 */
			lock->num_readers -= *my_read_count;
			*my_read_count = 0;

			/* Signal the upgrader — it may now be the only remaining reader. */
			pthread_cond_signal(&lock->upgrade_cv);

			/* Fall through to Case 3. */
		}
	}

	/* ---- Case 3: normal slow path ----------------------------------------- */
	/*
	 * Wait until:
	 *   (a) our ticket is at the front of the FIFO queue
	 *   (b) no readers are active          (num_readers == 0)
	 *   (c) no other writer holds the lock (write_recursion == 0)
	 *
	 * Condition (c) is necessary because serving_ticket advances when a
	 * holder ACQUIRES (not when it releases). Without it, a second writer
	 * arriving after the first has already acquired would find its ticket
	 * matching serving_ticket and num_readers == 0, and would incorrectly
	 * skip the wait — giving two simultaneous writers.
	 */
	unsigned int ticket = lock->next_ticket++;

	fifo_rwlock_node_t *node = calloc(1, sizeof(*node));
	if (!node)
	{
		pthread_mutex_unlock(&lock->mutex);
		abort();
	}
	pthread_cond_init(&node->cv, NULL);
	node->is_writer = true;
	node->ticket = ticket;

	enqueue(lock, node);

	while (node->ticket != lock->serving_ticket || lock->num_readers > 0 || lock->write_recursion > 0)
	{
		pthread_cond_wait(&node->cv, &lock->mutex);
	}

	lock->writer_thread = self;
	lock->write_recursion = 1;

	dequeue(lock);
	lock->serving_ticket++;

	/*
	 * Signal the next waiter even though we just took exclusive access.
	 * It will re-check conditions, find write_recursion > 0, and sleep.
	 * This keeps the wakeup chain live — without it, a waiter that arrived
	 * just before our signal here would have no one to wake it later.
	 */
	if (lock->queue_head)
	{
		pthread_cond_signal(&lock->queue_head->cv);
	}

	pthread_cond_destroy(&node->cv);
	free(node);

	pthread_mutex_unlock(&lock->mutex);
}

/* -----------------------------------------------------------------------------
   clib__unlock

   Identifies which lock type this thread last acquired and releases one level:

     A. Writer with nested reads outstanding (write_recursion > 0 AND my_read_count > 0)
        → release one nested read (TLS counter only; num_readers was never
          incremented for writer-nested reads per fast-path 1 above).

     B. Writer, no nested reads (write_recursion > 0 AND my_read_count == 0)
        → release one write recursion level. If write_recursion hits 0, clear
          writer identity and wake the next queued waiter.

     C. Reader (not the current writer)
        → decrement TLS counter and num_readers. Signal the upgrader if one is
          waiting. Wake the next queued waiter if the lock is now fully free.

   The LIFO ordering between A and B is automatic: as long as the caller
   matches each clib__read_lock / clib__write_lock with exactly one clib__unlock,
   the nested reads are always released before the write lock.
   ----------------------------------------------------------------------------- */
void clib__unlock(custom_rwlock_t *lock)
{
	pthread_mutex_lock(&lock->mutex);
	pthread_t self = pthread_self();

	int *my_read_count = get_read_count(lock);

	bool is_writer = (lock->write_recursion > 0 && pthread_equal(lock->writer_thread, self));

	if (is_writer && *my_read_count > 0)
	{
		/* A: release one writer-nested read */
		DBG_RWLOCKS log_info("%s", "clib__unlock: nested-read under writer ");
		(*my_read_count)--;
	}
	else if (is_writer)
	{
		/* B: release one write recursion level */
		DBG_RWLOCKS log_info("%s", "clib__unlock: write ");
		lock->write_recursion--;
		if (lock->write_recursion == 0)
		{
			lock->writer_thread = 0;
		}
	}
	else if (*my_read_count > 0)
	{
		/* C: release one read */
		DBG_RWLOCKS log_info("%s", "clib__unlock: read ");
		(*my_read_count)--;
		lock->num_readers--;

		/* If a thread is mid-upgrade, it wakes here to check whether it is
		   now the sole remaining reader. */
		if (lock->upgrade_pending)
		{
			pthread_cond_signal(&lock->upgrade_cv);
		}
	}
	else
	{
		/* Called without holding this lock — log and ignore rather than
		   silently corrupting state. */
		DBG_RWLOCKS log_info("%s", "clib__unlock: WARNING called by thread that does not hold this lock");
		pthread_mutex_unlock(&lock->mutex);
		return;
	}

	/* Wake the next queued waiter when the lock is fully free. */
	if (lock->num_readers == 0 && lock->write_recursion == 0 && lock->queue_head)
	{
		pthread_cond_signal(&lock->queue_head->cv);
	}

	pthread_mutex_unlock(&lock->mutex);
}
