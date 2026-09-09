/* skiplist.h — a probabilistic ordered structure, keyed by (score, member).
 *
 * Ordering is score ascending, ties broken by lexicographic comparison of
 * the member bytes. That tie-break is not decoration: day 4's sorted set
 * needs a total order over (score, member) pairs so that two members with
 * equal score still have a well-defined position, and everything below —
 * insert, rank, range — assumes that comparator is consistent everywhere.
 *
 * Why a skip list and not a balanced tree: a skip list keeps its shape in
 * *expectation* rather than by *enforcement*. Insert flips coins to decide
 * how many levels a new node gets, and never rotates, rebalances, or
 * recolors anything. That is why this file is a fraction of the size of an
 * AVL or red-black insert, and it is also the tradeoff: the O(log n) bounds
 * here are expected, not worst-case. An adversary who knew the coin flips in
 * advance could degrade this to a linked list; a balanced tree cannot be
 * degraded that way. For a teaching reference and for day 4's sorted set,
 * expected-log-n is the right trade for the enormous simplification.
 *
 * The other thing worth the trouble: `span`. Every forward pointer at every
 * level records how many level-0 nodes it jumps over. Summing spans along
 * the search path gives a node's rank for free, during the same descent
 * insert and delete already do — turning "what index is this element at"
 * from an O(n) walk into an O(log n) one. Getting span arithmetic right at
 * both insert and delete is the entire point of this module; get it wrong
 * and ranks silently drift while everything else still looks correct.
 */
#ifndef ING_SKIPLIST_H
#define ING_SKIPLIST_H

#include <stddef.h>

#define ING_SL_MAXLEVEL 32
#define ING_SL_P 0.25   /* probability a node gets another level */

typedef struct ing_slnode {
    double score;
    char  *member;                 /* owned copy */
    size_t mlen;
    struct ing_slnode *backward;   /* for reverse iteration; NULL at the first real node */
    int    height;
    struct ing_sllevel { struct ing_slnode *forward; size_t span; } level[];  /* flexible array */
} ing_slnode;

typedef struct {
    ing_slnode *head;   /* sentinel: height ING_SL_MAXLEVEL, holds no member */
    ing_slnode *tail;
    size_t length;
    int level;          /* highest level currently in use, 1-based */
} ing_skiplist;

int  ing_skiplist_init(ing_skiplist *sl);
void ing_skiplist_free(ing_skiplist *sl);

/* Rejects a duplicate (score, member) pair with ING_EEXIST rather than
 * silently updating it — a skip list node's height is baked in at
 * insertion, so "insert or update" would either need a second code path
 * that rewrites a node in place (fragile: its levels were sized for the
 * old occupant) or a delete+reinsert the caller can just as well do
 * themselves. Making that explicit keeps this function's contract simple. */
int  ing_skiplist_insert(ing_skiplist *sl, double score, const void *member,
                          size_t mlen, ing_slnode **out);

int  ing_skiplist_delete(ing_skiplist *sl, double score, const void *member, size_t mlen);

ing_slnode *ing_skiplist_find(const ing_skiplist *sl, double score,
                               const void *member, size_t mlen);

/* 1-based rank; 0 means "not present". Ranks start at 1 (the smallest
 * element) so that 0 is free to mean absence — a caller can write
 * `if (rank == 0)` instead of threading a separate found/not-found flag
 * through every call site. */
size_t ing_skiplist_rank(const ing_skiplist *sl, double score, const void *member, size_t mlen);

/* 1-based; NULL if rank is 0 or exceeds the length. */
ing_slnode *ing_skiplist_by_rank(const ing_skiplist *sl, size_t rank);

/* First node with min <= score <= max, or NULL if none. Walk ->level[0].forward
 * from there for the rest of the range. */
ing_slnode *ing_skiplist_first_in_range(const ing_skiplist *sl, double min, double max);

size_t ing_skiplist_length(const ing_skiplist *sl);

#endif /* ING_SKIPLIST_H */
