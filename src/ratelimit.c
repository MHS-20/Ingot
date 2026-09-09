/* ratelimit.c — see ratelimit.h for the design rationale. */
#include "ratelimit.h"
#include "common.h"

#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    /* CLOCK_MONOTONIC, never CLOCK_REALTIME: the wall clock can jump. An NTP
     * step, a DST transition, or an operator setting the clock back would
     * either stall every bucket for hours (clock moved forward then
     * corrected back) or hand out an unbounded burst (clock moved back, so
     * "elapsed" looks huge). Monotonic time only ever moves forward, at a
     * steady rate, so elapsed-time arithmetic is always sane. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------- */
/* Token bucket                                                          */
/* -------------------------------------------------------------------- */

int ing_bucket_init(ing_bucket *b, double rate_per_sec, double burst)
{
    if (rate_per_sec <= 0.0 || burst <= 0.0)
        return ING_EINVAL;

    if (pthread_mutex_init(&b->mu, NULL) != 0)
        return ING_ENOMEM;

    b->rate = rate_per_sec;
    b->burst = burst;
    b->tokens = burst;      /* start full: a cold limiter should not throttle */
    b->last_ns = now_ns();
    return ING_OK;
}

void ing_bucket_destroy(ing_bucket *b)
{
    pthread_mutex_destroy(&b->mu);
}

/* Bring b->tokens up to date for "now", clamped to burst. Caller holds mu.
 * This is the whole trick: there is no thread ticking tokens up in the
 * background, so a bucket nobody touches costs nothing and never drifts —
 * the next caller, whenever it arrives, computes the exact elapsed refill
 * from the timestamp rather than from however many timer ticks fired. */
static void refill(ing_bucket *b, uint64_t now)
{
    double elapsed_sec = (double)(now - b->last_ns) / 1e9;
    b->tokens += elapsed_sec * b->rate;
    if (b->tokens > b->burst)
        b->tokens = b->burst;      /* the clamp: idling forever must not bank tokens */
    b->last_ns = now;
}

int ing_bucket_allow(ing_bucket *b, double cost)
{
    if (cost <= 0.0 || cost > b->burst)
        return ING_EINVAL;    /* could never be granted; don't make the caller spin */

    pthread_mutex_lock(&b->mu);
    refill(b, now_ns());

    int rc;
    if (b->tokens >= cost) {
        b->tokens -= cost;
        rc = ING_OK;
    } else {
        rc = ING_EAGAIN;
    }
    pthread_mutex_unlock(&b->mu);
    return rc;
}

uint64_t ing_bucket_retry_after_ns(ing_bucket *b, double cost)
{
    if (cost <= 0.0 || cost > b->burst)
        return UINT64_MAX;    /* never satisfiable; caller misused us */

    pthread_mutex_lock(&b->mu);
    refill(b, now_ns());

    double deficit = cost - b->tokens;
    uint64_t wait_ns = 0;
    if (deficit > 0.0)
        wait_ns = (uint64_t)(deficit / b->rate * 1e9) + 1;  /* round up, not down */
    pthread_mutex_unlock(&b->mu);
    return wait_ns;
}

/* -------------------------------------------------------------------- */
/* Sliding window counter                                                */
/* -------------------------------------------------------------------- */

int ing_window_init(ing_window *w, uint64_t window_ns, uint64_t limit)
{
    if (window_ns == 0 || limit == 0)
        return ING_EINVAL;

    if (pthread_mutex_init(&w->mu, NULL) != 0)
        return ING_ENOMEM;

    w->window_ns = window_ns;
    w->limit = limit;
    w->cur_start_ns = now_ns();
    w->cur_count = 0;
    w->prev_count = 0;
    return ING_OK;
}

void ing_window_destroy(ing_window *w)
{
    pthread_mutex_destroy(&w->mu);
}

int ing_window_allow(ing_window *w)
{
    pthread_mutex_lock(&w->mu);

    uint64_t now = now_ns();
    uint64_t elapsed = now - w->cur_start_ns;

    if (elapsed >= 2 * w->window_ns) {
        /* Both the current and previous fixed windows are stale: whatever
         * happened has fully rolled off the sliding span. */
        w->prev_count = 0;
        w->cur_count = 0;
        w->cur_start_ns = now;
        elapsed = 0;
    } else if (elapsed >= w->window_ns) {
        /* The fixed window rolled over exactly once: what was "current"
         * becomes "previous", and we start a fresh current window. */
        w->prev_count = w->cur_count;
        w->cur_count = 0;
        w->cur_start_ns += w->window_ns;
        elapsed -= w->window_ns;
    }

    /* Fraction of the previous fixed window that still overlaps the
     * trailing sliding span of width window_ns anchored at `now`. */
    double overlap = 1.0 - (double)elapsed / (double)w->window_ns;
    double weighted = (double)w->prev_count * overlap + (double)w->cur_count;

    int rc;
    if (weighted + 1.0 <= (double)w->limit + 1e-9) {
        w->cur_count++;
        rc = ING_OK;
    } else {
        rc = ING_EAGAIN;
    }

    pthread_mutex_unlock(&w->mu);
    return rc;
}
