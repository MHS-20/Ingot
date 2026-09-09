/* htab.h — a chained hash table, intrusive.
 *
 * "Intrusive" here means the same thing it means in list.h: the link cell
 * (ing_hnode) lives inside the caller's struct, not in a node this table
 * allocates. The consequence is that ing_htab_insert cannot fail — there is
 * no allocation on the insert path at all, only pointer surgery — so it
 * returns void, not int. Contrast this with day 2's omap, which stores keys
 * and values *inline in its own slot array* and therefore must copy the key
 * on every put and can fail with ING_ENOMEM doing so.
 *
 * Because the node is embedded in the caller's struct, the table never owns
 * the memory behind a node. ing_htab_free tears down only the bucket array;
 * every node the table was holding is still live, still linked to whatever
 * else it is linked to (an LRU list, say), and it is the caller's job to
 * walk its own containers and free them. Freeing a node the table doesn't
 * own here would be freeing memory the caller might still need elsewhere.
 */
#ifndef ING_HTAB_H
#define ING_HTAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

typedef struct ing_hnode {
    struct ing_hnode *next;
    uint64_t hcode;
} ing_hnode;

/* Caller-supplied equality test. Two nodes with different hcode are never
 * compared — eq only has to disambiguate collisions within one bucket, and
 * for that it typically reaches past the ing_hnode into the enclosing
 * struct with ing_container_of. */
typedef bool (*ing_hnode_eq)(const ing_hnode *a, const ing_hnode *b);

typedef struct {
    ing_hnode **tab;   /* array of bucket heads, each a singly-linked chain */
    size_t mask;        /* nbuckets - 1; nbuckets is always a power of two */
    size_t size;         /* number of nodes currently stored */
} ing_htab;

/* nbuckets is rounded up to a power of two (minimum 1) so that bucket
 * selection can be `hcode & mask` instead of `hcode % nbuckets`. The AND is
 * one cycle; the modulo, for an arbitrary divisor, is a hardware division.
 * The cost of taking the fast path is that `& mask` only ever looks at the
 * low bits of hcode, so a hash function that doesn't mix its low bits well
 * shows up immediately as pathological clustering — a property a modulo by
 * a prime table size would have quietly hidden. ing_hash_bytes mixes well
 * enough that this doesn't bite in practice. */
int ing_htab_init(ing_htab *t, size_t nbuckets);

/* Frees the bucket array ONLY. It does not — cannot — free the nodes
 * threaded through it, because it never allocated them; see the file
 * comment above. Calling this without first reclaiming every node's
 * enclosing struct yourself leaks them. */
void ing_htab_free(ing_htab *t);

/* Caller must set node->hcode before calling. Pushes onto the front of the
 * target bucket's chain; cannot fail because it allocates nothing. */
void ing_htab_insert(ing_htab *t, ing_hnode *node);

/* key need not be a real stored node — only its hcode field and whatever eq
 * inspects through ing_container_of have to be valid. Returns NULL if
 * absent. */
ing_hnode *ing_htab_lookup(const ing_htab *t, const ing_hnode *key, ing_hnode_eq eq);

/* Detaches and returns the matching node (still owned by the caller, who
 * may now do anything with it, including reinsert it elsewhere), or NULL if
 * no node matched. */
ing_hnode *ing_htab_remove(ing_htab *t, const ing_hnode *key, ing_hnode_eq eq);

/* Stop-the-world rehash into a table with 2x the buckets. Every node in the
 * old array is walked once and re-threaded into the new one, which is
 * O(size) work done inline, on whichever caller happens to trigger it. That
 * is fine for a batch job and a bad idea for a request handler: one unlucky
 * request pays for the whole table's rehash and every other request queued
 * behind it sees a latency spike proportional to table size. day 2's omap
 * exists specifically to spread that cost — it migrates a bounded number of
 * slots per operation instead of all of them at once, at the price of a
 * more complicated lookup path that has to consult two tables while a
 * migration is in flight. Returns ING_ENOMEM if the new array can't be
 * allocated, in which case t is left unchanged and still usable.
 */
int ing_htab_grow(ing_htab *t);

size_t ing_htab_size(const ing_htab *t);

#endif /* ING_HTAB_H */
