/* zset.h — day 4: a sorted set, `member -> score`, ordered by (score, member).
 *
 * Neither structure this module leans on can do the whole job alone. The
 * hash table (day 1) gives O(1) score lookup but no order. The skip list
 * (day 3) gives O(log n) rank and range queries but no O(1) lookup by key.
 * A ZSET needs both at once, on the *same* logical entry — and that is only
 * possible because both underlying structures are intrusive: an
 * `ing_hnode` and a pointer to an `ing_slnode` can both live inside one
 * allocation, so one entry is, simultaneously, a node the hash table can
 * find by member and a node the skip list can find by (score, member).
 * That single fact is the whole lesson of the day; every design choice
 * below follows from it.
 */
#ifndef ING_ZSET_H
#define ING_ZSET_H

#include <stddef.h>

#include "skiplist.h"

typedef struct ing_zset ing_zset;   /* opaque, heap-allocated */

ing_zset *ing_zset_create(void);

/* Frees every entry exactly once. Ownership rule: the entry struct (hnode +
 * pointer to its skip list node) is what this module allocates and frees;
 * the skip list node itself is owned, as always, by the skip list, and is
 * torn down by walking the skip list once via ing_skiplist_free rather than
 * by following each entry's pointer. Freeing it twice — once through the
 * entry and once through the skip list — is exactly the bug this ownership
 * split has to avoid. */
void ing_zset_destroy(ing_zset *z);

/* Insert or update. Updating an existing member's score must move it to a
 * new position in the (score, member) ordering. The skip list has no
 * cheaper way to do that than deleting the old (score, member) pair and
 * inserting the new one — there is no "reseat this node at a different
 * score" operation, because a node's height was chosen for the position it
 * was inserted at, not for wherever it might move to later. So every score
 * update pays a full O(log n) delete plus O(log n) insert, even though only
 * one field changed. The hash table entry itself is untouched by this: its
 * ing_hnode never moves, only the ing_slnode pointer it holds is replaced. */
int ing_zset_add(ing_zset *z, double score, const void *member, size_t mlen);

/* O(1): one hash lookup, then a direct field read off the entry's skip list
 * pointer. This function must never call into the skip list's search path
 * — doing so would defeat the entire point of keeping a hash table around.
 * ING_ENOTFOUND if the member is absent. */
int ing_zset_score(const ing_zset *z, const void *member, size_t mlen, double *out);

int ing_zset_remove(ing_zset *z, const void *member, size_t mlen);

/* 1-based rank ascending by (score, member); 0 if the member is absent. */
size_t ing_zset_rank(const ing_zset *z, const void *member, size_t mlen);

size_t ing_zset_size(const ing_zset *z);

/* Ascending by (score, member), 1-based start_rank. Writes at most `max`
 * node pointers into `out` and returns how many were written (0 if
 * start_rank is 0, out of range, or the set is empty). */
size_t ing_zset_range(const ing_zset *z, size_t start_rank, size_t max, ing_slnode **out);

/* All members with min <= score <= max, up to `limit` entries. */
size_t ing_zset_range_by_score(const ing_zset *z, double min, double max, size_t limit,
                                ing_slnode **out);

#endif /* ING_ZSET_H */
