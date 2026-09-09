#include "harness.h"
#include "zset.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

static void test_basic(void)
{
    ing_zset *z = ing_zset_create();
    ING_CHECK(z != NULL, "create failed");

    ING_CHECK(ing_zset_add(z, 1.0, "a", 1) == ING_OK, "add a failed");
    ING_CHECK(ing_zset_add(z, 2.0, "b", 1) == ING_OK, "add b failed");
    ING_CHECK(ing_zset_size(z) == 2, "size should be 2, got %zu", ing_zset_size(z));

    double score;
    ING_CHECK(ing_zset_score(z, "a", 1, &score) == ING_OK && score == 1.0,
              "score(a) wrong");
    ING_CHECK(ing_zset_score(z, "zzz", 3, &score) == ING_ENOTFOUND,
              "score of missing member should be ENOTFOUND");

    ING_CHECK(ing_zset_remove(z, "a", 1) == ING_OK, "remove a failed");
    ING_CHECK(ing_zset_remove(z, "a", 1) == ING_ENOTFOUND,
              "removing an already-removed member should be ENOTFOUND");
    ING_CHECK(ing_zset_size(z) == 1, "size should be 1 after remove, got %zu",
              ing_zset_size(z));

    ing_zset_destroy(z);
}

/* The classic bug: updating a score must MOVE the member, not duplicate it. */
static void test_update_moves(void)
{
    ing_zset *z = ing_zset_create();
    ing_zset_add(z, 1.0, "a", 1);
    ing_zset_add(z, 2.0, "b", 1);
    ing_zset_add(z, 3.0, "c", 1);
    ING_CHECK(ing_zset_size(z) == 3, "size should be 3, got %zu", ing_zset_size(z));

    ING_CHECK(ing_zset_rank(z, "a", 1) == 1, "a should start at rank 1");

    ING_CHECK(ing_zset_add(z, 99.0, "a", 1) == ING_OK, "re-add a failed");
    ING_CHECK(ing_zset_size(z) == 3,
              "size must NOT grow on a score update, got %zu", ing_zset_size(z));

    double score;
    ing_zset_score(z, "a", 1, &score);
    ING_CHECK(score == 99.0, "a's score should be 99.0, got %f", score);
    ING_CHECK(ing_zset_rank(z, "a", 1) == 3,
              "a should now be last (rank 3), got %zu", ing_zset_rank(z, "a", 1));
    ING_CHECK(ing_zset_rank(z, "b", 1) == 1, "b should now be rank 1");
    ING_CHECK(ing_zset_rank(z, "c", 1) == 2, "c should now be rank 2");

    ing_zset_destroy(z);
}

static void test_ties_lexicographic(void)
{
    ing_zset *z = ing_zset_create();
    ing_zset_add(z, 5.0, "banana", 6);
    ing_zset_add(z, 5.0, "apple", 5);
    ing_zset_add(z, 5.0, "cherry", 6);

    ING_CHECK(ing_zset_rank(z, "apple", 5) == 1, "apple should rank 1 on tie");
    ING_CHECK(ing_zset_rank(z, "banana", 6) == 2, "banana should rank 2 on tie");
    ING_CHECK(ing_zset_rank(z, "cherry", 6) == 3, "cherry should rank 3 on tie");

    ing_zset_destroy(z);
}

static void test_range(void)
{
    ing_zset *z = ing_zset_create();
    for (int i = 0; i < 10; i++) {
        char buf[16];
        int n = snprintf(buf, sizeof buf, "m%02d", i);
        ing_zset_add(z, (double)i, buf, (size_t)n);
    }

    ing_slnode *out[5];
    size_t n = ing_zset_range(z, 1, 5, out);
    ING_CHECK(n == 5, "range should return 5, got %zu", n);
    ING_CHECK(memcmp(out[0]->member, "m00", 3) == 0, "range[0] should be m00");
    ING_CHECK(memcmp(out[4]->member, "m04", 3) == 0, "range[4] should be m04");

    /* out-of-bounds start */
    n = ing_zset_range(z, 100, 5, out);
    ING_CHECK(n == 0, "out-of-bounds start_rank should return 0, got %zu", n);

    n = ing_zset_range(z, 0, 5, out);
    ING_CHECK(n == 0, "start_rank 0 should return 0, got %zu", n);

    n = ing_zset_range(z, 9, 5, out);
    ING_CHECK(n == 2, "range near the end should be clamped, got %zu", n);

    ing_zset_destroy(z);
}

static void test_range_by_score(void)
{
    ing_zset *z = ing_zset_create();
    for (int i = 0; i < 10; i++) {
        char buf[16];
        int n = snprintf(buf, sizeof buf, "m%02d", i);
        ing_zset_add(z, (double)i, buf, (size_t)n);
    }

    ing_slnode *out[10];
    size_t n = ing_zset_range_by_score(z, 3.0, 6.0, 10, out);
    ING_CHECK(n == 4, "range_by_score [3,6] should return 4, got %zu", n);
    ING_CHECK(out[0]->score == 3.0, "first should have score 3.0");
    ING_CHECK(out[3]->score == 6.0, "last should have score 6.0");

    /* empty range */
    n = ing_zset_range_by_score(z, 50.0, 60.0, 10, out);
    ING_CHECK(n == 0, "empty range should return 0, got %zu", n);

    /* limit clamps */
    n = ing_zset_range_by_score(z, 0.0, 9.0, 3, out);
    ING_CHECK(n == 3, "limit should clamp to 3, got %zu", n);

    ing_zset_destroy(z);
}

/* Linear model cross-check over a shuffled batch of inserts. */
typedef struct { char member[16]; double score; } model_entry;

static int model_cmp(const void *pa, const void *pb)
{
    const model_entry *a = pa, *b = pb;
    if (a->score < b->score) return -1;
    if (a->score > b->score) return 1;
    return strcmp(a->member, b->member);
}

static void test_rank_against_model(void)
{
    enum { N = 300 };
    static model_entry model[N];
    ing_zset *z = ing_zset_create();

    unsigned seed = 12345;
    for (int i = 0; i < N; i++) {
        snprintf(model[i].member, sizeof model[i].member, "k%04d", i);
        seed = seed * 1103515245u + 12345u;
        model[i].score = (double)(seed % 100000);
    }
    /* shuffle insertion order */
    int order[N];
    for (int i = 0; i < N; i++) order[i] = i;
    for (int i = N - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)(seed % (unsigned)(i + 1));
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }
    for (int i = 0; i < N; i++) {
        int k = order[i];
        ing_zset_add(z, model[k].score, model[k].member, strlen(model[k].member));
    }

    model_entry sorted[N];
    memcpy(sorted, model, sizeof model);
    qsort(sorted, N, sizeof sorted[0], model_cmp);

    int all_match = 1;
    for (int i = 0; i < N; i++) {
        size_t expected_rank = (size_t)i + 1;
        size_t got = ing_zset_rank(z, sorted[i].member, strlen(sorted[i].member));
        if (got != expected_rank) { all_match = 0; break; }
    }
    ING_CHECK(all_match, "rank must match the linear model for all %d entries", N);
    ING_CHECK(ing_zset_size(z) == N, "size should be %d, got %zu", N, ing_zset_size(z));

    ing_zset_destroy(z);
}

/* The cross-check that catches the two structures drifting apart. */
static void test_consistency_fuzz(void)
{
    enum { KEYSPACE = 100, OPS = 2000 };
    ing_zset *z = ing_zset_create();
    int present[KEYSPACE] = {0};

    unsigned seed = 987654321u;
    for (int op = 0; op < OPS; op++) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)(seed % KEYSPACE);
        char buf[16];
        int n = snprintf(buf, sizeof buf, "k%03d", k);

        seed = seed * 1103515245u + 12345u;
        if (seed % 3 == 0 && present[k]) {
            ing_zset_remove(z, buf, (size_t)n);
            present[k] = 0;
        } else {
            seed = seed * 1103515245u + 12345u;
            double score = (double)(seed % 1000);
            ing_zset_add(z, score, buf, (size_t)n);
            present[k] = 1;
        }
    }

    size_t expected = 0;
    for (int i = 0; i < KEYSPACE; i++) expected += (size_t)present[i];
    ING_CHECK(ing_zset_size(z) == expected,
              "size (%zu) must equal the number of present keys (%zu)",
              ing_zset_size(z), expected);

    int consistent = 1;
    for (int i = 0; i < KEYSPACE; i++) {
        if (!present[i]) continue;
        char buf[16];
        int n = snprintf(buf, sizeof buf, "k%03d", i);
        double score;
        int rc = ing_zset_score(z, buf, (size_t)n, &score);
        if (rc != ING_OK) { consistent = 0; break; }
        size_t rank = ing_zset_rank(z, buf, (size_t)n);
        if (rank == 0) { consistent = 0; break; }
        ing_slnode *out[1];
        size_t got = ing_zset_range(z, rank, 1, out);
        if (got != 1 || out[0]->mlen != (size_t)n ||
            memcmp(out[0]->member, buf, (size_t)n) != 0 || out[0]->score != score) {
            consistent = 0;
            break;
        }
    }
    ING_CHECK(consistent,
              "every present member found via the hash table must be present "
              "in the skip list at its stated rank");

    ing_zset_destroy(z);
}

int main(void)
{
    ing_test_begin();
    ING_SECTION("zset basics");
    ING_RUN(test_basic);
    ING_RUN(test_update_moves);
    ING_RUN(test_ties_lexicographic);
    ING_SECTION("zset ranges");
    ING_RUN(test_range);
    ING_RUN(test_range_by_score);
    ING_SECTION("zset consistency");
    ING_RUN(test_rank_against_model);
    ING_RUN(test_consistency_fuzz);
    return ing_test_end();
}
