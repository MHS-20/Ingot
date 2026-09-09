/* test_rwlock.c — exercises overlap of concurrent readers, writer
 * exclusivity, try* semantics, and writer preference under a read stream. */
#include "../src/rwlock.h"
#include "../src/common.h"
#include "harness.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

typedef struct {
    ing_rwlock *l;
    atomic_int *active_readers;
    atomic_int *max_active;
    int hold_us;
} reader_arg;

static void *overlap_reader(void *arg)
{
    reader_arg *ra = arg;
    ing_rwlock_rdlock(ra->l);
    int now = atomic_fetch_add(ra->active_readers, 1) + 1;
    int prev_max = atomic_load(ra->max_active);
    while (now > prev_max && !atomic_compare_exchange_weak(ra->max_active, &prev_max, now))
        ;
    usleep(ra->hold_us);
    atomic_fetch_sub(ra->active_readers, 1);
    ing_rwlock_unlock(ra->l);
    return NULL;
}

static void test_readers_overlap(void)
{
    ing_rwlock l;
    ing_rwlock_init(&l);

    atomic_int active_readers = 0;
    atomic_int max_active = 0;
    const int N = 8;
    pthread_t t[8];
    reader_arg args[8];

    for (int i = 0; i < N; i++) {
        args[i] = (reader_arg){ &l, &active_readers, &max_active, 20 * 1000 };
        pthread_create(&t[i], NULL, overlap_reader, &args[i]);
    }
    for (int i = 0; i < N; i++)
        pthread_join(t[i], NULL);

    ING_CHECK(atomic_load(&max_active) > 1, "readers must genuinely overlap, max concurrent was %d", atomic_load(&max_active));

    ing_rwlock_destroy(&l);
}

typedef struct {
    ing_rwlock *l;
    long *shared;   /* incremented non-atomically under the write lock */
    int iters;
} writer_arg;

static void *excl_writer(void *arg)
{
    writer_arg *wa = arg;
    for (int i = 0; i < wa->iters; i++) {
        ing_rwlock_wrlock(wa->l);
        long v = *wa->shared;
        /* Give a competing writer a real chance to interleave if exclusion
         * is broken: read, yield, then write back. */
        usleep(0);
        *wa->shared = v + 1;
        ing_rwlock_unlock(wa->l);
    }
    return NULL;
}

static void test_writer_excludes_all(void)
{
    ing_rwlock l;
    ing_rwlock_init(&l);
    long shared = 0;
    const int NWRITERS = 6;
    const int ITERS = 500;

    pthread_t t[6];
    writer_arg args[6];
    for (int i = 0; i < NWRITERS; i++) {
        args[i] = (writer_arg){ &l, &shared, ITERS };
        pthread_create(&t[i], NULL, excl_writer, &args[i]);
    }
    for (int i = 0; i < NWRITERS; i++)
        pthread_join(t[i], NULL);

    ING_CHECK(shared == (long)NWRITERS * ITERS,
              "non-atomic increment under exclusive lock must total exactly %ld, got %ld",
              (long)NWRITERS * ITERS, shared);

    ing_rwlock_destroy(&l);
}

static void test_try_semantics(void)
{
    ing_rwlock l;
    ing_rwlock_init(&l);

    ING_CHECK(ing_rwlock_tryrdlock(&l) == ING_OK, "first tryrdlock must succeed");
    ING_CHECK(ing_rwlock_tryrdlock(&l) == ING_OK, "second tryrdlock must succeed (readers share)");
    ING_CHECK(ing_rwlock_trywrlock(&l) == ING_EAGAIN, "trywrlock must fail while readers hold the lock");
    ing_rwlock_unlock(&l);
    ing_rwlock_unlock(&l);

    ING_CHECK(ing_rwlock_trywrlock(&l) == ING_OK, "trywrlock on a free lock must succeed");
    ING_CHECK(ing_rwlock_tryrdlock(&l) == ING_EAGAIN, "tryrdlock must fail while a writer holds the lock");
    ING_CHECK(ing_rwlock_trywrlock(&l) == ING_EAGAIN, "trywrlock must fail while another writer holds the lock");
    ing_rwlock_unlock(&l);

    ing_rwlock_destroy(&l);
}

typedef struct {
    ing_rwlock *l;
    atomic_int *stop;
} reader_stream_arg;

static void *reader_stream(void *arg)
{
    reader_stream_arg *ra = arg;
    while (!atomic_load(ra->stop)) {
        ing_rwlock_rdlock(ra->l);
        usleep(1000);
        ing_rwlock_unlock(ra->l);
    }
    return NULL;
}

typedef struct {
    ing_rwlock *l;
    atomic_int acquired;
} writer_wait_arg;

static void *waiting_writer(void *arg)
{
    writer_wait_arg *wa = arg;
    ing_rwlock_wrlock(wa->l);
    atomic_store(&wa->acquired, 1);
    ing_rwlock_unlock(wa->l);
    return NULL;
}

/* Bounded with a generous timeout: under writer preference, a writer must
 * acquire the lock in finite time even against a continuous stream of
 * readers. Failure here means the test FAILs (join times out and we bail),
 * never that the group hangs — the harness's per-group alarm is the
 * ultimate backstop, but this test also polls with its own bound so a
 * starved writer is reported as a clear failure. */
static void test_writer_preference_not_starved(void)
{
    ing_rwlock l;
    ing_rwlock_init(&l);

    atomic_int stop = 0;
    const int NREADERS = 4;
    pthread_t readers[4];
    reader_stream_arg rargs[4];
    for (int i = 0; i < NREADERS; i++) {
        rargs[i] = (reader_stream_arg){ &l, &stop };
        pthread_create(&readers[i], NULL, reader_stream, &rargs[i]);
    }

    usleep(20 * 1000); /* let the reader stream get going first */

    writer_wait_arg wa = { &l, 0 };
    pthread_t writer;
    pthread_create(&writer, NULL, waiting_writer, &wa);

    int acquired = 0;
    for (int i = 0; i < 500; i++) { /* up to ~1s of polling */
        if (atomic_load(&wa.acquired)) {
            acquired = 1;
            break;
        }
        usleep(2000);
    }
    ING_CHECK(acquired, "a waiting writer must eventually acquire under a stream of readers");

    atomic_store(&stop, 1);
    pthread_join(writer, NULL);
    for (int i = 0; i < NREADERS; i++)
        pthread_join(readers[i], NULL);

    ing_rwlock_destroy(&l);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("rwlock: basic semantics");
    ING_RUN(test_readers_overlap);
    ING_RUN(test_writer_excludes_all);
    ING_RUN(test_try_semantics);

    ING_SECTION("rwlock: writer preference");
    ING_RUN(test_writer_preference_not_starved);

    return ing_test_end();
}
