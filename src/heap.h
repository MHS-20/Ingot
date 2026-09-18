/* heap.h — a binary min-heap, and the cheapest useful ordering there is.
 *
 * Placed before the skip list because the two are the opposite ends of one
 * argument. Modules 3 and 4 spend their length maintaining a *total* order —
 * every element's position relative to every other — which is what makes
 * rank and range queries possible and what is paid for on every mutation.
 * A heap is the structure that asks whether that was ever needed. If the
 * only question is "what is the smallest thing in here right now", a total
 * order is work done and thrown away.
 *
 * So the invariant here is strictly weaker: every node is <= its children,
 * and nothing at all is promised about siblings, cousins, or any two nodes
 * that are not on one root-to-leaf path. That is a partial order, and the
 * entire performance profile falls out of it — O(1) peek, O(log n) push and
 * pop, O(n) to build, and no rank, no range query, no ordered traversal, and
 * O(n) to find anything that is not the minimum.
 *
 * NOT SORTED, and this is the model people get wrong. Walking `items` from 0
 * to len yields an order that is almost never ascending; only items[0] is
 * guaranteed to be anything. A fully sorted array *does* satisfy the heap
 * property, so sortedness is permitted and never promised — which is exactly
 * why the tests assert the parent-child inequality against the layout rather
 * than asserting that pops come out ascending. The latter is also satisfied
 * by an insertion-sorted array, which is a fine priority queue and is not a
 * heap.
 *
 * NO POINTERS. Every other ordered structure here stores its shape in
 * pointers that get followed. This one stores it in index arithmetic: the
 * node at i has children at 2i+1 and 2i+2 and its parent at (i-1)/2. There
 * is no left, no right, no parent field; the array *is* the tree, read
 * breadth-first. One allocation, zero per-node overhead, and the hot upper
 * levels stay in cache — against a skip list node that can carry 32 forward
 * pointers and costs a malloc each.
 *
 * The price of that representation is that the array must stay dense. No
 * gaps, ever, because a gap invalidates the arithmetic for every index after
 * it. That single constraint is what dictates both algorithms below: a push
 * must append at the end (the only placement that keeps density) and
 * therefore sifts up, and a pop must backfill the root with the *last*
 * element (the only one whose removal leaves no hole elsewhere) and
 * therefore sifts down.
 *
 * MIN, NOT MAX, and there is no second exercise hiding here: a max-heap is
 * this file with one comparison reversed, or this file with negated keys.
 * Whether the comparison should have been a caller-supplied function pointer
 * instead is a real question; it is not asked here because a double key
 * covers the cases this library cares about and an indirect call on the hot
 * path of every sift is not free.
 */
#ifndef ING_HEAP_H
#define ING_HEAP_H

#include <stddef.h>

#include "common.h"

/* The heap owns the array of these and never the memory behind `val`, which
 * is an opaque pointer it does not dereference and does not free. */
typedef struct {
    double key;   /* the priority; smaller leaves first */
    void *val;
} ing_heap_item;

/* `len` is how many items are live, `cap` is how many the allocation holds.
 * Everything at or past `len` is scratch: stale bytes from a previous life,
 * never read, overwritten by the next push before anyone can observe them.
 * `len` is the only source of truth for what is in the heap.
 *
 * The layout is public because it is contract, not detail: children of i at
 * 2i+1 and 2i+2, parent at (i-1)/2, root at items[0], and
 * items[(i-1)/2].key <= items[i].key for every i in [1, len). */
typedef struct {
    ing_heap_item *items;
    size_t len;
    size_t cap;
} ing_heap;

/* `cap` is a starting capacity, not a limit. Zero is legal and means
 * "allocate on the first push", which is the right default for a heap whose
 * eventual size is unknown. */
int ing_heap_init(ing_heap *h, size_t cap);

/* Frees the item array and zeroes the header. Never touches a `val`. */
void ing_heap_free(ing_heap *h);

/* O(log n) amortised. ING_ENOMEM if growth fails, in which case the heap is
 * unchanged and still usable — the allocation is attempted before anything
 * is mutated, which is what makes that promise cheap to keep.
 *
 * Duplicate keys are accepted, in deliberate contrast to the skip list's
 * rejection of a duplicate (score, member): a priority queue has no reason
 * to care that two things are equally urgent, and no way to break the tie
 * that would mean anything to the caller. */
int ing_heap_push(ing_heap *h, double key, void *val);

/* Removes and returns the minimum. O(log n). ING_ENOTFOUND on an empty heap,
 * with *out left untouched. */
int ing_heap_pop(ing_heap *h, ing_heap_item *out);

/* The minimum, without removing it. O(1), and the operation the entire
 * structure exists to make free. ING_ENOTFOUND on an empty heap, with *out
 * left untouched. */
int ing_heap_peek(const ing_heap *h, ing_heap_item *out);

/* Replaces the contents with these n items, heapified in place. Prior
 * contents are discarded without being popped.
 *
 * O(n), not O(n log n), and the gap between those two is the reason this
 * function exists instead of a documented loop of pushes. Sifting down from
 * the last internal node backwards does linear total work: half the nodes
 * are leaves and travel zero, a quarter travel at most one, an eighth at
 * most two, and only the root can travel the full height — the distance
 * grows logarithmically while the population it applies to decays
 * geometrically, and the decay wins. Building by insertion has exactly the
 * opposite profile, since sifting *up* gives the longest journey to the most
 * numerous nodes. */
int ing_heap_build(ing_heap *h, const ing_heap_item *items, size_t n);

size_t ing_heap_size(const ing_heap *h);

#endif /* ING_HEAP_H */
