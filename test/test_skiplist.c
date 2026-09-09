/* test_skiplist.c — exercises skiplist.c against the promises in its header:
 * ordering by (score, member), O(log n) rank via span, and correct
 * backward/tail bookkeeping. Span maintenance on delete is the likeliest
 * place for a subtle bug, so ranks are re-checked after deletions, not just
 * after inserts.
 */
#include "../src/skiplist.h"
#include "../src/common.h"
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mk(char *buf, size_t n, unsigned v)
{
    snprintf(buf, n, "m%u", v);
}

static void test_basic_insert_find_delete(void)
{
    ing_skiplist sl;
    ING_CHECK(ing_skiplist_init(&sl) == ING_OK, "init failed");

    ing_slnode *n = NULL;
    ING_CHECK(ing_skiplist_insert(&sl, 1.0, "a", 1, &n) == ING_OK, "insert a");
    ING_CHECK(n != NULL && n->score == 1.0, "returned node wrong");
    ING_CHECK(ing_skiplist_length(&sl) == 1, "length after 1 insert");

    ING_CHECK(ing_skiplist_find(&sl, 1.0, "a", 1) == n, "find a");
    ING_CHECK(ing_skiplist_find(&sl, 1.0, "b", 1) == NULL, "find missing b");
    ING_CHECK(ing_skiplist_find(&sl, 2.0, "a", 1) == NULL, "find wrong score");

    ING_CHECK(ing_skiplist_delete(&sl, 9.0, "a", 1) == ING_ENOTFOUND, "delete wrong score");
    ING_CHECK(ing_skiplist_delete(&sl, 1.0, "zzz", 3) == ING_ENOTFOUND, "delete missing member");
    ING_CHECK(ing_skiplist_delete(&sl, 1.0, "a", 1) == ING_OK, "delete a");
    ING_CHECK(ing_skiplist_length(&sl) == 0, "length after delete");
    ING_CHECK(ing_skiplist_find(&sl, 1.0, "a", 1) == NULL, "a gone after delete");

    ing_skiplist_free(&sl);
}

static void test_duplicate_rejected(void)
{
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    ING_CHECK(ing_skiplist_insert(&sl, 5.0, "x", 1, NULL) == ING_OK, "first insert x");
    ING_CHECK(ing_skiplist_insert(&sl, 5.0, "x", 1, NULL) == ING_EEXIST, "dup rejected");
    /* Same member, different score is not a duplicate. */
    ING_CHECK(ing_skiplist_insert(&sl, 6.0, "x", 1, NULL) == ING_OK, "same member diff score ok");
    ING_CHECK(ing_skiplist_length(&sl) == 2, "length after dup attempt");

    ing_skiplist_free(&sl);
}

static void test_empty_and_single(void)
{
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    ING_CHECK(ing_skiplist_length(&sl) == 0, "empty length");
    ING_CHECK(ing_skiplist_find(&sl, 0.0, "a", 1) == NULL, "find on empty");
    ING_CHECK(ing_skiplist_rank(&sl, 0.0, "a", 1) == 0, "rank on empty");
    ING_CHECK(ing_skiplist_by_rank(&sl, 1) == NULL, "by_rank on empty");
    ING_CHECK(ing_skiplist_first_in_range(&sl, 0.0, 10.0) == NULL, "range on empty");

    ing_skiplist_insert(&sl, 3.0, "only", 4, NULL);
    ING_CHECK(ing_skiplist_rank(&sl, 3.0, "only", 4) == 1, "single element rank 1");
    ING_CHECK(ing_skiplist_by_rank(&sl, 1) != NULL, "single by_rank 1");
    ING_CHECK(ing_skiplist_by_rank(&sl, 2) == NULL, "single by_rank 2 out of range");
    ING_CHECK(sl.head->level[0].forward == sl.tail, "single element is tail");
    ING_CHECK(sl.tail->backward == NULL, "single element backward is NULL");

    ing_skiplist_free(&sl);
}

/* Comparator matching the module's own ordering rule, used by the test to
 * build an independent model to check against. */
static int model_cmp(double sa, const char *ma, size_t la, double sb, const char *mb, size_t lb)
{
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    size_t n = la < lb ? la : lb;
    if (n) {
        int c = memcmp(ma, mb, n);
        if (c) return c < 0 ? -1 : 1;
    }
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

#define N_ORDER 400

static void test_ordering_and_ties(void)
{
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    double scores[N_ORDER];
    char members[N_ORDER][16];
    int order[N_ORDER];

    /* Several repeated scores so the tie-break actually gets exercised. */
    for (int i = 0; i < N_ORDER; i++) {
        scores[i] = (double)(i % 40);
        mk(members[i], sizeof members[i], (unsigned)(N_ORDER - i)); /* distinct, non-monotone lexically */
        order[i] = i;
    }
    /* shuffle insertion order */
    unsigned seed = 12345;
    for (int i = N_ORDER - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)(seed % (unsigned)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }

    for (int i = 0; i < N_ORDER; i++) {
        int k = order[i];
        int rc = ing_skiplist_insert(&sl, scores[k], members[k], strlen(members[k]), NULL);
        ING_CHECK(rc == ING_OK, "insert %d failed rc=%d", k, rc);
    }
    ING_CHECK(ing_skiplist_length(&sl) == N_ORDER, "length after shuffled insert");

    ing_slnode *x = sl.head->level[0].forward;
    ing_slnode *prev = NULL;
    size_t count = 0;
    while (x) {
        if (prev) {
            int c = model_cmp(prev->score, prev->member, prev->mlen, x->score, x->member, x->mlen);
            ING_CHECK(c < 0, "ordering violated between consecutive nodes");
        }
        prev = x;
        count++;
        x = x->level[0].forward;
    }
    ING_CHECK(count == N_ORDER, "walked count matches length");

    ing_skiplist_free(&sl);
}

#define N_SPAN 300

static void check_ranks(ing_skiplist *sl, char members[][16], double *scores, int n, int *alive, int n_alive_hint)
{
    (void)n_alive_hint;
    /* Build the true sorted order from the model (only alive elements),
     * then confirm rank() and by_rank() agree with it exactly. */
    int idx[N_SPAN];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (alive[i]) idx[m++] = i;

    for (int a = 0; a < m; a++)
        for (int b = a + 1; b < m; b++)
            if (model_cmp(scores[idx[a]], members[idx[a]], strlen(members[idx[a]]),
                           scores[idx[b]], members[idx[b]], strlen(members[idx[b]])) > 0) {
                int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
            }

    ING_CHECK((size_t)m == ing_skiplist_length(sl), "model length matches skiplist length");

    for (int r = 0; r < m; r++) {
        int i = idx[r];
        size_t got = ing_skiplist_rank(sl, scores[i], members[i], strlen(members[i]));
        ING_CHECK(got == (size_t)(r + 1), "rank mismatch for %s: got %zu want %d", members[i], got, r + 1);

        ing_slnode *node = ing_skiplist_by_rank(sl, (size_t)(r + 1));
        ING_CHECK(node != NULL, "by_rank(%d) returned NULL", r + 1);
        if (node)
            ING_CHECK(node->score == scores[i] && node->mlen == strlen(members[i]) &&
                      memcmp(node->member, members[i], node->mlen) == 0,
                      "by_rank(%d) returned wrong node", r + 1);
    }
}

static void test_spans_after_insert_and_delete(void)
{
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    double scores[N_SPAN];
    char members[N_SPAN][16];
    int alive[N_SPAN];

    unsigned seed = 987654321u;
    for (int i = 0; i < N_SPAN; i++) {
        seed = seed * 1103515245u + 12345u;
        scores[i] = (double)(seed % 50);
        mk(members[i], sizeof members[i], (unsigned)i);
        alive[i] = 1;
        int rc = ing_skiplist_insert(&sl, scores[i], members[i], strlen(members[i]), NULL);
        ING_CHECK(rc == ING_OK, "insert %d", i);
    }

    check_ranks(&sl, members, scores, N_SPAN, alive, N_SPAN);

    /* Delete every third element and recheck: this is where broken span
     * maintenance on delete would show up as silently wrong ranks. */
    for (int i = 0; i < N_SPAN; i += 3) {
        int rc = ing_skiplist_delete(&sl, scores[i], members[i], strlen(members[i]));
        ING_CHECK(rc == ING_OK, "delete %d", i);
        alive[i] = 0;
    }

    check_ranks(&sl, members, scores, N_SPAN, alive, -1);

    ing_skiplist_free(&sl);
}

static void test_backward_and_tail(void)
{
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    enum { N = 100 };
    char members[N][16];
    double scores[N];
    unsigned seed = 42;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        mk(members[i], sizeof members[i], (unsigned)i);
        scores[i] = (double)(seed % 30);
        ing_skiplist_insert(&sl, scores[i], members[i], strlen(members[i]), NULL);
    }

    /* Delete a scattering of elements so backward/tail get exercised past
     * a plain build-once list. */
    for (int i = 0; i < N; i += 7) {
        int rc = ing_skiplist_delete(&sl, scores[i], members[i], strlen(members[i]));
        ING_CHECK(rc == ING_OK, "backward test delete %d", i);
    }

    /* Forward walk, collecting nodes. */
    ing_slnode *fwd[N + 1];
    int fn = 0;
    for (ing_slnode *x = sl.head->level[0].forward; x; x = x->level[0].forward)
        fwd[fn++] = x;

    ING_CHECK((size_t)fn == ing_skiplist_length(&sl), "forward walk count matches length");
    if (fn > 0)
        ING_CHECK(fwd[fn - 1] == sl.tail, "tail matches last forward node");
    else
        ING_CHECK(sl.tail == NULL, "empty list has NULL tail");

    /* Backward walk from tail must be the exact reverse. */
    int bn = 0;
    ing_slnode *rev[N + 1];
    for (ing_slnode *x = sl.tail; x; x = x->backward)
        rev[bn++] = x;

    ING_CHECK(bn == fn, "backward walk length matches forward walk length");
    int mismatch = 0;
    for (int i = 0; i < fn && i < bn; i++)
        if (fwd[i] != rev[fn - 1 - i]) mismatch = 1;
    ING_CHECK(!mismatch, "backward walk is not the exact reverse of forward walk");

    ing_skiplist_free(&sl);
}

static void test_range_queries(void)
{
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    for (int i = 0; i < 20; i++) {
        char m[16];
        mk(m, sizeof m, (unsigned)i);
        ing_skiplist_insert(&sl, (double)i, m, strlen(m), NULL);
    }

    ing_slnode *n = ing_skiplist_first_in_range(&sl, 5.0, 10.0);
    ING_CHECK(n != NULL && n->score == 5.0, "range start matches min");

    n = ing_skiplist_first_in_range(&sl, -100.0, 0.0);
    ING_CHECK(n != NULL && n->score == 0.0, "range clamps to first element");

    n = ing_skiplist_first_in_range(&sl, 100.0, 200.0);
    ING_CHECK(n == NULL, "range above all elements is empty");

    n = ing_skiplist_first_in_range(&sl, 3.5, 3.9);
    ING_CHECK(n == NULL, "range that falls strictly between scores is empty");

    n = ing_skiplist_first_in_range(&sl, 19.0, 19.0);
    ING_CHECK(n != NULL && n->score == 19.0, "single-point range at the last element");

    ing_skiplist_free(&sl);
}

/* ---- fuzz: skiplist vs. a dumb sorted array kept as ground truth ------- */

typedef struct { double score; char member[16]; int used; } model_item;

static int model_cmp_items(const void *pa, const void *pb)
{
    const model_item *a = pa, *b = pb;
    return model_cmp(a->score, a->member, strlen(a->member), b->score, b->member, strlen(b->member));
}

static void test_fuzz_against_model(void)
{
    enum { CAP = 600, OPS = 4000 };
    ing_skiplist sl;
    ing_skiplist_init(&sl);

    model_item pool[CAP];
    int live = 0;
    unsigned seed = 0xC0FFEEu;

    for (int i = 0; i < CAP; i++) {
        pool[i].used = 0;
        mk(pool[i].member, sizeof pool[i].member, (unsigned)i);
    }

    for (int op = 0; op < OPS; op++) {
        seed = seed * 1103515245u + 12345u;
        int idx = (int)(seed % CAP);
        seed = seed * 1103515245u + 12345u;

        if (!pool[idx].used) {
            double score = (double)(seed % 1000) / 3.0;
            int rc = ing_skiplist_insert(&sl, score, pool[idx].member, strlen(pool[idx].member), NULL);
            ING_CHECK(rc == ING_OK, "fuzz insert op %d", op);
            pool[idx].score = score;
            pool[idx].used = 1;
            live++;
        } else {
            int rc = ing_skiplist_delete(&sl, pool[idx].score, pool[idx].member, strlen(pool[idx].member));
            ING_CHECK(rc == ING_OK, "fuzz delete op %d", op);
            pool[idx].used = 0;
            live--;
        }

        ING_CHECK(ing_skiplist_length(&sl) == (size_t)live, "fuzz length agreement op %d", op);
    }

    /* Final rank check against a freshly sorted snapshot of the model. */
    model_item sorted[CAP];
    int m = 0;
    for (int i = 0; i < CAP; i++)
        if (pool[i].used) sorted[m++] = pool[i];
    qsort(sorted, (size_t)m, sizeof sorted[0], model_cmp_items);

    for (int r = 0; r < m; r++) {
        size_t got = ing_skiplist_rank(&sl, sorted[r].score, sorted[r].member, strlen(sorted[r].member));
        ING_CHECK(got == (size_t)(r + 1), "fuzz final rank mismatch for %s", sorted[r].member);
    }

    ing_skiplist_free(&sl);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("basics");
    ING_RUN(test_basic_insert_find_delete);
    ING_RUN(test_duplicate_rejected);
    ING_RUN(test_empty_and_single);

    ING_SECTION("ordering");
    ING_RUN(test_ordering_and_ties);

    ING_SECTION("spans and ranks");
    ING_RUN(test_spans_after_insert_and_delete);

    ING_SECTION("backward and tail");
    ING_RUN(test_backward_and_tail);

    ING_SECTION("range queries");
    ING_RUN(test_range_queries);

    ING_SECTION("fuzz vs model");
    ING_RUN(test_fuzz_against_model);

    return ing_test_end();
}
