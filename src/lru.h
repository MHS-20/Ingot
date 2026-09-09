/* lru.h — day 5: an O(1) LRU cache.
 *
 * Lookup by key needs a hash table (day 1); knowing what's least recently
 * used, and evicting it without walking anything, needs an intrusive
 * doubly-linked list (the one from list.h) ordered by recency. This is the
 * same lesson as day 4's ZSET, one struct down the hall: one entry lives in
 * two intrusive structures at once — an `ing_hnode` for the table, an
 * `ing_link` for the list — as a single allocation. Where the ZSET needed
 * that to get O(1) score *and* O(log n) order out of one entry, the LRU
 * needs it to get O(1) lookup *and* O(1) eviction out of one entry: no
 * hash table alone can tell you what's least recently used, and no plain
 * list alone can find a key without a linear scan.
 *
 * Why doubly linked and not singly linked: eviction removes the tail. A
 * singly-linked list can walk forward from the head in O(1) per step but
 * cannot get from the last node back to the second-to-last without either
 * a full traversal or a second, prev-tracking pointer — which is exactly
 * what "doubly linked" buys. Without it, eviction stops being O(1) and the
 * whole point of this cache is lost.
 */
#ifndef ING_LRU_H
#define ING_LRU_H

#include <stddef.h>

typedef struct ing_lru ing_lru;

/* Called for every entry that leaves the cache, whether by explicit
 * eviction, an overwritten put, ing_lru_remove, or ing_lru_destroy walking
 * out whatever is left. Exactly once per entry, never zero, never twice —
 * a caller relying on this to free `value` (or close a file descriptor,
 * etc.) needs that guarantee to avoid both leaks and double frees. */
typedef void (*ing_lru_evict_fn)(const void *key, size_t klen, void *value, void *ctx);

/* capacity == 0 is rejected (returns NULL, no way to report ING_EINVAL
 * through a constructor that returns a pointer) — a zero-capacity cache
 * can never hold anything, which makes every put an immediate evict of
 * what was just inserted; that's a degenerate no-op mode not worth
 * supporting silently. on_evict may be NULL if the caller doesn't need the
 * callback (e.g. values are ints, not owned pointers). */
ing_lru *ing_lru_create(size_t capacity, ing_lru_evict_fn on_evict, void *ctx);

/* Fires on_evict for every entry still resident, in whatever order the
 * list holds them, then frees the cache itself. */
void ing_lru_destroy(ing_lru *c);

/* Inserts a new key, or overwrites the value of an existing one (firing
 * on_evict for the value being replaced, since it is leaving the cache
 * just as surely as an evicted one is) and refreshes its recency either
 * way. If the cache is at capacity and this is a genuinely new key, the
 * least-recently-used entry is evicted first to make room — and that
 * eviction is guaranteed to succeed, see the note in lru.c, so this
 * function's only failure mode is ING_ENOMEM on the new entry's own
 * allocation. */
int ing_lru_put(ing_lru *c, const void *key, size_t klen, void *value);

/* ING_OK with *out set, or ING_ENOTFOUND. A hit moves the entry to the
 * front of the recency list — that's what makes it "least recently used"
 * eviction rather than "least recently inserted". */
int ing_lru_get(ing_lru *c, const void *key, size_t klen, void **out);

/* Same lookup, but does not touch recency. A cache is a structure whose
 * behaviour depends on the history of operations performed on it, which
 * makes "just look, don't disturb anything" a capability worth having in
 * its own right: metrics code that samples a value shouldn't change what
 * gets evicted next, and a test asserting eviction order would corrupt the
 * very order it's trying to check if checking it were itself a `get`. */
int ing_lru_peek(const ing_lru *c, const void *key, size_t klen, void **out);

/* Fires on_evict for the removed entry. ING_ENOTFOUND if the key is absent. */
int ing_lru_remove(ing_lru *c, const void *key, size_t klen);

size_t ing_lru_size(const ing_lru *c);
size_t ing_lru_capacity(const ing_lru *c);

#endif /* ING_LRU_H */
