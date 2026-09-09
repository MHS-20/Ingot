/* bqueue.c — see bqueue.h for the design rationale (two condvars, why). */
#include "bqueue.h"
#include "common.h"

#include <pthread.h>
#include <stdlib.h>

struct ing_bqueue {
    void **slots;
    size_t capacity;
    size_t head;      /* index of the next item to dequeue */
    size_t count;     /* number of items currently buffered */
    int closed;

    pthread_mutex_t lock;
    pthread_cond_t not_full;   /* producers wait here; a get() signals it */
    pthread_cond_t not_empty;  /* consumers wait here; a put() signals it */
};

ing_bqueue *ing_bqueue_create(size_t capacity)
{
    if (capacity == 0)
        return NULL;

    ing_bqueue *q = malloc(sizeof *q);
    if (!q)
        return NULL;
    q->slots = malloc(capacity * sizeof *q->slots);
    if (!q->slots) {
        free(q);
        return NULL;
    }
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->closed = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void ing_bqueue_destroy(ing_bqueue *q)
{
    if (!q)
        return;
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->slots);
    free(q);
}

int ing_bqueue_put(ing_bqueue *q, void *item)
{
    pthread_mutex_lock(&q->lock);

    /* while, never if: a broadcast can wake several producers for one
     * freed slot, and between a woken thread returning from cond_wait and
     * it re-acquiring the mutex, another producer can already have taken
     * that slot. The predicate has to be re-checked under the lock every
     * time control returns from the wait, not trusted because the signal
     * fired. */
    while (q->count == q->capacity && !q->closed)
        pthread_cond_wait(&q->not_full, &q->lock);

    if (q->closed) {
        /* Closing refuses new work outright — no point buffering an item
         * behind a shutdown that is already underway. */
        pthread_mutex_unlock(&q->lock);
        return ING_ECLOSED;
    }

    size_t tail = (q->head + q->count) % q->capacity;
    q->slots[tail] = item;
    q->count++;

    /* Wake exactly the class of thread that can use this news: a consumer,
     * not another producer. */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return ING_OK;
}

int ing_bqueue_get(ing_bqueue *q, void **out)
{
    pthread_mutex_lock(&q->lock);

    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0) {
        /* Empty AND closed: nothing left to drain. Reached only once the
         * buffer is exhausted, so every item accepted before close() is
         * guaranteed to reach a consumer first. */
        pthread_mutex_unlock(&q->lock);
        return ING_ECLOSED;
    }

    *out = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return ING_OK;
}

int ing_bqueue_try_put(ing_bqueue *q, void *item)
{
    pthread_mutex_lock(&q->lock);
    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        return ING_ECLOSED;
    }
    if (q->count == q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return ING_EAGAIN;
    }
    size_t tail = (q->head + q->count) % q->capacity;
    q->slots[tail] = item;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return ING_OK;
}

int ing_bqueue_try_get(ing_bqueue *q, void **out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        int rc = q->closed ? ING_ECLOSED : ING_EAGAIN;
        pthread_mutex_unlock(&q->lock);
        return rc;
    }
    *out = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return ING_OK;
}

void ing_bqueue_close(ing_bqueue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    /* Broadcast, not signal: every blocked thread of both classes must
     * observe the shutdown, not just one lucky waiter per condvar. */
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

size_t ing_bqueue_size(ing_bqueue *q)
{
    pthread_mutex_lock(&q->lock);
    size_t n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}
