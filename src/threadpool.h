/* threadpool.h — a fixed pool of worker threads pulling from a task queue.
 *
 * Deliberately does not #include bqueue.h and does not reuse its ring
 * buffer implementation, even though the task queue inside threadpool.c is
 * built on the same mutex+condvar ring-buffer idea. Day 6 and day 7 are
 * meant to be readable in isolation — a reader who has only seen this file
 * should not have to go read bqueue.c to understand how tasks are queued,
 * and a change to bqueue's internals should never be able to break the
 * pool. The duplication is small and the independence is worth it.
 */
#ifndef ING_THREADPOOL_H
#define ING_THREADPOOL_H

#include <stddef.h>

typedef struct ing_threadpool ing_threadpool;
typedef void (*ing_task_fn)(void *arg);

/* NULL on nworkers == 0, queue_capacity == 0, or allocation failure. */
ing_threadpool *ing_threadpool_create(size_t nworkers, size_t queue_capacity);

/* Enqueues (fn, arg) to run on some worker. Blocks while the queue is full.
 * ING_ECLOSED if the pool is shutting down or already destroyed-in-progress. */
int ing_threadpool_submit(ing_threadpool *p, ing_task_fn fn, void *arg);

/* Blocks until every task submitted so far has *finished running* — not
 * merely been dequeued. See threadpool.c for why that distinction is the
 * whole difficulty of this function. */
int ing_threadpool_wait_idle(ing_threadpool *p);

/* Stops accepting new work, lets every already-queued task run to
 * completion, joins every worker thread, then frees the pool. Safe to call
 * with work still outstanding. */
void ing_threadpool_destroy(ing_threadpool *p);

size_t ing_threadpool_workers(const ing_threadpool *p);

#endif /* ING_THREADPOOL_H */
