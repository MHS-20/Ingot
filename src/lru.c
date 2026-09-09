/* lru.c — see lru.h for the design rationale. The entry below is the join
 * point between day 1's hash table and list.h's doubly-linked list: one
 * allocation, two intrusive link cells, found two different ways.
 */
#include "lru.h"
#include "htab.h"
#include "list.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

typedef struct ing_lru_entry {
    ing_hnode hnode;   /* found by key, via the hash table */
    ing_link  link;    /* threaded into the recency list; front = most
                         * recently used, back = next to evict */
    void  *key;        /* owned copy — see ing_lru_put for why we copy
                         * rather than borrow the caller's buffer */
    size_t klen;
    void  *value;      /* NOT owned; on_evict is the caller's chance to
                         * free it, which is why every removal path fires it */
} ing_lru_entry;

struct ing_lru {
    ing_htab tab;
    ing_link  list;    /* sentinel head; see list.h */
    size_t capacity;
    ing_lru_evict_fn on_evict;
    void *ctx;
};

static bool lru_entry_eq(const ing_hnode *a, const ing_hnode *b)
{
    const ing_lru_entry *ea = ing_container_of(a, ing_lru_entry, hnode);
    const ing_lru_entry *eb = ing_container_of(b, ing_lru_entry, hnode);
    return ea->klen == eb->klen && memcmp(ea->key, eb->key, ea->klen) == 0;
}

ing_lru *ing_lru_create(size_t capacity, ing_lru_evict_fn on_evict, void *ctx)
{
    if (capacity == 0) return NULL;   /* see header: no sensible ING_EINVAL path here */

    ing_lru *c = malloc(sizeof *c);
    if (!c) return NULL;

    /* Bucket count is sized off capacity, not off however big the cache
     * happens to get right now — capacity is a hard ceiling on size, so
     * the average chain length never exceeds roughly 1 and there is no
     * "grow as it fills" logic to write or reason about, unlike the ZSET,
     * whose hash table has no such ceiling. */
    if (ing_htab_init(&c->tab, capacity) != ING_OK) {
        free(c);
        return NULL;
    }
    ing_list_init(&c->list);
    c->capacity = capacity;
    c->on_evict = on_evict;
    c->ctx = ctx;
    return c;
}

static void lru_free_entry(ing_lru *c, ing_lru_entry *e)
{
    if (c->on_evict)
        c->on_evict(e->key, e->klen, e->value, c->ctx);
    free(e->key);
    free(e);
}

void ing_lru_destroy(ing_lru *c)
{
    if (!c) return;
    /* ing_list_for_each_safe rather than plain for_each: the loop body
     * removes and frees the current node, and list.h is explicit that
     * doing that with the non-safe form corrupts the iteration (remove()
     * points the freed node's own next/prev back at itself, so the
     * for_each step would read garbage out of memory we just freed). */
    ing_list_for_each_safe(e, tmp, &c->list, ing_lru_entry, link) {
        ing_list_remove(&e->link);
        lru_free_entry(c, e);
    }
    ing_htab_free(&c->tab);
    free(c);
}

static ing_hnode *lru_lookup(const ing_lru *c, const void *key, size_t klen, uint64_t *hcode_out)
{
    uint64_t hcode = ing_hash_bytes(key, klen);
    if (hcode_out) *hcode_out = hcode;
    ing_lru_entry probe = { .hnode = { .hcode = hcode }, .key = (void *)key, .klen = klen };
    return ing_htab_lookup(&c->tab, &probe.hnode, lru_entry_eq);
}

int ing_lru_put(ing_lru *c, const void *key, size_t klen, void *value)
{
    if (!c || !key) return ING_EINVAL;

    uint64_t hcode;
    ing_hnode *found = lru_lookup(c, key, klen, &hcode);
    if (found) {
        ing_lru_entry *e = ing_container_of(found, ing_lru_entry, hnode);
        /* The old value is leaving the cache just as surely as an evicted
         * one would; on_evict fires for it before it's overwritten so the
         * caller gets exactly one notification per value that ever left,
         * whether that's via eviction, remove, destroy, or here. */
        if (c->on_evict)
            c->on_evict(e->key, e->klen, e->value, c->ctx);
        e->value = value;
        /* ing_list_move_front is idempotent — see list.h — so there is no
         * need to special-case "this was already the most recent". */
        ing_list_move_front(&c->list, &e->link);
        return ING_OK;
    }

    /* A genuinely new key at capacity must evict before inserting. This
     * step allocates nothing: the victim is unlinked from an existing
     * list and removed from an existing hash table, both pure pointer
     * surgery (see list.h and htab.h). That matters because it means
     * eviction cannot fail with ENOMEM partway through a put — if it
     * could, the cache would be left in whatever half-evicted state the
     * failure happened in, with no good way to unwind. Guaranteeing
     * eviction can't fail is what keeps this function's only failure mode
     * confined to the one real allocation below. */
    if (ing_htab_size(&c->tab) >= c->capacity) {
        ing_link *tail = c->list.prev;
        ing_lru_entry *victim = ing_container_of(tail, ing_lru_entry, link);
        ing_htab_remove(&c->tab, &victim->hnode, lru_entry_eq);
        ing_list_remove(&victim->link);
        lru_free_entry(c, victim);
    }

    ing_lru_entry *e = malloc(sizeof *e);
    if (!e) return ING_ENOMEM;
    e->key = malloc(klen ? klen : 1);
    if (!e->key) { free(e); return ING_ENOMEM; }
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->value = value;
    e->hnode.hcode = hcode;

    ing_htab_insert(&c->tab, &e->hnode);
    ing_list_push_front(&c->list, &e->link);
    return ING_OK;
}

int ing_lru_get(ing_lru *c, const void *key, size_t klen, void **out)
{
    if (!c || !key || !out) return ING_EINVAL;
    ing_hnode *found = lru_lookup(c, key, klen, NULL);
    if (!found) return ING_ENOTFOUND;
    ing_lru_entry *e = ing_container_of(found, ing_lru_entry, hnode);
    ing_list_move_front(&c->list, &e->link);
    *out = e->value;
    return ING_OK;
}

int ing_lru_peek(const ing_lru *c, const void *key, size_t klen, void **out)
{
    if (!c || !key || !out) return ING_EINVAL;
    ing_hnode *found = lru_lookup(c, key, klen, NULL);
    if (!found) return ING_ENOTFOUND;
    /* Deliberately no ing_list_move_front here — that's the whole point
     * of peek existing as a separate function. See lru.h. */
    ing_lru_entry *e = ing_container_of(found, ing_lru_entry, hnode);
    *out = e->value;
    return ING_OK;
}

int ing_lru_remove(ing_lru *c, const void *key, size_t klen)
{
    if (!c || !key) return ING_EINVAL;
    ing_hnode *found = lru_lookup(c, key, klen, NULL);
    if (!found) return ING_ENOTFOUND;
    ing_lru_entry *e = ing_container_of(found, ing_lru_entry, hnode);
    ing_htab_remove(&c->tab, &e->hnode, lru_entry_eq);
    ing_list_remove(&e->link);
    lru_free_entry(c, e);
    return ING_OK;
}

size_t ing_lru_size(const ing_lru *c)
{
    return c ? ing_htab_size(&c->tab) : 0;
}

size_t ing_lru_capacity(const ing_lru *c)
{
    return c ? c->capacity : 0;
}
