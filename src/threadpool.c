/* threadpool.c — see threadpool.h for why this does not reuse bqueue.c. */
#include "threadpool.h"
#include "common.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct {
    ing_task_fn fn;
    void *arg;
} ing_task;

struct ing_threadpool {
    /* --- task ring buffer: same shape as bqueue's, kept private here --- */
    ing_task *slots;
    size_t capacity;
    size_t head;
    size_t qcount;      /* tasks currently sitting in the ring */

    /* --- in-flight accounting: the actual point of this module --- */
    size_t inflight;    /* queued *plus* currently executing. See wait_idle. */

    int shutting_down;

    pthread_mutex_t lock;
    pthread_cond_t not_full;    /* submitters wait here */
    pthread_cond_t not_empty;   /* workers wait here */
    pthread_cond_t idle;        /* wait_idle waits here for inflight == 0 */

    pthread_t *workers;
    size_t nworkers;
};

static void *worker_main(void *arg)
{
    ing_threadpool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (p->qcount == 0 && !p->shutting_down)
            pthread_cond_wait(&p->not_empty, &p->lock);

        if (p->qcount == 0 && p->shutting_down) {
            /* Nothing left to drain and no more will arrive: exit. */
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }

        ing_task t = p->slots[p->head];
        p->head = (p->head + 1) % p->capacity;
        p->qcount--;
        pthread_cond_signal(&p->not_full);
        pthread_mutex_unlock(&p->lock);

        /* The task runs with the lock released — a slow task must not
         * stall every other submitter and worker. `inflight` already
         * counted this task (set at submit time) and stays incremented
         * for the task's entire execution, not just while it sat in the
         * ring. That is the one fact this whole module exists to get
         * right: if inflight dropped the moment the task was dequeued,
         * wait_idle could observe inflight == 0 and return while this
         * task is still running its body below, which is exactly the bug
         * the task description calls out. */
        t.fn(t.arg);

        pthread_mutex_lock(&p->lock);
        p->inflight--;
        if (p->inflight == 0)
            pthread_cond_broadcast(&p->idle);
        pthread_mutex_unlock(&p->lock);
    }
}

ing_threadpool *ing_threadpool_create(size_t nworkers, size_t queue_capacity)
{
    if (nworkers == 0 || queue_capacity == 0)
        return NULL;

    ing_threadpool *p = malloc(sizeof *p);
    if (!p)
        return NULL;

    p->slots = malloc(queue_capacity * sizeof *p->slots);
    p->workers = malloc(nworkers * sizeof *p->workers);
    if (!p->slots || !p->workers) {
        free(p->slots);
        free(p->workers);
        free(p);
        return NULL;
    }

    p->capacity = queue_capacity;
    p->head = 0;
    p->qcount = 0;
    p->inflight = 0;
    p->shutting_down = 0;
    p->nworkers = nworkers;

    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->not_full, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->idle, NULL);

    for (size_t i = 0; i < nworkers; i++) {
        if (pthread_create(&p->workers[i], NULL, worker_main, p) != 0) {
            /* Roll back: tell already-started workers to shut down, join
             * them, then free. Simpler than a partial-pool state machine,
             * and thread-creation failure this deep into startup is rare
             * enough that simplicity wins. */
            pthread_mutex_lock(&p->lock);
            p->shutting_down = 1;
            pthread_cond_broadcast(&p->not_empty);
            pthread_mutex_unlock(&p->lock);
            for (size_t j = 0; j < i; j++)
                pthread_join(p->workers[j], NULL);
            pthread_cond_destroy(&p->not_full);
            pthread_cond_destroy(&p->not_empty);
            pthread_cond_destroy(&p->idle);
            pthread_mutex_destroy(&p->lock);
            free(p->slots);
            free(p->workers);
            free(p);
            return NULL;
        }
    }

    return p;
}

int ing_threadpool_submit(ing_threadpool *p, ing_task_fn fn, void *arg)
{
    pthread_mutex_lock(&p->lock);

    while (p->qcount == p->capacity && !p->shutting_down)
        pthread_cond_wait(&p->not_full, &p->lock);

    if (p->shutting_down) {
        pthread_mutex_unlock(&p->lock);
        return ING_ECLOSED;
    }

    size_t tail = (p->head + p->qcount) % p->capacity;
    p->slots[tail].fn = fn;
    p->slots[tail].arg = arg;
    p->qcount++;
    /* Counted as in-flight from the instant it is accepted, not from the
     * instant a worker picks it up — see worker_main for why that matters. */
    p->inflight++;

    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return ING_OK;
}

int ing_threadpool_wait_idle(ing_threadpool *p)
{
    pthread_mutex_lock(&p->lock);
    while (p->inflight != 0)
        pthread_cond_wait(&p->idle, &p->lock);
    pthread_mutex_unlock(&p->lock);
    return ING_OK;
}

void ing_threadpool_destroy(ing_threadpool *p)
{
    if (!p)
        return;

    pthread_mutex_lock(&p->lock);
    p->shutting_down = 1;
    /* Every worker may be parked in not_empty; wake them all so each can
     * either grab remaining work or see shutdown+empty and exit. */
    pthread_cond_broadcast(&p->not_empty);
    /* Also wake any submitter blocked on a full queue — it must see
     * ING_ECLOSED rather than wait forever for room that will never
     * reopen once shutdown starts. */
    pthread_cond_broadcast(&p->not_full);
    pthread_mutex_unlock(&p->lock);

    /* Join before freeing anything, with no exception: a worker that is
     * still running when p->slots or p->lock is freed would read or lock
     * memory that no longer belongs to this object. Joining is the only
     * proof a thread has actually stopped touching `p` — a flag it might
     * still be in the middle of checking does not count. */
    for (size_t i = 0; i < p->nworkers; i++)
        pthread_join(p->workers[i], NULL);

    pthread_cond_destroy(&p->not_full);
    pthread_cond_destroy(&p->not_empty);
    pthread_cond_destroy(&p->idle);
    pthread_mutex_destroy(&p->lock);
    free(p->slots);
    free(p->workers);
    free(p);
}

size_t ing_threadpool_workers(const ing_threadpool *p)
{
    return p->nworkers;
}
