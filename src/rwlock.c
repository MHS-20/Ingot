/* rwlock.c — see rwlock.h for the writer-preferring trade-off this makes. */
#include "rwlock.h"
#include "common.h"

int ing_rwlock_init(ing_rwlock *l)
{
    l->readers = 0;
    l->writer_active = 0;
    l->writers_waiting = 0;

    if (pthread_mutex_init(&l->lock, NULL) != 0)
        return ING_EINVAL;
    if (pthread_cond_init(&l->readers_ok, NULL) != 0) {
        pthread_mutex_destroy(&l->lock);
        return ING_EINVAL;
    }
    if (pthread_cond_init(&l->writer_ok, NULL) != 0) {
        pthread_cond_destroy(&l->readers_ok);
        pthread_mutex_destroy(&l->lock);
        return ING_EINVAL;
    }
    return ING_OK;
}

void ing_rwlock_destroy(ing_rwlock *l)
{
    pthread_cond_destroy(&l->readers_ok);
    pthread_cond_destroy(&l->writer_ok);
    pthread_mutex_destroy(&l->lock);
}

void ing_rwlock_rdlock(ing_rwlock *l)
{
    pthread_mutex_lock(&l->lock);
    /* The writers_waiting check is the whole of writer preference: a new
     * reader yields not only to an *active* writer but to a *queued* one,
     * so a writer that has been waiting can never be leapfrogged by a
     * fresh wave of readers that all arrived after it did. while, not if:
     * a broadcast on writer_ok can wake several readers for the same
     * departed writer, and the state must be rechecked after re-acquiring
     * the mutex regardless. */
    while (l->writer_active || l->writers_waiting > 0)
        pthread_cond_wait(&l->readers_ok, &l->lock);
    l->readers++;
    pthread_mutex_unlock(&l->lock);
}

void ing_rwlock_wrlock(ing_rwlock *l)
{
    pthread_mutex_lock(&l->lock);
    l->writers_waiting++;
    /* Counted as "waiting" for the whole time this loop runs, including
     * while some other writer is currently active — that is what blocks
     * new readers from cutting in front of a writer that is merely
     * queued, not yet running. */
    while (l->writer_active || l->readers > 0)
        pthread_cond_wait(&l->writer_ok, &l->lock);
    l->writers_waiting--;
    l->writer_active = 1;
    pthread_mutex_unlock(&l->lock);
}

void ing_rwlock_unlock(ing_rwlock *l)
{
    pthread_mutex_lock(&l->lock);
    if (l->writer_active) {
        l->writer_active = 0;
        if (l->writers_waiting > 0)
            /* Hand off to the next writer first — that is the preference
             * policy applied on the way out, not just on the way in. */
            pthread_cond_signal(&l->writer_ok);
        else
            /* No writer queued: every waiting reader can now proceed
             * together, hence broadcast rather than signal. */
            pthread_cond_broadcast(&l->readers_ok);
    } else {
        l->readers--;
        if (l->readers == 0 && l->writers_waiting > 0)
            /* Only the last reader out can hand off to a writer: any
             * earlier signal would fire while other readers are still
             * active, and the writer's own while-loop would just go back
             * to sleep having consumed a wakeup meant for whoever the
             * true last reader turns out to be. */
            pthread_cond_signal(&l->writer_ok);
    }
    pthread_mutex_unlock(&l->lock);
}

int ing_rwlock_tryrdlock(ing_rwlock *l)
{
    pthread_mutex_lock(&l->lock);
    if (l->writer_active || l->writers_waiting > 0) {
        pthread_mutex_unlock(&l->lock);
        return ING_EAGAIN;
    }
    l->readers++;
    pthread_mutex_unlock(&l->lock);
    return ING_OK;
}

int ing_rwlock_trywrlock(ing_rwlock *l)
{
    pthread_mutex_lock(&l->lock);
    if (l->writer_active || l->readers > 0) {
        pthread_mutex_unlock(&l->lock);
        return ING_EAGAIN;
    }
    l->writer_active = 1;
    pthread_mutex_unlock(&l->lock);
    return ING_OK;
}
