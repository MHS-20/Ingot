/* skiplist.c — see skiplist.h for the design rationale (sentinel, coin flips,
 * why span exists at all). This file is the mechanics: the comparator that
 * defines the total order, the RNG that decides node height, and the two
 * places — insert and delete — where span has to be kept exactly right.
 */
#include "skiplist.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

/* ---- ordering -----------------------------------------------------------
 *
 * The order is (score, member): score first, then lexicographic byte
 * comparison of the member when scores tie. memcmp over the shorter length
 * followed by a length comparison gives the usual "prefix is smaller"
 * behaviour (e.g. "ab" < "abc"), same as strcmp on NUL-free byte strings.
 * Every other function in this file — search, rank, delete — is only
 * correct if this comparator is the single source of truth for ordering.
 */
static int ing_sl_cmp(double sa, const void *ma, size_t la,
                       double sb, const void *mb, size_t lb)
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

/* ---- RNG -----------------------------------------------------------------
 *
 * xorshift64*, not libc rand(). Two reasons: rand()'s state is process-wide,
 * so a skip list that calls it is quietly coupled to whatever else in the
 * program seeds or consumes rand() — surprising action at a distance for a
 * data structure that has no business touching global state. And a
 * self-contained generator with a fixed seed makes a "few thousand random
 * ops" fuzz test reproduce identically on every run, which matters when it
 * fails: you want the same failing sequence twice, not a Heisenbug that
 * needs re-seeding to reproduce.
 */
static uint64_t ing_sl_rng_state = 0x2545F4914F6CDD1DULL;

static uint64_t ing_sl_xorshift(void)
{
    uint64_t x = ing_sl_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    ing_sl_rng_state = x;
    return x;
}

/* Geometric distribution with parameter ING_SL_P: each level above the
 * first is granted independently with probability P, so P(height == k) =
 * P^(k-1) * (1-P) and the expected height is 1/(1-P). This is the entire
 * "rebalancing" story for a skip list — there is no rotation, no rebuilt
 * subtree, just a coin flip made once at insertion and never revisited.
 * That is strictly weaker than a balanced tree's worst-case guarantee, and
 * it is why the structure has to be trusted only in expectation. */
static int ing_sl_random_level(void)
{
    int level = 1;
    /* (P * 0xFFFF) as an integer threshold avoids pulling in floating point
     * comparisons for something this hot a path. */
    const uint32_t threshold = (uint32_t)(ING_SL_P * 0xFFFF);
    while (level < ING_SL_MAXLEVEL &&
           (uint32_t)(ing_sl_xorshift() & 0xFFFF) < threshold)
        level++;
    return level;
}

static ing_slnode *ing_sl_node_new(int height, double score, const void *member, size_t mlen)
{
    ing_slnode *n = malloc(sizeof *n + (size_t)height * sizeof(n->level[0]));
    if (!n) return NULL;
    n->member = malloc(mlen ? mlen : 1);
    if (!n->member) { free(n); return NULL; }
    memcpy(n->member, member, mlen);
    n->score = score;
    n->mlen = mlen;
    n->backward = NULL;
    n->height = height;
    for (int i = 0; i < height; i++) {
        n->level[i].forward = NULL;
        n->level[i].span = 0;
    }
    return n;
}

int ing_skiplist_init(ing_skiplist *sl)
{
    if (!sl) return ING_EINVAL;
    /* The sentinel is a real allocation, sized for the tallest node the
     * list will ever hold, so that "insert into an empty list" and "insert
     * a node taller than any seen so far" are the *same* code path as any
     * other insert: update[] always bottoms out at a real node (the head),
     * never at NULL. Every insert/delete/search below leans on that. */
    sl->head = ing_sl_node_new(ING_SL_MAXLEVEL, 0, "", 0);
    if (!sl->head) return ING_ENOMEM;
    sl->tail = NULL;
    sl->length = 0;
    sl->level = 1;
    return ING_OK;
}

void ing_skiplist_free(ing_skiplist *sl)
{
    if (!sl || !sl->head) return;
    ing_slnode *x = sl->head->level[0].forward;
    while (x) {
        ing_slnode *next = x->level[0].forward;
        free(x->member);
        free(x);
        x = next;
    }
    free(sl->head->member);
    free(sl->head);
    sl->head = NULL;
    sl->tail = NULL;
    sl->length = 0;
    sl->level = 0;
}

size_t ing_skiplist_length(const ing_skiplist *sl)
{
    return sl ? sl->length : 0;
}

int ing_skiplist_insert(ing_skiplist *sl, double score, const void *member,
                         size_t mlen, ing_slnode **out)
{
    if (!sl || !member) return ING_EINVAL;

    ing_slnode *update[ING_SL_MAXLEVEL];
    size_t rank[ING_SL_MAXLEVEL];

    ing_slnode *x = sl->head;
    for (int i = sl->level - 1; i >= 0; i--) {
        /* rank[i] accumulates, at each level, the number of level-0 nodes
         * between the head and `x` — the sum of every span walked so far.
         * By the time the descent reaches level 0, rank[0] is exactly the
         * 0-based position the new node will land at, which is what lets
         * span be computed below without a second pass. */
        rank[i] = (size_t)(i == sl->level - 1 ? 0 : rank[i + 1]);
        while (x->level[i].forward &&
               ing_sl_cmp(x->level[i].forward->score, x->level[i].forward->member,
                          x->level[i].forward->mlen, score, member, mlen) < 0) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Reject duplicates up front. A skip list node's height is fixed at
     * creation, so "insert or overwrite in place" would mean rewriting a
     * node whose levels were sized for whatever it used to be — not worth
     * it when the caller can delete-then-insert themselves. */
    if (x->level[0].forward &&
        ing_sl_cmp(x->level[0].forward->score, x->level[0].forward->member,
                   x->level[0].forward->mlen, score, member, mlen) == 0)
        return ING_EEXIST;

    int level = ing_sl_random_level();
    if (level > sl->level) {
        /* Levels between the old height and the new one have never been
         * touched by any node, so every one of them spans the *entire*
         * list so far — head's forward pointer at that level is still
         * NULL and its span is the full length, exactly as if one giant
         * jump covered everything. */
        for (int i = sl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = sl->head;
            update[i]->level[i].span = sl->length;
        }
        sl->level = level;
    }

    ing_slnode *node = ing_sl_node_new(level, score, member, mlen);
    if (!node) return ING_ENOMEM;

    for (int i = 0; i < level; i++) {
        node->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = node;

        /* update[i] used to span (update[i]->level[i].span) nodes to reach
         * whatever came after it. The new node is inserted (rank[0] -
         * rank[i]) nodes after update[i] (that difference is how far
         * update[i] at *this* level is behind the level-0 position — the
         * higher the level, the further behind, since it skips more).
         * So: update[i]'s new span is that distance plus one (to count the
         * new node itself), and what the new node then points past is
         * whatever remained of the old span after subtracting that
         * distance. */
        node->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    /* Levels above the new node's height never gained a new forward
     * pointer, but the new node still lives underneath them, so every span
     * that passes over it has to grow by one to keep counting level-0
     * nodes correctly. */
    for (int i = level; i < sl->level; i++)
        update[i]->level[i].span++;

    node->backward = (update[0] == sl->head) ? NULL : update[0];
    if (node->level[0].forward)
        node->level[0].forward->backward = node;
    else
        sl->tail = node;

    sl->length++;
    if (out) *out = node;
    return ING_OK;
}

int ing_skiplist_delete(ing_skiplist *sl, double score, const void *member, size_t mlen)
{
    if (!sl || !member) return ING_EINVAL;

    ing_slnode *update[ING_SL_MAXLEVEL];
    ing_slnode *x = sl->head;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               ing_sl_cmp(x->level[i].forward->score, x->level[i].forward->member,
                          x->level[i].forward->mlen, score, member, mlen) < 0)
            x = x->level[i].forward;
        update[i] = x;
    }

    ing_slnode *target = x->level[0].forward;
    if (!target ||
        ing_sl_cmp(target->score, target->member, target->mlen, score, member, mlen) != 0)
        return ING_ENOTFOUND;

    for (int i = 0; i < sl->level; i++) {
        if (update[i]->level[i].forward == target) {
            /* update[i] is about to absorb whatever target used to skip
             * over, minus the one node (target itself) that is leaving. */
            update[i]->level[i].span += target->level[i].span - 1;
            update[i]->level[i].forward = target->level[i].forward;
        } else {
            /* This level never pointed at target directly, but target sat
             * underneath its span, so the span shrinks by one regardless. */
            update[i]->level[i].span--;
        }
    }

    if (target->level[0].forward)
        target->level[0].forward->backward = target->backward;
    else
        sl->tail = target->backward;

    /* Levels that only ever existed because of taller-than-average nodes
     * can empty out entirely once the tallest node is gone; shrinking
     * sl->level keeps every search descent starting from the true top
     * instead of walking through levels that lead nowhere. */
    while (sl->level > 1 && sl->head->level[sl->level - 1].forward == NULL)
        sl->level--;

    free(target->member);
    free(target);
    sl->length--;
    return ING_OK;
}

ing_slnode *ing_skiplist_find(const ing_skiplist *sl, double score, const void *member, size_t mlen)
{
    if (!sl || !member) return NULL;
    ing_slnode *x = sl->head;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               ing_sl_cmp(x->level[i].forward->score, x->level[i].forward->member,
                          x->level[i].forward->mlen, score, member, mlen) < 0)
            x = x->level[i].forward;
    }
    ing_slnode *cand = x->level[0].forward;
    if (cand && ing_sl_cmp(cand->score, cand->member, cand->mlen, score, member, mlen) == 0)
        return cand;
    return NULL;
}

size_t ing_skiplist_rank(const ing_skiplist *sl, double score, const void *member, size_t mlen)
{
    if (!sl || !member) return 0;
    ing_slnode *x = sl->head;
    size_t rank = 0;
    for (int i = sl->level - 1; i >= 0; i--) {
        /* Note <= here, not <: unlike a plain search, rank has to walk
         * *onto* a node whose key equals the target so that its own span
         * gets folded into the running total. That's what turns "distance
         * from head to just before the target" into "distance from head
         * to the target itself". */
        while (x->level[i].forward &&
               ing_sl_cmp(x->level[i].forward->score, x->level[i].forward->member,
                          x->level[i].forward->mlen, score, member, mlen) <= 0) {
            rank += x->level[i].span;
            x = x->level[i].forward;
        }
    }
    if (x != sl->head && ing_sl_cmp(x->score, x->member, x->mlen, score, member, mlen) == 0)
        return rank;
    return 0;
}

ing_slnode *ing_skiplist_by_rank(const ing_skiplist *sl, size_t rank)
{
    if (!sl || rank == 0 || rank > sl->length) return NULL;
    ing_slnode *x = sl->head;
    size_t traversed = 0;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && traversed + x->level[i].span <= rank) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank)
            return x;
    }
    return NULL;
}

ing_slnode *ing_skiplist_first_in_range(const ing_skiplist *sl, double min, double max)
{
    if (!sl || min > max) return NULL;
    ing_slnode *x = sl->head;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < min)
            x = x->level[i].forward;
    }
    ing_slnode *cand = x->level[0].forward;
    if (cand && cand->score <= max)
        return cand;
    return NULL;
}
