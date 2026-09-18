#include "harness.h"
#include "heap.h"
#include "common.h"

#include <stdint.h>
#include <stdlib.h>

/* The heap property, checked against the layout the header pins down rather
 * than against the order pops come out in. Asserting only that pops ascend
 * would pass for an insertion-sorted array, which is a priority queue and is
 * not a heap — the structure has to be checked directly or it is not being
 * checked at all. */
static int heap_property_ok(const ing_heap *h)
{
    for (size_t i = 1; i < h->len; i++)
        if (h->items[(i - 1) / 2].key > h->items[i].key)
            return 0;
    return 1;
}

/* A deterministic PRNG, so a failing stress run is reproducible. rand() is
 * not used because its sequence is implementation-defined and a bug that
 * only reproduces on one libc is not worth the debugging session. */
static uint64_t t_state = 0x9E3779B97F4A7C15ull;

static uint32_t t_rand(void)
{
    t_state ^= t_state << 13;
    t_state ^= t_state >> 7;
    t_state ^= t_state << 17;
    return (uint32_t)(t_state >> 32);
}

static void test_init_and_empty(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 8) == ING_OK, "init failed");
    ING_CHECK(ing_heap_size(&h) == 0, "a fresh heap is empty, got %zu", ing_heap_size(&h));

    ing_heap_item it = { .key = 999.0, .val = (void *)0xDEAD };
    ING_CHECK(ing_heap_peek(&h, &it) == ING_ENOTFOUND, "peek on empty must be ING_ENOTFOUND");
    ING_CHECK(ing_heap_pop(&h, &it) == ING_ENOTFOUND, "pop on empty must be ING_ENOTFOUND");
    ING_CHECK(it.key == 999.0 && it.val == (void *)0xDEAD,
              "a failed peek or pop must leave *out untouched");

    ing_heap_free(&h);
}

static void test_init_zero_capacity(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 0) == ING_OK, "capacity 0 must be legal");
    ING_CHECK(ing_heap_size(&h) == 0, "a zero-capacity heap starts empty");
    ING_CHECK(ing_heap_push(&h, 1.0, NULL) == ING_OK,
              "a zero-capacity heap must allocate on the first push");
    ING_CHECK(ing_heap_size(&h) == 1, "size must be 1 after one push");
    ing_heap_free(&h);
}

static void test_push_pop_ascending(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 4) == ING_OK, "init failed");

    /* Deliberately not sorted, and the minimum is neither first nor last. */
    const double keys[] = { 5.0, 3.0, 8.0, 1.0, 9.0, 2.0, 7.0, 4.0, 6.0 };
    const int n = (int)(sizeof keys / sizeof keys[0]);

    int push_ok = 1;
    for (int i = 0; i < n; i++)
        if (ing_heap_push(&h, keys[i], (void *)(intptr_t)i) != ING_OK)
            push_ok = 0;

    ING_CHECK(push_ok, "every push into a growing heap must succeed");
    ING_CHECK(ing_heap_size(&h) == (size_t)n, "size must be %d, got %zu", n, ing_heap_size(&h));
    ING_CHECK(heap_property_ok(&h), "the heap property must hold after a run of pushes");

    int ascending = 1, sizes_ok = 1;
    double prev = -1.0;
    for (int i = 0; i < n; i++) {
        ing_heap_item it;
        if (ing_heap_pop(&h, &it) != ING_OK) { ascending = 0; break; }
        if (it.key < prev) ascending = 0;
        prev = it.key;
        if (ing_heap_size(&h) != (size_t)(n - i - 1)) sizes_ok = 0;
        if (!heap_property_ok(&h)) ascending = 0;
    }

    ING_CHECK(ascending, "pops must yield ascending keys and preserve the heap property");
    ING_CHECK(sizes_ok, "size must drop by exactly one per pop");
    ING_CHECK(ing_heap_size(&h) == 0, "the heap must be empty after popping everything");

    ing_heap_free(&h);
}

static void test_peek_is_nondestructive(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 4) == ING_OK, "init failed");

    const double keys[] = { 4.0, 2.0, 7.0, 1.0 };
    for (int i = 0; i < 4; i++)
        ing_heap_push(&h, keys[i], (void *)(intptr_t)i);

    ing_heap_item a, b;
    ING_CHECK(ing_heap_peek(&h, &a) == ING_OK && ing_heap_peek(&h, &b) == ING_OK,
              "peek on a non-empty heap must succeed");
    ING_CHECK(a.key == 1.0, "peek must return the minimum, got %f", a.key);
    ING_CHECK(a.key == b.key && a.val == b.val, "two peeks must agree");
    ING_CHECK(ing_heap_size(&h) == 4, "peek must not remove anything, size is %zu",
              ing_heap_size(&h));

    ing_heap_item popped;
    ING_CHECK(ing_heap_pop(&h, &popped) == ING_OK && popped.key == a.key &&
              popped.val == a.val, "pop must return what peek just showed");

    ing_heap_free(&h);
}

/* Unlike the skip list, which rejects a duplicate (score, member) pair. */
static void test_duplicate_keys_allowed(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 2) == ING_OK, "init failed");

    for (int i = 0; i < 6; i++)
        ING_CHECK(ing_heap_push(&h, 5.0, (void *)(intptr_t)i) == ING_OK,
                  "a duplicate key must not be rejected");

    ING_CHECK(ing_heap_size(&h) == 6, "all six equal keys must be stored, got %zu",
              ing_heap_size(&h));

    int count = 0, all_five = 1;
    ing_heap_item it;
    while (ing_heap_pop(&h, &it) == ING_OK) {
        if (it.key != 5.0) all_five = 0;
        count++;
    }
    ING_CHECK(count == 6 && all_five, "all six must come back out, got %d", count);

    ing_heap_free(&h);
}

static void test_grows_past_initial_capacity(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 0) == ING_OK, "init with capacity 0 must succeed");

    const int n = 1000;
    int push_ok = 1;
    for (int i = 0; i < n; i++)
        if (ing_heap_push(&h, (double)(t_rand() % 10000), NULL) != ING_OK)
            push_ok = 0;

    ING_CHECK(push_ok, "growth must not fail");
    ING_CHECK(ing_heap_size(&h) == (size_t)n, "size must be %d, got %zu", n, ing_heap_size(&h));
    ING_CHECK(heap_property_ok(&h), "the heap property must survive every reallocation");

    ing_heap_free(&h);
}

static void test_build_heapifies(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 0) == ING_OK, "init failed");

    ing_heap_item src[64];
    for (int i = 0; i < 64; i++) {
        src[i].key = (double)(t_rand() % 500);
        src[i].val = (void *)(intptr_t)i;
    }

    ING_CHECK(ing_heap_build(&h, src, 64) == ING_OK, "build failed");
    ING_CHECK(ing_heap_size(&h) == 64, "build must set the size to n, got %zu",
              ing_heap_size(&h));
    ING_CHECK(heap_property_ok(&h), "build must leave a valid heap");

    int ascending = 1;
    double prev = -1.0;
    for (int i = 0; i < 64; i++) {
        ing_heap_item it;
        if (ing_heap_pop(&h, &it) != ING_OK || it.key < prev) { ascending = 0; break; }
        prev = it.key;
    }
    ING_CHECK(ascending, "a built heap must pop in ascending order");

    ing_heap_free(&h);
}

/* n = 0, 1 and 2 are where the index arithmetic in build goes off the end:
 * the last internal node is at n/2 - 1, which underflows for n < 2. */
static void test_build_degenerate_sizes(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 0) == ING_OK, "init failed");

    ing_heap_item src[2] = { { .key = 9.0, .val = NULL }, { .key = 4.0, .val = NULL } };

    ING_CHECK(ing_heap_build(&h, src, 0) == ING_OK, "build of 0 items must succeed");
    ING_CHECK(ing_heap_size(&h) == 0, "build of 0 items must leave an empty heap");

    ING_CHECK(ing_heap_build(&h, src, 1) == ING_OK, "build of 1 item must succeed");
    ING_CHECK(ing_heap_size(&h) == 1 && heap_property_ok(&h), "build of 1 item must be valid");

    ING_CHECK(ing_heap_build(&h, src, 2) == ING_OK, "build of 2 items must succeed");
    ING_CHECK(ing_heap_size(&h) == 2 && heap_property_ok(&h),
              "build of 2 items must sift the root");

    ing_heap_item it;
    ING_CHECK(ing_heap_peek(&h, &it) == ING_OK && it.key == 4.0,
              "the smaller of two built items must be at the root, got %f", it.key);

    ing_heap_free(&h);
}

/* build replaces, it does not append — and a build smaller than what was
 * there before must not let the stale tail back into the heap. */
static void test_build_replaces_contents(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 0) == ING_OK, "init failed");

    for (int i = 0; i < 20; i++)
        ing_heap_push(&h, (double)i, NULL);

    ing_heap_item src[3] = { { .key = 100.0, .val = NULL },
                             { .key = 50.0,  .val = NULL },
                             { .key = 75.0,  .val = NULL } };

    ING_CHECK(ing_heap_build(&h, src, 3) == ING_OK, "build failed");
    ING_CHECK(ing_heap_size(&h) == 3, "build must replace, not append: size is %zu",
              ing_heap_size(&h));
    ING_CHECK(heap_property_ok(&h), "the heap property must hold after a replacing build");

    ing_heap_item it;
    ING_CHECK(ing_heap_peek(&h, &it) == ING_OK && it.key == 50.0,
              "the minimum must come from the new contents, got %f", it.key);

    ing_heap_free(&h);
}

/* Against a sorted model, over a mixed run, because the failure mode of a
 * wrong sift is a heap that stays plausible for hundreds of operations and
 * then returns the second-smallest element once. */
static void test_fuzz_against_sorted_model(void)
{
    ing_heap h;
    ING_CHECK(ing_heap_init(&h, 4) == ING_OK, "init failed");

    enum { MAX = 512 };
    static double model[MAX];
    size_t model_len = 0;
    int ok = 1;

    for (int op = 0; op < 4000 && ok; op++) {
        if (model_len == 0 || (model_len < MAX && t_rand() % 100 < 60)) {
            double key = (double)(t_rand() % 1000);
            if (ing_heap_push(&h, key, NULL) != ING_OK) { ok = 0; break; }

            size_t pos = model_len;
            while (pos > 0 && model[pos - 1] > key) {
                model[pos] = model[pos - 1];
                pos--;
            }
            model[pos] = key;
            model_len++;
        } else {
            ing_heap_item it;
            if (ing_heap_pop(&h, &it) != ING_OK) { ok = 0; break; }
            if (it.key != model[0]) { ok = 0; break; }

            memmove(model, model + 1, (model_len - 1) * sizeof *model);
            model_len--;
        }

        if (ing_heap_size(&h) != model_len || !heap_property_ok(&h)) { ok = 0; break; }
    }

    ING_CHECK(ok, "every pop must return the model's minimum and the heap property "
                  "must hold after all 4000 operations");

    ing_heap_free(&h);
}

int main(void)
{
    ing_test_begin();
    ING_SECTION("heap basics");
    ING_RUN(test_init_and_empty);
    ING_RUN(test_init_zero_capacity);
    ING_RUN(test_push_pop_ascending);
    ING_RUN(test_peek_is_nondestructive);
    ING_RUN(test_duplicate_keys_allowed);
    ING_RUN(test_grows_past_initial_capacity);
    ING_SECTION("heap build");
    ING_RUN(test_build_heapifies);
    ING_RUN(test_build_degenerate_sizes);
    ING_RUN(test_build_replaces_contents);
    ING_SECTION("heap fuzz");
    ING_RUN(test_fuzz_against_sorted_model);
    return ing_test_end();
}
