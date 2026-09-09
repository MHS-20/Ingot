#include "harness.h"
#include "lru.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

typedef struct { char key[16]; size_t klen; void *value; int fired; } evict_record;

typedef struct {
    evict_record records[64];
    int count;
} evict_log;

static void log_evict(const void *key, size_t klen, void *value, void *ctx)
{
    evict_log *log = ctx;
    evict_record *r = &log->records[log->count++];
    memcpy(r->key, key, klen);
    r->klen = klen;
    r->value = value;
    r->fired = 1;
}

static void test_basic(void)
{
    ing_lru *c = ing_lru_create(4, NULL, NULL);
    ING_CHECK(c != NULL, "create failed");
    ING_CHECK(ing_lru_capacity(c) == 4, "capacity should be 4");

    ING_CHECK(ing_lru_put(c, "a", 1, (void *)(intptr_t)1) == ING_OK, "put a failed");
    ING_CHECK(ing_lru_size(c) == 1, "size should be 1, got %zu", ing_lru_size(c));

    void *out;
    ING_CHECK(ing_lru_get(c, "a", 1, &out) == ING_OK && (intptr_t)out == 1,
              "get a wrong");
    ING_CHECK(ing_lru_get(c, "missing", 7, &out) == ING_ENOTFOUND,
              "missing key should be ENOTFOUND");

    ing_lru_destroy(c);
}

static void test_zero_capacity(void)
{
    ing_lru *c = ing_lru_create(0, NULL, NULL);
    ING_CHECK(c == NULL, "capacity 0 must be rejected");
}

/* The test that distinguishes an LRU from a FIFO. */
static void test_eviction_is_by_recency(void)
{
    evict_log log = {0};
    ing_lru *c = ing_lru_create(3, log_evict, &log);

    ing_lru_put(c, "a", 1, (void *)(intptr_t)1);
    ing_lru_put(c, "b", 1, (void *)(intptr_t)2);
    ing_lru_put(c, "c", 1, (void *)(intptr_t)3);

    void *out;
    ING_CHECK(ing_lru_get(c, "a", 1, &out) == ING_OK, "get a failed");
    /* recency order is now: a (front), c, b (back/LRU) */

    ing_lru_put(c, "d", 1, (void *)(intptr_t)4);
    /* b was least recently used and must be the one evicted, not a. */

    ING_CHECK(log.count == 1, "exactly one eviction expected, got %d", log.count);
    ING_CHECK(log.count == 1 && log.records[0].klen == 1 && log.records[0].key[0] == 'b',
              "b should have been evicted, not a FIFO victim");

    ING_CHECK(ing_lru_peek(c, "a", 1, &out) == ING_OK, "a should have survived");
    ING_CHECK(ing_lru_peek(c, "b", 1, &out) == ING_ENOTFOUND, "b should be gone");
    ING_CHECK(ing_lru_peek(c, "c", 1, &out) == ING_OK, "c should have survived");
    ING_CHECK(ing_lru_peek(c, "d", 1, &out) == ING_OK, "d should be present");
    ING_CHECK(ing_lru_size(c) == 3, "size should stay at capacity, got %zu", ing_lru_size(c));

    ing_lru_destroy(c);
}

static void test_evict_callback_fires_once(void)
{
    evict_log log = {0};
    ing_lru *c = ing_lru_create(2, log_evict, &log);

    ing_lru_put(c, "a", 1, (void *)(intptr_t)100);
    ing_lru_put(c, "b", 1, (void *)(intptr_t)200);
    ing_lru_put(c, "c", 1, (void *)(intptr_t)300);  /* evicts a */

    ING_CHECK(log.count == 1, "expected 1 eviction, got %d", log.count);
    ING_CHECK(log.records[0].key[0] == 'a' && (intptr_t)log.records[0].value == 100,
              "eviction record should be (a, 100)");

    ing_lru_remove(c, "b", 1);
    ING_CHECK(log.count == 2, "remove should also fire the callback, got %d", log.count);
    ING_CHECK(log.records[1].key[0] == 'b' && (intptr_t)log.records[1].value == 200,
              "removal record should be (b, 200)");

    ing_lru_destroy(c);
    ING_CHECK(log.count == 3, "destroy should fire the callback for what's left, got %d",
              log.count);
    ING_CHECK(log.records[2].key[0] == 'c', "destroy should have evicted c");
}

static void test_put_existing_key_updates_in_place(void)
{
    evict_log log = {0};
    ing_lru *c = ing_lru_create(3, log_evict, &log);

    ing_lru_put(c, "a", 1, (void *)(intptr_t)1);
    ing_lru_put(c, "b", 1, (void *)(intptr_t)2);
    ING_CHECK(ing_lru_size(c) == 2, "size should be 2, got %zu", ing_lru_size(c));

    ing_lru_put(c, "a", 1, (void *)(intptr_t)99);
    ING_CHECK(ing_lru_size(c) == 2, "size must not grow on update, got %zu", ing_lru_size(c));
    ING_CHECK(log.count == 1, "old value should fire on_evict exactly once, got %d", log.count);
    ING_CHECK((intptr_t)log.records[0].value == 1, "the old value (1) should have been evicted");

    void *out;
    ing_lru_peek(c, "a", 1, &out);
    ING_CHECK((intptr_t)out == 99, "a's value should now be 99");

    /* updating "a" should have refreshed its recency to the front, so a
     * put of a third key evicts b, not a. */
    ing_lru_put(c, "c", 1, (void *)(intptr_t)3);
    ing_lru_put(c, "d", 1, (void *)(intptr_t)4);  /* cache full at 3: a, c, then this evicts b */

    ING_CHECK(ing_lru_peek(c, "b", 1, &out) == ING_ENOTFOUND,
              "b should have been evicted, not a");
    ING_CHECK(ing_lru_peek(c, "a", 1, &out) == ING_OK, "a should have survived");

    ing_lru_destroy(c);
}

static void test_peek_does_not_refresh(void)
{
    ing_lru *c = ing_lru_create(2, NULL, NULL);
    ing_lru_put(c, "a", 1, (void *)(intptr_t)1);
    ing_lru_put(c, "b", 1, (void *)(intptr_t)2);

    void *out;
    ING_CHECK(ing_lru_peek(c, "a", 1, &out) == ING_OK, "peek a failed");

    /* a is still least-recently-used despite the peek; putting c should
     * evict a, not b. */
    ing_lru_put(c, "c", 1, (void *)(intptr_t)3);

    ING_CHECK(ing_lru_peek(c, "a", 1, &out) == ING_ENOTFOUND,
              "peek must not have refreshed a's recency");
    ING_CHECK(ing_lru_peek(c, "b", 1, &out) == ING_OK, "b should have survived");

    ing_lru_destroy(c);
}

static void test_remove(void)
{
    ing_lru *c = ing_lru_create(3, NULL, NULL);
    ing_lru_put(c, "a", 1, NULL);
    ING_CHECK(ing_lru_remove(c, "a", 1) == ING_OK, "remove a failed");
    ING_CHECK(ing_lru_remove(c, "a", 1) == ING_ENOTFOUND, "double remove should be ENOTFOUND");
    ING_CHECK(ing_lru_size(c) == 0, "size should be 0, got %zu", ing_lru_size(c));
    ing_lru_destroy(c);
}

static void test_capacity_one(void)
{
    evict_log log = {0};
    ing_lru *c = ing_lru_create(1, log_evict, &log);

    ing_lru_put(c, "a", 1, (void *)(intptr_t)1);
    ing_lru_put(c, "b", 1, (void *)(intptr_t)2);
    ING_CHECK(ing_lru_size(c) == 1, "size should stay 1, got %zu", ing_lru_size(c));
    ING_CHECK(log.count == 1 && log.records[0].key[0] == 'a', "a should have been evicted");

    void *out;
    ING_CHECK(ing_lru_get(c, "b", 1, &out) == ING_OK, "b should be present");
    ing_lru_destroy(c);
}

static void test_destroy_fires_for_remaining(void)
{
    evict_log log = {0};
    ing_lru *c = ing_lru_create(3, log_evict, &log);
    ing_lru_put(c, "a", 1, (void *)(intptr_t)1);
    ing_lru_put(c, "b", 1, (void *)(intptr_t)2);
    ing_lru_put(c, "c", 1, (void *)(intptr_t)3);
    ing_lru_destroy(c);
    ING_CHECK(log.count == 3, "destroy should fire once per remaining entry, got %d", log.count);
}

static void test_stress_size_bound(void)
{
    enum { CAP = 16, OPS = 5000 };
    ing_lru *c = ing_lru_create(CAP, NULL, NULL);

    unsigned seed = 424242u;
    int max_size_ok = 1;
    for (int op = 0; op < OPS; op++) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)(seed % 200);
        char buf[16];
        int n = snprintf(buf, sizeof buf, "k%03d", k);

        seed = seed * 1103515245u + 12345u;
        unsigned action = seed % 4;
        if (action == 0) {
            ing_lru_remove(c, buf, (size_t)n);
        } else {
            ing_lru_put(c, buf, (size_t)n, (void *)(intptr_t)op);
        }
        if (ing_lru_size(c) > CAP) { max_size_ok = 0; break; }
    }
    ING_CHECK(max_size_ok, "size must never exceed capacity during the stress loop");
    ING_CHECK(ing_lru_size(c) <= CAP, "final size must be within capacity");

    ing_lru_destroy(c);
}

int main(void)
{
    ing_test_begin();
    ING_SECTION("lru basics");
    ING_RUN(test_basic);
    ING_RUN(test_zero_capacity);
    ING_RUN(test_remove);
    ING_RUN(test_capacity_one);
    ING_SECTION("lru recency and eviction");
    ING_RUN(test_eviction_is_by_recency);
    ING_RUN(test_evict_callback_fires_once);
    ING_RUN(test_put_existing_key_updates_in_place);
    ING_RUN(test_peek_does_not_refresh);
    ING_RUN(test_destroy_fires_for_remaining);
    ING_SECTION("lru stress");
    ING_RUN(test_stress_size_bound);
    return ing_test_end();
}
