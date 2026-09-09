/* test_ratelimit.c — token bucket and sliding window.
 *
 * Timing tests are inherently flaky under load, so every one of them
 * asserts a *bound* ("no more than N", "at least N") with generous
 * tolerance rather than an exact count, and sleeps are kept short so the
 * whole suite stays well under the harness's 10s per-group timeout.
 */
#include "../src/ratelimit.h"
#include "../src/common.h"
#include "harness.h"

#include <pthread.h>
#include <time.h>
#include <stdint.h>

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------- bucket ------------------------------- */

static void test_bucket_burst_then_refuse(void)
{
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 10.0, 5.0) == ING_OK, "init");

    int allowed = 0;
    for (int i = 0; i < 5; i++)
        if (ing_bucket_allow(&b, 1.0) == ING_OK)
            allowed++;
    ING_CHECK(allowed == 5, "expected exactly burst=5 allowed, got %d", allowed);

    ING_CHECK(ing_bucket_allow(&b, 1.0) == ING_EAGAIN, "bucket should be empty now");

    ing_bucket_destroy(&b);
}

static void test_bucket_refill_over_time(void)
{
    /* rate=100/s, burst=5: drain fully, sleep 50ms (~5 tokens worth), expect
     * roughly 5 more available, with a generous tolerance either side. */
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 100.0, 5.0) == ING_OK, "init");
    for (int i = 0; i < 5; i++)
        ing_bucket_allow(&b, 1.0);

    sleep_ms(50);

    int allowed = 0;
    for (int i = 0; i < 10; i++)
        if (ing_bucket_allow(&b, 1.0) == ING_OK)
            allowed++;
    ING_CHECK(allowed >= 2 && allowed <= 8,
              "expected ~5 tokens refilled after 50ms at 100/s, got %d", allowed);

    ing_bucket_destroy(&b);
}

static void test_bucket_never_exceeds_burst(void)
{
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 1000.0, 3.0) == ING_OK, "init");

    sleep_ms(100);   /* idle far longer than it takes to refill fully */

    int allowed = 0;
    for (int i = 0; i < 100; i++)
        if (ing_bucket_allow(&b, 1.0) == ING_OK)
            allowed++;
    ING_CHECK(allowed == 3, "burst=3 must clamp the bank, got %d allowed", allowed);

    ing_bucket_destroy(&b);
}

static void test_bucket_cost_exceeding_burst(void)
{
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 10.0, 5.0) == ING_OK, "init");

    /* A cost above capacity can never be granted; must be rejected outright
     * rather than leaving the caller to retry forever. */
    ING_CHECK(ing_bucket_allow(&b, 6.0) == ING_EINVAL, "cost > burst must be EINVAL");
    ING_CHECK(ing_bucket_retry_after_ns(&b, 6.0) == UINT64_MAX,
              "retry_after on impossible cost must signal never");

    ing_bucket_destroy(&b);
}

static void test_bucket_retry_after(void)
{
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 10.0, 2.0) == ING_OK, "init");

    ING_CHECK(ing_bucket_retry_after_ns(&b, 1.0) == 0, "full bucket: retry_after is 0");

    ing_bucket_allow(&b, 2.0);   /* drain fully */
    uint64_t wait_ns = ing_bucket_retry_after_ns(&b, 1.0);
    /* at 10 tokens/s, 1 token takes ~100ms; allow a wide band. */
    ING_CHECK(wait_ns > 0 && wait_ns < 500000000ull,
              "expected a sane positive wait, got %llu ns", (unsigned long long)wait_ns);

    ing_bucket_destroy(&b);
}

static void test_bucket_einval_args(void)
{
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 0.0, 5.0) == ING_EINVAL, "rate<=0 rejected");
    ING_CHECK(ing_bucket_init(&b, 10.0, 0.0) == ING_EINVAL, "burst<=0 rejected");
    ING_CHECK(ing_bucket_init(&b, -1.0, -1.0) == ING_EINVAL, "negative args rejected");

    ING_SKIP_UNLESS(ing_bucket_init(&b, 10.0, 5.0) == ING_OK, "init must work first");
    ING_CHECK(ing_bucket_allow(&b, 0.0) == ING_EINVAL, "cost==0 rejected");
    ING_CHECK(ing_bucket_allow(&b, -3.0) == ING_EINVAL, "negative cost rejected");
    ing_bucket_destroy(&b);
}

static void test_bucket_sustained_rate(void)
{
    /* Over ~300ms at 50 tokens/s with a small burst, the number allowed
     * should land near rate*0.3 = 15, plus the initial burst. Poll fast
     * enough not to be limited by loop overhead, and use a wide band since
     * this depends on scheduler timing. */
    ing_bucket b;
    ING_CHECK(ing_bucket_init(&b, 50.0, 2.0) == ING_OK, "init");

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int allowed = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= 0.3)
            break;
        if (ing_bucket_allow(&b, 1.0) == ING_OK)
            allowed++;
    }

    int expected = (int)(50.0 * 0.3) + 2;  /* + initial burst */
    ING_CHECK(allowed >= expected - 12 && allowed <= expected + 12,
              "expected ~%d allowed over 300ms, got %d", expected, allowed);

    ing_bucket_destroy(&b);
}

typedef struct {
    ing_bucket *b;
    long ms;
    int allowed;
} thread_arg;

static void *hammer(void *arg)
{
    thread_arg *ta = arg;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int allowed = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_ms = (now.tv_sec - start.tv_sec) * 1000.0
                           + (now.tv_nsec - start.tv_nsec) / 1e6;
        if (elapsed_ms >= (double)ta->ms)
            break;
        if (ing_bucket_allow(ta->b, 1.0) == ING_OK)
            allowed++;
    }
    ta->allowed = allowed;
    return NULL;
}

static void test_bucket_thread_safety(void)
{
    /* This is the test that catches a missing lock: without one, concurrent
     * refill+spend can double-spend tokens or drive the count negative,
     * and the total allowed sails past what rate and burst allow. */
    ing_bucket b;
    double rate = 200.0, burst = 4.0;
    ING_CHECK(ing_bucket_init(&b, rate, burst) == ING_OK, "init");

    enum { N = 8 };
    pthread_t th[N];
    thread_arg args[N];
    long run_ms = 150;

    for (int i = 0; i < N; i++) {
        args[i].b = &b;
        args[i].ms = run_ms;
        args[i].allowed = 0;
        pthread_create(&th[i], NULL, hammer, &args[i]);
    }

    long long total = 0;
    for (int i = 0; i < N; i++) {
        pthread_join(th[i], NULL);
        total += args[i].allowed;
    }

    double elapsed_sec = (double)run_ms / 1000.0 * 1.5;  /* generous slack for scheduling */
    double max_allowed = burst + rate * elapsed_sec;
    ING_CHECK((double)total <= max_allowed,
              "total allowed %lld exceeds burst+rate*elapsed bound %.1f (lock missing?)",
              total, max_allowed);

    ing_bucket_destroy(&b);
}

/* ------------------------------- window ------------------------------- */

static void test_window_limit_then_refuse(void)
{
    ing_window w;
    ING_CHECK(ing_window_init(&w, 200000000ull /* 200ms */, 5) == ING_OK, "init");

    int allowed = 0;
    for (int i = 0; i < 5; i++)
        if (ing_window_allow(&w) == ING_OK)
            allowed++;
    ING_CHECK(allowed == 5, "expected exactly limit=5 allowed, got %d", allowed);
    ING_CHECK(ing_window_allow(&w) == ING_EAGAIN, "6th request in-window must refuse");

    ing_window_destroy(&w);
}

static void test_window_boundary_no_double_burst(void)
{
    /* The whole point of the sliding approximation: firing `limit` requests
     * right before a fixed-window boundary and `limit` more right after
     * must not let 2*limit through inside one window-width span. */
    uint64_t window_ns = 200000000ull;   /* 200ms */
    uint64_t limit = 5;
    ing_window w;
    ING_CHECK(ing_window_init(&w, window_ns, limit) == ING_OK, "init");

    /* Spend the limit near the end of the first fixed window. */
    sleep_ms(150);
    int first_batch = 0;
    for (int i = 0; i < (int)limit; i++)
        if (ing_window_allow(&w) == ING_OK)
            first_batch++;
    ING_CHECK(first_batch == (int)limit, "first batch should fill the limit");

    /* Cross into the next fixed window and immediately try to spend the
     * limit again. With a naive fixed window this would succeed in full,
     * granting 2*limit within ~a few ms; the sliding weight must knock most
     * of it down. */
    sleep_ms(60);
    int second_batch = 0;
    for (int i = 0; i < (int)limit; i++)
        if (ing_window_allow(&w) == ING_OK)
            second_batch++;

    ING_CHECK(first_batch + second_batch < 2 * (int)limit,
              "sliding window let %d through near the boundary (>= 2*limit)",
              first_batch + second_batch);

    ing_window_destroy(&w);
}

static void test_window_einval_args(void)
{
    ing_window w;
    ING_CHECK(ing_window_init(&w, 0, 5) == ING_EINVAL, "window_ns==0 rejected");
    ING_CHECK(ing_window_init(&w, 1000, 0) == ING_EINVAL, "limit==0 rejected");
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("token bucket");
    ING_RUN(test_bucket_burst_then_refuse);
    ING_RUN(test_bucket_refill_over_time);
    ING_RUN(test_bucket_never_exceeds_burst);
    ING_RUN(test_bucket_cost_exceeding_burst);
    ING_RUN(test_bucket_retry_after);
    ING_RUN(test_bucket_einval_args);
    ING_RUN(test_bucket_sustained_rate);
    ING_RUN(test_bucket_thread_safety);

    ING_SECTION("sliding window");
    ING_RUN(test_window_limit_then_refuse);
    ING_RUN(test_window_boundary_no_double_burst);
    ING_RUN(test_window_einval_args);

    return ing_test_end();
}
