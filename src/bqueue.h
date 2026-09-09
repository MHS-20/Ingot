/* bqueue.h — a bounded, thread-safe producer-consumer queue.
 *
 * A ring buffer of `void *` slots guarded by one mutex and *two* condition
 * variables. Two, not one, is the actual lesson of this module:
 *
 *   - With a single condvar and broadcast-on-every-change, every waiter
 *     wakes on every change, including producers waking because a *producer*
 *     made room disappear, or consumers waking because a *consumer* took the
 *     last item. That is correct (predicates are still re-checked) but it is
 *     O(n) wakeups for O(1) useful work under contention.
 *   - With a single condvar and signal-on-every-change, you can wake the
 *     *wrong class* of waiter: a `put` that frees no slot for another
 *     producer signals a thread that happens to be a producer, that producer
 *     rechecks "is there room" — no — and goes back to sleep having
 *     consumed the wakeup meant for a consumer, who now sleeps forever with
 *     an item sitting in the queue. Splitting into `not_full` (producers
 *     wait here, consumers signal it) and `not_empty` (consumers wait here,
 *     producers signal it) means a signal always reaches a thread that can
 *     actually make use of it.
 *
 * Opaque and heap-allocated: callers never see the ring buffer or the locks,
 * so the layout can change without breaking anyone.
 */
#ifndef ING_BQUEUE_H
#define ING_BQUEUE_H

#include <stddef.h>

typedef struct ing_bqueue ing_bqueue;

/* NULL on invalid capacity (0) or allocation failure. */
ing_bqueue *ing_bqueue_create(size_t capacity);

/* Safe to call once the queue is idle (no thread blocked in put/get). Calling
 * it while another thread is still using the queue is a use-after-free
 * waiting to happen, same as freeing any other object out from under a
 * live user. */
void ing_bqueue_destroy(ing_bqueue *q);

/* Blocks while the queue is full. Returns ING_ECLOSED without enqueuing if
 * the queue has been closed — a closed queue accepts no new work, closing
 * being a one-way trip. */
int ing_bqueue_put(ing_bqueue *q, void *item);

/* Blocks while the queue is empty. Returns ING_ECLOSED only once the queue
 * is both closed *and* drained — see ing_bqueue_close for why that ordering
 * is the useful one. */
int ing_bqueue_get(ing_bqueue *q, void **out);

/* Non-blocking variants: ING_EAGAIN instead of waiting, ING_ECLOSED under the
 * same rules as the blocking form above. */
int ing_bqueue_try_put(ing_bqueue *q, void *item);
int ing_bqueue_try_get(ing_bqueue *q, void **out);

/* Wakes every thread currently blocked in put or get. Producers blocked in
 * put see ING_ECLOSED immediately: there is no point admitting more work to
 * a queue that is shutting down. Consumers blocked in get keep receiving
 * whatever is already buffered — draining before closing means work already
 * accepted is never silently dropped, only *new* work is refused. Only once
 * the buffer is empty does get start returning ING_ECLOSED. */
void ing_bqueue_close(ing_bqueue *q);

size_t ing_bqueue_size(ing_bqueue *q);

#endif /* ING_BQUEUE_H */
