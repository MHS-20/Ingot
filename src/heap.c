#include "heap.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

/* Eight, not one. Growing from a zero capacity has to pick some floor, and
 * `cap * 2` picks zero forever; `cap * 2 + 1` picks 1, 3, 7, 15 and pays for
 * four reallocations before holding anything worth holding. */
#define ING_HEAP_MIN_CAP 8

/* Both sifts hold the moving item aside rather than swapping pairwise. A
 * swap writes three times per level; this writes once per level plus once at
 * the end, because the item's intermediate positions are never observed by
 * anyone. Same logic, half the stores. */

/* Toward the root, for as long as the item is smaller than its parent. Used
 * after a push, where the appended item is the only node that can be out of
 * place and a leaf has exactly one upward path — which is why there is no
 * choice of direction to make here, and why this is the simpler of the two. */
static void sift_up(ing_heap *h, size_t i)
{
    ing_heap_item moving = h->items[i];

    /* `i != 0` first, and not merely for the loop bound: i is unsigned, so
     * computing (0 - 1) / 2 for the root would wrap to SIZE_MAX rather than
     * yield -1, and index the array somewhere in the next process. */
    while (i != 0) {
        size_t parent = (i - 1) / 2;

        if (h->items[parent].key <= moving.key)
            break;

        h->items[i] = h->items[parent];
        i = parent;
    }

    h->items[i] = moving;
}

/* Toward the leaves, for as long as the item is larger than its smallest
 * child. Used after a pop, where the last element has been moved to the root
 * and is almost certainly far too large for it, and by build from the bottom
 * up.
 *
 * Against the *smallest* child, and that is not a stylistic preference: with
 * keys 5 at the root over children 2 and 3, swapping with the larger child
 * gives root 3 over children 2 and 5, which passes an eyeball check at the
 * swapped edge and leaves 3 sitting above 2. */
static void sift_down(ing_heap *h, size_t i)
{
    ing_heap_item moving = h->items[i];
    size_t left;

    /* Only the left child needs an existence test. The array is dense and
     * fills left to right, so a node with a right child always has a left
     * one, and a node without a left child is a leaf and is done. The test
     * is re-evaluated every iteration because a node three levels down may
     * have one child where the node that started the descent had two. */
    while ((left = 2 * i + 1) < h->len) {
        size_t right = left + 1;
        size_t smallest = left;

        /* Short-circuit order matters: when the right child does not exist,
         * the bounds test fails first and its key is never read. */
        if (right < h->len && h->items[right].key < h->items[left].key)
            smallest = right;

        if (h->items[smallest].key >= moving.key)
            break;

        h->items[i] = h->items[smallest];
        i = smallest;
    }

    h->items[i] = moving;
}

/* Grows to hold at least `want` items. Separated out because push and build
 * need the same "only if it is not already big enough" test, and because the
 * failure contract — old array intact, heap untouched — is easier to keep in
 * one place than in two. */
static int reserve(ing_heap *h, size_t want)
{
    if (h->cap >= want)
        return ING_OK;

    size_t newcap = h->cap ? h->cap * 2 : ING_HEAP_MIN_CAP;
    if (newcap < want)
        newcap = want;

    /* Into a temporary, never straight back into h->items: realloc returns
     * NULL on failure *without* freeing the old block, so assigning the
     * result directly would leak the only pointer to a live allocation. */
    ing_heap_item *p = realloc(h->items, newcap * sizeof *p);
    if (!p)
        return ING_ENOMEM;

    h->items = p;
    h->cap = newcap;
    return ING_OK;
}

int ing_heap_init(ing_heap *h, size_t cap)
{
    memset(h, 0, sizeof *h);

    if (cap == 0)
        return ING_OK;

    h->items = malloc(cap * sizeof *h->items);
    if (!h->items)
        return ING_ENOMEM;

    h->cap = cap;
    return ING_OK;
}

void ing_heap_free(ing_heap *h)
{
    free(h->items);
    memset(h, 0, sizeof *h);
}

int ing_heap_push(ing_heap *h, double key, void *val)
{
    int rc = reserve(h, h->len + 1);
    if (rc < 0)
        return rc;

    h->items[h->len] = (ing_heap_item){ .key = key, .val = val };
    sift_up(h, h->len);
    h->len++;
    return ING_OK;
}

int ing_heap_pop(ing_heap *h, ing_heap_item *out)
{
    if (h->len == 0)
        return ING_ENOTFOUND;

    *out = h->items[0];

    /* The last element is the only one that can backfill the root without
     * leaving a hole somewhere else, and len is decremented *before* the
     * sift: sift_down reads h->len to bound the descent, and the element now
     * duplicated at the tail is no longer part of the heap. Sifting first
     * lets that stale slot take part in comparisons. */
    h->items[0] = h->items[h->len - 1];
    h->len--;

    if (h->len > 1)
        sift_down(h, 0);

    return ING_OK;
}

int ing_heap_peek(const ing_heap *h, ing_heap_item *out)
{
    if (h->len == 0)
        return ING_ENOTFOUND;

    *out = h->items[0];
    return ING_OK;
}

int ing_heap_build(ing_heap *h, const ing_heap_item *items, size_t n)
{
    int rc = reserve(h, n);
    if (rc < 0)
        return rc;

    /* The items are plain data with no ownership the heap has to respect
     * field by field, so the whole block moves at once. */
    if (n)
        memcpy(h->items, items, n * sizeof *h->items);
    h->len = n;

    /* Fewer than two items is already a heap, and the guard is load-bearing
     * rather than an optimisation: n / 2 - 1 underflows to SIZE_MAX for
     * n < 2. */
    if (n < 2)
        return ING_OK;

    /* Backwards from the last node that has a child. The direction is the
     * precondition: sift_down(i) is only correct if both subtrees under i
     * are already heaps, and descending indices guarantee that every index
     * inside i's subtree has been sifted before i is reached. This is also
     * why the heapify cannot be interleaved with the copy above — half the
     * array would not be populated yet.
     *
     * Decrement inside the body rather than in the for-clause, because index
     * 0 must be processed and `i--` on an unsigned 0 wraps instead of
     * terminating. */
    for (size_t i = n / 2 - 1; ; i--) {
        sift_down(h, i);
        if (i == 0)
            break;
    }

    return ING_OK;
}

size_t ing_heap_size(const ing_heap *h)
{
    return h->len;
}
