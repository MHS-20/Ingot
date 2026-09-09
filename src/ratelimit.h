/* ratelimit.h — two limiters with deliberately different characters.
 *
 * ing_bucket (token bucket) allows bursts up to its size and refills
 * continuously; ing_window (sliding window counter) caps a count over a
 * fixed span and, unlike a naive fixed window, does not let a client double
 * its allowance by timing requests around a window edge.
 *
 * Both compute time lazily rather than running a background thread. A timer
 * thread would mean one thread per limiter, wakeups even while the limiter
 * sits idle, and an answer quantised to the timer's period. Reading the
 * clock at call time costs nothing when nobody calls, and is exact rather
 * than "as fresh as the last tick".
 */
#ifndef ING_RATELIMIT_H
#define ING_RATELIMIT_H

#include <pthread.h>
#include <stdint.h>

/* Token bucket: `burst` tokens capacity, refilling at `rate` tokens/sec.
 * A request of `cost` tokens is allowed iff enough have accumulated. */
typedef struct {
    pthread_mutex_t mu;
    double rate;        /* tokens per second */
    double burst;       /* bucket capacity, also the max single grant */
    double tokens;      /* tokens available as of last_ns */
    uint64_t last_ns;   /* CLOCK_MONOTONIC timestamp of last update */
} ing_bucket;

int  ing_bucket_init(ing_bucket *b, double rate_per_sec, double burst);
void ing_bucket_destroy(ing_bucket *b);

/* ING_OK if `cost` tokens were available and have been spent, ING_EAGAIN if
 * not (nothing is spent on refusal). ING_EINVAL if cost is non-positive or
 * exceeds the bucket's capacity — such a request could never succeed, so it
 * is rejected outright rather than making the caller retry forever. */
int  ing_bucket_allow(ing_bucket *b, double cost);

/* Nanoseconds until `cost` tokens would be available if no one else spends
 * any in the meantime; 0 if `cost` is available right now. Advisory only:
 * concurrent callers can still race for the tokens once the wait is over. */
uint64_t ing_bucket_retry_after_ns(ing_bucket *b, double cost);

/* Sliding window counter: at most `limit` requests in any `window_ns`-wide
 * span. Approximated with two adjacent fixed windows: the current window's
 * count plus the previous window's count weighted by how much of it still
 * overlaps the sliding span. This is the standard fix for a fixed window's
 * worst case — a client firing `limit` requests in the last instant of one
 * window and another `limit` in the first instant of the next gets 2*limit
 * through in a span barely wider than zero. Weighting the previous window
 * down smooths that spike, but it is an approximation: it assumes the
 * previous window's requests were spread evenly across it, which is not
 * always true. */
typedef struct {
    pthread_mutex_t mu;
    uint64_t window_ns;
    uint64_t limit;
    uint64_t cur_start_ns;  /* start of the current fixed window */
    uint64_t cur_count;
    uint64_t prev_count;    /* count of the fixed window just before this one */
} ing_window;

int  ing_window_init(ing_window *w, uint64_t window_ns, uint64_t limit);
void ing_window_destroy(ing_window *w);

/* ING_OK if the request fits under the weighted count, ING_EAGAIN if not. */
int  ing_window_allow(ing_window *w);

#endif /* ING_RATELIMIT_H */
