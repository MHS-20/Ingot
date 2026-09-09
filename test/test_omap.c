#include "harness.h"
#include "omap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_basic(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 4) == ING_OK, "init failed");

    ING_CHECK(ing_omap_put(&m, "alpha", 5, (void *)1) == ING_OK, "put alpha failed");
    ING_CHECK(ing_omap_put(&m, "beta", 4, (void *)2) == ING_OK, "put beta failed");
    ING_CHECK(ing_omap_size(&m) == 2, "size should be 2, got %zu", ing_omap_size(&m));

    void *out = NULL;
    ING_CHECK(ing_omap_get(&m, "alpha", 5, &out) == ING_OK, "get alpha failed");
    ING_CHECK(out == (void *)1, "alpha value wrong");

    /* overwrite */
    ING_CHECK(ing_omap_put(&m, "alpha", 5, (void *)42) == ING_OK, "overwrite failed");
    ING_CHECK(ing_omap_size(&m) == 2, "overwrite must not change size");
    ING_CHECK(ing_omap_get(&m, "alpha", 5, &out) == ING_OK && out == (void *)42,
              "overwritten value wrong");

    ING_CHECK(ing_omap_del(&m, "beta", 4) == ING_OK, "delete beta failed");
    ING_CHECK(ing_omap_size(&m) == 1, "size should be 1 after delete");
    ING_CHECK(ing_omap_get(&m, "beta", 4, &out) == ING_ENOTFOUND, "beta should be gone");

    ing_omap_free(&m);
}

static void test_missing_and_empty(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 4) == ING_OK, "init failed");
    void *out = NULL;
    ING_CHECK(ing_omap_get(&m, "x", 1, &out) == ING_ENOTFOUND, "get on empty must miss");
    ING_CHECK(ing_omap_del(&m, "x", 1) == ING_ENOTFOUND, "del on empty must miss");
    ing_omap_free(&m); /* freeing an empty table must not crash */
}

/* Force collisions by using keys whose FNV hash happens to differ, but we
 * cannot override the hash function the way the htab test overrides
 * hcode directly (omap hashes the key bytes internally). Instead this test
 * relies on a small table (nslots=4) plus enough keys that pigeonhole
 * forces multiple keys to share the same starting bucket even though the
 * chain of collisions is a byproduct of table size rather than an explicit
 * override — which exercises linear probing over a run of occupied slots
 * the same way an explicit collision would. */
static void test_collisions(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 4) == ING_OK, "init failed");

    const char *keys[6] = {"k0", "k1", "k2", "k3", "k4", "k5"};
    for (int i = 0; i < 6; i++)
        ING_CHECK(ing_omap_put(&m, keys[i], 2, (void *)(intptr_t)(i + 1)) == ING_OK,
                  "put %s failed", keys[i]);

    for (int i = 0; i < 6; i++) {
        void *out = NULL;
        ING_CHECK(ing_omap_get(&m, keys[i], 2, &out) == ING_OK, "get %s missed", keys[i]);
        ING_CHECK(out == (void *)(intptr_t)(i + 1), "get %s wrong value", keys[i]);
    }

    ing_omap_free(&m);
}

static void test_grow_preserves_elements(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 8) == ING_OK, "init failed");

    enum { N = 10000 };
    for (int i = 0; i < N; i++) {
        char key[32];
        int klen = snprintf(key, sizeof key, "key-%d", i);
        ING_CHECK(ing_omap_put(&m, key, (size_t)klen, (void *)(intptr_t)i) == ING_OK,
                  "put failed at i=%d", i);
    }
    ING_CHECK(ing_omap_size(&m) == N, "size should be %d, got %zu", N, ing_omap_size(&m));

    int all_found = 1;
    for (int i = 0; i < N; i++) {
        char key[32];
        int klen = snprintf(key, sizeof key, "key-%d", i);
        void *out = NULL;
        if (ing_omap_get(&m, key, (size_t)klen, &out) != ING_OK ||
            out != (void *)(intptr_t)i)
            all_found = 0;
    }
    ING_CHECK(all_found, "every inserted key must survive incremental migration");

    ing_omap_free(&m);
}

static void test_delete_then_reinsert(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 4) == ING_OK, "init failed");

    ING_CHECK(ing_omap_put(&m, "gamma", 5, (void *)1) == ING_OK, "put failed");
    ING_CHECK(ing_omap_del(&m, "gamma", 5) == ING_OK, "delete failed");
    ING_CHECK(ing_omap_size(&m) == 0, "size should be 0 after delete");

    ING_CHECK(ing_omap_put(&m, "gamma", 5, (void *)2) == ING_OK, "reinsert failed");
    void *out = NULL;
    ING_CHECK(ing_omap_get(&m, "gamma", 5, &out) == ING_OK && out == (void *)2,
              "reinsert value wrong");
    ING_CHECK(ing_omap_size(&m) == 1, "size should be 1 after reinsert");

    ing_omap_free(&m);
}

/* Delete and reinsert enough keys, in a small fixed-size table (no
 * migration triggered), that tombstones must be reused rather than the
 * table silently having grown out from under the test. Verifies both that
 * gets past a tombstone still work and that the table doesn't run out of
 * slots. */
static void test_tombstone_reclamation(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 4) == ING_OK, "init failed");

    for (int round = 0; round < 50; round++) {
        char key[16];
        snprintf(key, sizeof key, "r%d", round % 3);
        ING_CHECK(ing_omap_put(&m, key, strlen(key), (void *)(intptr_t)round) == ING_OK,
                  "put failed round %d", round);
        ING_CHECK(ing_omap_del(&m, key, strlen(key)) == ING_OK,
                  "delete failed round %d", round);
    }
    ING_CHECK(ing_omap_size(&m) == 0, "size should be 0 after all rounds");

    ING_CHECK(ing_omap_put(&m, "r0", 2, (void *)999) == ING_OK, "final put failed");
    void *out = NULL;
    ING_CHECK(ing_omap_get(&m, "r0", 2, &out) == ING_OK && out == (void *)999,
              "final get wrong after tombstone churn");

    ing_omap_free(&m);
}

/* Puts enough keys to force a migration, then interleaves gets with the
 * remaining puts that drive the migration forward, checking on every
 * iteration that every key inserted so far is still visible. Because
 * ing_omap_get itself calls migrate_step, this exercises "lookups must
 * consult both tables while a migration is in flight" directly rather
 * than just checking the end state. */
static void test_migration_interleaved(void)
{
    ing_omap m;
    ING_CHECK(ing_omap_init(&m, 8) == ING_OK, "init failed");

    enum { N = 2000 };
    for (int i = 0; i < N; i++) {
        char key[32];
        int klen = snprintf(key, sizeof key, "mig-%d", i);
        ING_CHECK(ing_omap_put(&m, key, (size_t)klen, (void *)(intptr_t)i) == ING_OK,
                  "put failed at i=%d", i);

        /* Re-check a slice of already-inserted keys on every iteration —
         * this runs both before, during and after the migration this
         * loop's own puts trigger. */
        int start = i - 20 < 0 ? 0 : i - 20;
        for (int j = start; j <= i; j++) {
            char kk[32];
            int kl = snprintf(kk, sizeof kk, "mig-%d", j);
            void *out = NULL;
            int rc = ing_omap_get(&m, kk, (size_t)kl, &out);
            ING_CHECK(rc == ING_OK && out == (void *)(intptr_t)j,
                      "key mig-%d lost during migration at i=%d", j, i);
        }
    }

    ing_omap_free(&m);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("basic put/get/del, overwrite");
    ING_RUN(test_basic);

    ING_SECTION("missing keys and empty table");
    ING_RUN(test_missing_and_empty);

    ING_SECTION("collisions via small table + many keys");
    ING_RUN(test_collisions);

    ING_SECTION("growth preserves every element");
    ING_RUN(test_grow_preserves_elements);

    ING_SECTION("delete then reinsert");
    ING_RUN(test_delete_then_reinsert);

    ING_SECTION("tombstone reclamation");
    ING_RUN(test_tombstone_reclamation);

    ING_SECTION("interleaved gets during incremental migration");
    ING_RUN(test_migration_interleaved);

    return ing_test_end();
}
