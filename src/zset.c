/* zset.c — see zset.h for the design rationale. This file wires an
 * intrusive hash table (day 1) and an intrusive skip list (day 3) into one
 * structure, and the entry below is the join point: it carries an
 * ing_hnode for the table and a pointer to its ing_slnode in the list.
 *
 * We chose "pointer to the skip list node" over "store the score directly
 * and re-find the skip list node by (score, member) on demand". The
 * pointer costs one extra machine word per entry; the alternative costs an
 * O(log n) skip list search on every score access from inside the hash
 * side, which would make even ing_zset_score O(log n) again — precisely
 * the guarantee this module exists to keep O(1). Paying one pointer's worth
 * of memory for that is the right trade every time.
 */
#include "zset.h"
#include "htab.h"
#include "list.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

typedef struct ing_zset_entry {
    ing_hnode hnode;      /* found by member, via the hash table */
    ing_slnode *sl;       /* this entry's node, found by (score, member) via
                            * the skip list; owned by the skip list, not by
                            * this entry — see the destroy comment below. */
    const void *member;  /* == sl->member: the same pointer, not a copy.
                            * Kept redundantly on the entry so that hash
                            * table equality (and probing for lookup/remove,
                            * which needs a key to compare before any real
                            * entry exists) never has to dereference `sl` —
                            * that keeps the aliasing simple: this is a real
                            * pointer into memory the skip list owns, read
                            * with its real type, never a stand-in struct
                            * pretending to be an ing_slnode. */
    size_t mlen;
} ing_zset_entry;

struct ing_zset {
    ing_htab tab;
    ing_skiplist sl;
};

/* Compares member bytes via the entry's own member/mlen fields, which
 * always mirror the skip list node's — see the entry struct comment for
 * why those are kept redundantly rather than dereferenced through `sl`. */
static bool zset_entry_eq(const ing_hnode *a, const ing_hnode *b)
{
    const ing_zset_entry *ea = ing_container_of(a, ing_zset_entry, hnode);
    const ing_zset_entry *eb = ing_container_of(b, ing_zset_entry, hnode);
    return ea->mlen == eb->mlen &&
           memcmp(ea->member, eb->member, ea->mlen) == 0;
}

/* Grow once size exceeds the bucket count, i.e. once the average chain
 * length would exceed 1. A chained table's lookup cost is O(1 + size /
 * nbuckets) — the "1 +" is the bucket index step, the rest is the average
 * chain walk. Left ungrown, that chain walk grows linearly with the set
 * and every O(1) claim this module makes quietly becomes O(n). */
#define ING_ZSET_INITIAL_BUCKETS 16

static int zset_maybe_grow(ing_zset *z)
{
    if (ing_htab_size(&z->tab) > z->tab.mask + 1)
        return ing_htab_grow(&z->tab);
    return ING_OK;
}

ing_zset *ing_zset_create(void)
{
    ing_zset *z = malloc(sizeof *z);
    if (!z) return NULL;
    if (ing_htab_init(&z->tab, ING_ZSET_INITIAL_BUCKETS) != ING_OK) {
        free(z);
        return NULL;
    }
    if (ing_skiplist_init(&z->sl) != ING_OK) {
        ing_htab_free(&z->tab);
        free(z);
        return NULL;
    }
    return z;
}

/* Ownership rule: the skip list owns every ing_slnode (member bytes and
 * all), because ing_skiplist_free already knows how to walk and free every
 * node it holds in one pass. The entry struct is what *this* module
 * allocated, and it owns only itself plus its embedded ing_hnode. So
 * teardown is: free every entry (found by walking the hash table's
 * buckets, since that costs nothing extra we don't already pay for
 * ing_htab_free), then let ing_skiplist_free reclaim the skip list nodes
 * in one pass. Doing it in the other order — freeing skip list nodes
 * first — would leave every entry's `sl` pointer dangling for the brief
 * window before the entry itself is freed; freeing entries first avoids
 * ever holding a dangling pointer, even momentarily. */
void ing_zset_destroy(ing_zset *z)
{
    if (!z) return;
    for (size_t b = 0; b <= z->tab.mask; b++) {
        ing_hnode *n = z->tab.tab[b];
        while (n) {
            ing_hnode *next = n->next;
            ing_zset_entry *e = ing_container_of(n, ing_zset_entry, hnode);
            free(e);
            n = next;
        }
    }
    ing_htab_free(&z->tab);
    ing_skiplist_free(&z->sl);
    free(z);
}

/* A stack probe good enough for zset_entry_eq: it only reads hcode,
 * member and mlen, all real values supplied by the caller — no stand-in
 * for a skip list node required. `sl` is left NULL; eq never touches it. */
static ing_hnode *zset_lookup(const ing_zset *z, const void *member, size_t mlen, uint64_t hcode)
{
    ing_zset_entry probe = { .hnode = { .hcode = hcode }, .sl = NULL,
                              .member = member, .mlen = mlen };
    return ing_htab_lookup(&z->tab, &probe.hnode, zset_entry_eq);
}

int ing_zset_add(ing_zset *z, double score, const void *member, size_t mlen)
{
    if (!z || !member) return ING_EINVAL;

    uint64_t hcode = ing_hash_bytes(member, mlen);
    ing_hnode *found = zset_lookup(z, member, mlen, hcode);

    if (found) {
        ing_zset_entry *e = ing_container_of(found, ing_zset_entry, hnode);
        if (e->sl->score == score)
            return ING_OK;   /* no-op: already at this score */

        /* Delete-then-reinsert is the only way to move a skip list node;
         * see the header comment on ing_zset_add for why. The hash table
         * entry (and its ing_hnode) is never touched — only the ing_slnode
         * pointer it holds gets replaced. */
        int rc = ing_skiplist_delete(&z->sl, e->sl->score, member, mlen);
        if (rc != ING_OK) return rc;   /* should be unreachable if the two structures agree */

        ing_slnode *node;
        rc = ing_skiplist_insert(&z->sl, score, member, mlen, &node);
        if (rc != ING_OK) return rc;
        e->sl = node;
        e->member = node->member;   /* the skip list's own copy, not the caller's buffer */
        e->mlen = node->mlen;
        return ING_OK;
    }

    ing_slnode *node;
    int rc = ing_skiplist_insert(&z->sl, score, member, mlen, &node);
    if (rc != ING_OK) return rc;   /* ING_EEXIST can't happen: we just proved absence above */

    ing_zset_entry *e = malloc(sizeof *e);
    if (!e) {
        ing_skiplist_delete(&z->sl, score, member, mlen);
        return ING_ENOMEM;
    }
    e->hnode.hcode = hcode;
    e->sl = node;
    e->member = node->member;       /* mirrors the skip list's owned copy; see struct comment */
    e->mlen = node->mlen;
    ing_htab_insert(&z->tab, &e->hnode);

    rc = zset_maybe_grow(z);
    if (rc != ING_OK) {
        /* Growth failure doesn't undo the insert — the table is still a
         * perfectly correct, just more collision-prone, table at its old
         * size. Reporting the error here would be misleading: the add
         * itself succeeded. */
    }
    return ING_OK;
}

int ing_zset_score(const ing_zset *z, const void *member, size_t mlen, double *out)
{
    if (!z || !member || !out) return ING_EINVAL;
    uint64_t hcode = ing_hash_bytes(member, mlen);
    ing_hnode *found = zset_lookup(z, member, mlen, hcode);
    if (!found) return ING_ENOTFOUND;

    /* The whole point: one hash lookup, one field read, done. No skip list
     * function is called on this path. */
    ing_zset_entry *e = ing_container_of(found, ing_zset_entry, hnode);
    *out = e->sl->score;
    return ING_OK;
}

int ing_zset_remove(ing_zset *z, const void *member, size_t mlen)
{
    if (!z || !member) return ING_EINVAL;
    uint64_t hcode = ing_hash_bytes(member, mlen);

    ing_zset_entry probe = { .hnode = { .hcode = hcode }, .sl = NULL,
                              .member = member, .mlen = mlen };

    ing_hnode *removed = ing_htab_remove(&z->tab, &probe.hnode, zset_entry_eq);
    if (!removed) return ING_ENOTFOUND;

    ing_zset_entry *e = ing_container_of(removed, ing_zset_entry, hnode);
    double score = e->sl->score;
    int rc = ing_skiplist_delete(&z->sl, score, member, mlen);
    free(e);
    return rc;   /* ING_OK, or ING_ENOTFOUND only if the two structures had drifted apart */
}

size_t ing_zset_rank(const ing_zset *z, const void *member, size_t mlen)
{
    if (!z || !member) return 0;
    uint64_t hcode = ing_hash_bytes(member, mlen);
    ing_hnode *found = zset_lookup(z, member, mlen, hcode);
    if (!found) return 0;
    ing_zset_entry *e = ing_container_of(found, ing_zset_entry, hnode);
    /* This one DOES walk the skip list — rank isn't stored anywhere, it's
     * derived from summed spans, and that's exactly the job the skip list
     * exists to do in O(log n). */
    return ing_skiplist_rank(&z->sl, e->sl->score, member, mlen);
}

size_t ing_zset_size(const ing_zset *z)
{
    return z ? ing_htab_size(&z->tab) : 0;
}

size_t ing_zset_range(const ing_zset *z, size_t start_rank, size_t max, ing_slnode **out)
{
    if (!z || !out || start_rank == 0) return 0;
    ing_slnode *n = ing_skiplist_by_rank(&z->sl, start_rank);
    size_t count = 0;
    while (n && count < max) {
        out[count++] = n;
        n = n->level[0].forward;
    }
    return count;
}

size_t ing_zset_range_by_score(const ing_zset *z, double min, double max, size_t limit,
                                ing_slnode **out)
{
    if (!z || !out) return 0;
    ing_slnode *n = ing_skiplist_first_in_range(&z->sl, min, max);
    size_t count = 0;
    while (n && n->score <= max && count < limit) {
        out[count++] = n;
        n = n->level[0].forward;
    }
    return count;
}
