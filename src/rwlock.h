/* rwlock.h — a reader-writer lock, hand-built rather than wrapping
 * pthread_rwlock_t (wrapping it would skip the point of the exercise).
 *
 * Built from one pthread_mutex_t protecting three counters, plus two
 * condition variables to wait on those counters.
 *
 * This lock is *writer-preferring*: once a writer is waiting, no new reader
 * is admitted, even if readers are already active and could in principle
 * share the lock with more readers. That is a real trade-off, not a free
 * lunch:
 *
 *   - Reader-preferring (a waiting writer never blocks a new reader) is the
 *     "obvious" design and it is wrong under sustained read load: a steady
 *     trickle of new readers can keep at least one reader active forever,
 *     so a writer waiting for readers to drain to zero may wait
 *     indefinitely. That is writer starvation, and it is the classic bug.
 *   - Writer-preferring (this file) fixes that by giving a waiting writer
 *     priority over readers that have not yet started: once one writer is
 *     queued, every new rdlock call blocks until that writer has run,
 *     however briefly it holds the lock. Under a workload with a rapid
 *     back-to-back stream of writers, readers can now be the ones starved.
 *
 * Neither policy is universally correct; it is a bet on your workload. This
 * module bets that writes are rarer and more important to make progress on
 * than reads (a cache invalidation, a config reload) — the classic case to
 * regret this choice is a write-heavy or write-bursty workload where reads
 * need low, predictable latency (e.g. a request-per-thread web server
 * reading shared config under bursty admin writes): here, readers can queue
 * up behind writers for long stretches. Swap the priority test in rdlock
 * (drop the writers_waiting check) to go back to reader-preferring.
 */
#ifndef ING_RWLOCK_H
#define ING_RWLOCK_H

#include <pthread.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t readers_ok;   /* waiting readers wait here */
    pthread_cond_t writer_ok;    /* waiting writers wait here */
    int readers;                /* number of readers currently holding the lock */
    int writer_active;          /* 0 or 1: a writer currently holds the lock */
    int writers_waiting;        /* number of writers blocked in wrlock */
} ing_rwlock;

/* ING_OK, or ING_ENOMEM/ING_EINVAL if the underlying pthread objects fail
 * to initialize. */
int ing_rwlock_init(ing_rwlock *l);
void ing_rwlock_destroy(ing_rwlock *l);

void ing_rwlock_rdlock(ing_rwlock *l);
void ing_rwlock_wrlock(ing_rwlock *l);

/* Releases whichever mode the caller holds. There is deliberately no
 * separate rdunlock/wrunlock: the lock's own counters (readers > 0 vs.
 * writer_active) already record which mode is held, so making the caller
 * repeat that information back is one more way for caller and lock to
 * disagree. The one obligation this places on the caller is not calling
 * unlock from a thread that holds neither mode, which is true of any lock. */
void ing_rwlock_unlock(ing_rwlock *l);

int ing_rwlock_tryrdlock(ing_rwlock *l);   /* ING_OK or ING_EAGAIN */
int ing_rwlock_trywrlock(ing_rwlock *l);   /* ING_OK or ING_EAGAIN */

#endif /* ING_RWLOCK_H */
