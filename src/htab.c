#include "htab.h"

#include <stdlib.h>

/* Smallest power of two >= n, minimum 1. A zero-bucket table would make
 * `hcode & mask` well-defined but useless (mask would be -1, i.e. all bits
 * set, aliasing every hcode into bucket 0 of a zero-length array) so we
 * refuse to construct one. */
static size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

int ing_htab_init(ing_htab *t, size_t nbuckets)
{
    size_t n = next_pow2(nbuckets ? nbuckets : 1);
    t->tab = calloc(n, sizeof *t->tab);
    if (!t->tab)
        return ING_ENOMEM;
    t->mask = n - 1;
    t->size = 0;
    return ING_OK;
}

void ing_htab_free(ing_htab *t)
{
    /* See the header: this frees the bucket array only. The chains hanging
     * off it are the caller's structs and are simply abandoned here, not
     * touched. */
    free(t->tab);
    t->tab = NULL;
    t->mask = 0;
    t->size = 0;
}

void ing_htab_insert(ing_htab *t, ing_hnode *node)
{
    size_t b = node->hcode & t->mask;
    node->next = t->tab[b];
    t->tab[b] = node;
    t->size++;
}

ing_hnode *ing_htab_lookup(const ing_htab *t, const ing_hnode *key, ing_hnode_eq eq)
{
    size_t b = key->hcode & t->mask;
    for (ing_hnode *n = t->tab[b]; n; n = n->next) {
        /* Compare hcode before calling eq: a mismatch there means the keys
         * cannot be equal (barring a hash collision, which eq alone would
         * also have to reject), and it lets eq skip re-deriving anything
         * about the key it can read straight off n->hcode. */
        if (n->hcode == key->hcode && eq(n, key))
            return n;
    }
    return NULL;
}

ing_hnode *ing_htab_remove(ing_htab *t, const ing_hnode *key, ing_hnode_eq eq)
{
    size_t b = key->hcode & t->mask;
    /* `pp` is "the pointer that points at me" — it starts as the address of
     * the bucket head and becomes the address of a node's `next` field as
     * we walk. Unlinking is always `*pp = (*pp)->next`, whether *pp is the
     * bucket head or some node's next pointer. That is the whole reason
     * this loop has no special case for removing the first node in a
     * chain: the bucket head and an ordinary node's `next` field are, from
     * the unlinker's point of view, the same kind of slot. Without this
     * trick you'd need a `prev` variable and an if/else for "is prev NULL
     * (head) or not" at every removal. */
    for (ing_hnode **pp = &t->tab[b]; *pp; pp = &(*pp)->next) {
        ing_hnode *n = *pp;
        if (n->hcode == key->hcode && eq(n, key)) {
            *pp = n->next;
            t->size--;
            return n;
        }
    }
    return NULL;
}

int ing_htab_grow(ing_htab *t)
{
    size_t new_n = (t->mask + 1) * 2;
    ing_hnode **new_tab = calloc(new_n, sizeof *new_tab);
    if (!new_tab)
        return ING_ENOMEM;
    size_t new_mask = new_n - 1;

    /* Stop-the-world: every node currently in t->tab is re-threaded into
     * new_tab before this function returns. See the header comment for why
     * that's the design's one real weakness. */
    for (size_t b = 0; b <= t->mask; b++) {
        ing_hnode *n = t->tab[b];
        while (n) {
            ing_hnode *next = n->next;
            size_t nb = n->hcode & new_mask;
            n->next = new_tab[nb];
            new_tab[nb] = n;
            n = next;
        }
    }

    free(t->tab);
    t->tab = new_tab;
    t->mask = new_mask;
    return ING_OK;
}

size_t ing_htab_size(const ing_htab *t)
{
    return t->size;
}
