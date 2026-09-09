/* test_bqueue.c — exercises the bounded queue's blocking, non-blocking, and
 * close semantics, plus a multi-producer/multi-consumer stress run. */
#include "../src/bqueue.h"
#include "../src/common.h"
#include "harness.h"

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

static void test_fifo_order(void)
{
    ing_bqueue *q = ing_bqueue_create(4);
    ING_CHECK(q != NULL, "create failed");

    for (intptr_t i = 0; i < 4; i++)
        ING_CHECK(ing_bqueue_put(q, (void *)i) == ING_OK, "put %ld failed", (long)i);

    for (intptr_t i = 0; i < 4; i++) {
        void *out = NULL;
        int rc = ing_bqueue_get(q, &out);
        ING_CHECK(rc == ING_OK, "get failed rc=%d", rc);
        ING_CHECK((intptr_t)out == i, "expected %ld got %ld", (long)i, (long)(intptr_t)out);
    }

    ing_bqueue_destroy(q);
}

static void test_try_boundaries(void)
{
    ing_bqueue *q = ing_bqueue_create(2);
    void *out;

    ING_CHECK(ing_bqueue_try_get(q, &out) == ING_EAGAIN, "try_get on empty must EAGAIN");

    ING_CHECK(ing_bqueue_try_put(q, (void *)1) == ING_OK, "try_put 1");
    ING_CHECK(ing_bqueue_try_put(q, (void *)2) == ING_OK, "try_put 2");
    ING_CHECK(ing_bqueue_try_put(q, (void *)3) == ING_EAGAIN, "try_put on full must EAGAIN");

    ING_CHECK(ing_bqueue_try_get(q, &out) == ING_OK && (intptr_t)out == 1, "try_get 1");
    ING_CHECK(ing_bqueue_try_get(q, &out) == ING_OK && (intptr_t)out == 2, "try_get 2");
    ING_CHECK(ing_bqueue_try_get(q, &out) == ING_EAGAIN, "try_get on empty again must EAGAIN");

    ing_bqueue_destroy(q);
}

typedef struct {
    ing_bqueue *q;
    volatile int *producer_ran;
    volatile int *consumer_ran;
} block_ctx;

static void *blocking_producer(void *arg)
{
    block_ctx *c = arg;
    /* Queue starts full (capacity 1, already holding one item), so this
     * put must block until the main thread consumes the existing item. */
    ing_bqueue_put(c->q, (void *)42);
    *c->producer_ran = 1;
    return NULL;
}

static void test_put_actually_blocks(void)
{
    ing_bqueue *q = ing_bqueue_create(1);
    ING_CHECK(ing_bqueue_try_put(q, (void *)1) == ING_OK, "prime the queue");

    volatile int producer_ran = 0;
    block_ctx c = { q, &producer_ran, NULL };
    pthread_t t;
    pthread_create(&t, NULL, blocking_producer, &c);

    usleep(100 * 1000); /* give the producer every chance to (wrongly) proceed */
    ING_CHECK(producer_ran == 0, "put on a full queue must not return before room exists");

    void *out;
    ing_bqueue_get(q, &out); /* frees the slot the primed item occupied */

    pthread_join(t, NULL);
    ING_CHECK(producer_ran == 1, "put must complete once a slot frees up");

    ing_bqueue_get(q, &out);
    ing_bqueue_destroy(q);
}

static void *blocked_getter(void *arg)
{
    block_ctx *c = arg;
    void *out;
    int rc = ing_bqueue_get(c->q, &out);
    *c->consumer_ran = rc;
    return NULL;
}

static void test_close_wakes_blocked_waiters(void)
{
    ing_bqueue *q = ing_bqueue_create(1);
    volatile int consumer_rc = -100;
    block_ctx c = { q, NULL, &consumer_rc };
    pthread_t t;
    pthread_create(&t, NULL, blocked_getter, &c);

    usleep(50 * 1000); /* let the consumer actually park in get() */
    ing_bqueue_close(q);
    pthread_join(t, NULL); /* would hang forever if close() failed to wake it */

    ING_CHECK(consumer_rc == ING_ECLOSED, "get on empty+closed queue must report ECLOSED, got %d", consumer_rc);
    ing_bqueue_destroy(q);
}

static void test_close_drains_before_closed(void)
{
    ing_bqueue *q = ing_bqueue_create(4);
    ing_bqueue_put(q, (void *)1);
    ing_bqueue_put(q, (void *)2);
    ing_bqueue_close(q);

    /* Buffered items must still come out even though the queue is closed. */
    void *out;
    ING_CHECK(ing_bqueue_get(q, &out) == ING_OK && (intptr_t)out == 1, "drain 1st buffered item");
    ING_CHECK(ing_bqueue_get(q, &out) == ING_OK && (intptr_t)out == 2, "drain 2nd buffered item");
    /* Only once drained does ECLOSED appear. */
    ING_CHECK(ing_bqueue_get(q, &out) == ING_ECLOSED, "empty+closed must now report ECLOSED");
    /* New puts are refused immediately, no draining semantics for those. */
    ING_CHECK(ing_bqueue_put(q, (void *)3) == ING_ECLOSED, "put after close must be refused");

    ing_bqueue_destroy(q);
}

#define NPRODUCERS 4
#define NCONSUMERS 4
#define NITEMS 5000
#define TOTAL (NPRODUCERS * NITEMS)

typedef struct {
    ing_bqueue *q;
    int producer_id;
} producer_arg;

typedef struct {
    ing_bqueue *q;
    int *seen;          /* TOTAL-sized counter array, shared */
    pthread_mutex_t *seen_lock;
} consumer_arg;

/* Item encodes (producer_id, sequence) so every one of TOTAL items is
 * globally unique and can be checked off exactly once. */
static void *mpmc_producer(void *arg)
{
    producer_arg *pa = arg;
    for (int i = 0; i < NITEMS; i++) {
        intptr_t id = (intptr_t)pa->producer_id * NITEMS + i;
        ing_bqueue_put(pa->q, (void *)id);
    }
    return NULL;
}

static void *mpmc_consumer(void *arg)
{
    consumer_arg *ca = arg;
    for (;;) {
        void *out;
        int rc = ing_bqueue_get(ca->q, &out);
        if (rc == ING_ECLOSED)
            return NULL;
        intptr_t id = (intptr_t)out;
        pthread_mutex_lock(ca->seen_lock);
        ca->seen[id]++;
        pthread_mutex_unlock(ca->seen_lock);
    }
}

static void test_mpmc_exactly_once(void)
{
    ing_bqueue *q = ing_bqueue_create(64);
    int *seen = calloc(TOTAL, sizeof *seen);
    pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_t producers[NPRODUCERS], consumers[NCONSUMERS];
    producer_arg pargs[NPRODUCERS];
    consumer_arg cargs[NCONSUMERS];

    for (int i = 0; i < NCONSUMERS; i++) {
        cargs[i] = (consumer_arg){ q, seen, &seen_lock };
        pthread_create(&consumers[i], NULL, mpmc_consumer, &cargs[i]);
    }
    for (int i = 0; i < NPRODUCERS; i++) {
        pargs[i] = (producer_arg){ q, i };
        pthread_create(&producers[i], NULL, mpmc_producer, &pargs[i]);
    }

    for (int i = 0; i < NPRODUCERS; i++)
        pthread_join(producers[i], NULL);

    /* All producers done and every item enqueued: close so consumers exit
     * once the buffer is drained rather than blocking forever. */
    ing_bqueue_close(q);

    for (int i = 0; i < NCONSUMERS; i++)
        pthread_join(consumers[i], NULL);

    int ok = 1;
    for (int i = 0; i < TOTAL; i++) {
        if (seen[i] != 1) {
            ok = 0;
            break;
        }
    }
    ING_CHECK(ok, "every one of %d items must be received exactly once", TOTAL);

    free(seen);
    pthread_mutex_destroy(&seen_lock);
    ing_bqueue_destroy(q);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("bqueue: basic semantics");
    ING_RUN(test_fifo_order);
    ING_RUN(test_try_boundaries);
    ING_RUN(test_put_actually_blocks);
    ING_RUN(test_close_wakes_blocked_waiters);
    ING_RUN(test_close_drains_before_closed);

    ING_SECTION("bqueue: concurrency stress");
    ING_RUN(test_mpmc_exactly_once);

    return ing_test_end();
}
