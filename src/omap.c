#include "omap.h"

#include <stdlib.h>
#include <string.h>

/* Grow once the table is this full — counting tombstones as "full" too,
 * because a tombstone still occupies a slot and still lengthens the probe
 * sequences that walk past it. A table full of tombstones has zero live
 * keys but behaves like a 100%-loaded one. */
#define ING_OMAP_MAX_LOAD_NUM 7
#define ING_OMAP_MAX_LOAD_DEN 10

/* Slots migrated per put/get/del while a resize is in flight. Small enough
 * that no single call does meaningful work, large enough that a migration
 * started at 0.7 load finishes in a bounded number of calls rather than
 * dragging on so long that `newer` itself fills up first — a real
 * production table would size this dynamically against the operation rate;
 * a fixed constant is the honest simplification for a teaching reference. */
#define ING_OMAP_MIGRATE_BATCH 16

static size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static int otable_init(ing_otable *t, size_t nslots)
{
    size_t n = next_pow2(nslots ? nslots : 1);
    t->slots = calloc(n, sizeof *t->slots);
    if (!t->slots)
        return ING_ENOMEM;
    t->mask = n - 1;
    t->count = 0;
    t->tombs = 0;
    return ING_OK;
}

/* Frees every owned key copy still parked in the table, then the slot
 * array itself. Used both for a real ing_omap_free and, internally, to
 * discard the emptied `older` table once a migration completes. */
static void otable_free(ing_otable *t)
{
    if (!t->slots)
        return;
    for (size_t i = 0; i <= t->mask; i++)
        if (t->slots[i].state == ING_SLOT_FULL)
            free(t->slots[i].key);
    free(t->slots);
    t->slots = NULL;
    t->mask = 0;
    t->count = 0;
    t->tombs = 0;
}

static int key_eq(const ing_oslot *s, uint64_t hcode, const void *key, size_t klen)
{
    return s->hcode == hcode && s->klen == klen &&
           memcmp(s->key, key, klen) == 0;
}

/* Linear probing. Stops at the first EMPTY slot because EMPTY is a true
 * "nothing was ever inserted past here on this probe sequence" marker — a
 * key that hashed to this chain would have been placed at or before this
 * slot, never after. TOMBSTONE carries no such guarantee (see
 * ing_omap_del) so the search must step over it and keep going. */
static ing_oslot *otable_find(const ing_otable *t, uint64_t hcode,
                               const void *key, size_t klen)
{
    if (!t->slots)
        return NULL;
    size_t start = hcode & t->mask;
    for (size_t i = 0; i <= t->mask; i++) {
        ing_oslot *s = &t->slots[(start + i) & t->mask];
        if (s->state == ING_SLOT_EMPTY)
            return NULL;
        if (s->state == ING_SLOT_FULL && key_eq(s, hcode, key, klen))
            return s;
    }
    return NULL;
}

/* Finds where a put of (hcode, key, klen) should land: either the existing
 * FULL slot for this exact key (an overwrite), or the first
 * TOMBSTONE/EMPTY slot on the probe sequence (a fresh insert, reusing a
 * tombstone in preference to an empty slot so probe sequences shrink back
 * down rather than growing without bound). *is_update tells the caller
 * which case happened. Returns NULL only if the table is completely full
 * of live keys and tombstones with no match found — callers are expected
 * never to hit this because ing_omap_put grows well before that point. */
static ing_oslot *otable_find_slot(ing_otable *t, uint64_t hcode,
                                    const void *key, size_t klen, int *is_update)
{
    size_t start = hcode & t->mask;
    ing_oslot *reuse = NULL;
    for (size_t i = 0; i <= t->mask; i++) {
        ing_oslot *s = &t->slots[(start + i) & t->mask];
        if (s->state == ING_SLOT_FULL) {
            if (key_eq(s, hcode, key, klen)) {
                *is_update = 1;
                return s;
            }
            continue;
        }
        if (!reuse)
            reuse = s;               /* first TOMBSTONE or EMPTY seen */
        if (s->state == ING_SLOT_EMPTY)
            break;                    /* nothing further out can match */
    }
    *is_update = 0;
    return reuse;
}

/* Moves an already-FULL slot's contents (key pointer, value, hcode) into
 * `t` verbatim — no malloc, no free, no memcmp against an existing entry.
 * Used only during migration, where the caller (ing_migrate_step) has
 * already established that no equal key exists in `t` yet, so this is
 * always a fresh insert, never an overwrite. */
static void otable_insert_move(ing_otable *t, const ing_oslot *src)
{
    size_t start = src->hcode & t->mask;
    for (size_t i = 0; i <= t->mask; i++) {
        ing_oslot *s = &t->slots[(start + i) & t->mask];
        if (s->state != ING_SLOT_FULL) {
            if (s->state == ING_SLOT_TOMBSTONE)
                t->tombs--;
            s->key = src->key;
            s->klen = src->klen;
            s->val = src->val;
            s->hcode = src->hcode;
            s->state = ING_SLOT_FULL;
            t->count++;
            return;
        }
    }
    /* Unreachable in practice: `newer` is always allocated at 2x the size
     * of the table being migrated out of, so it can never fill up purely
     * from receiving that table's contents. */
}

int ing_omap_init(ing_omap *m, size_t nslots)
{
    m->migrating = 0;
    m->migrate_cursor = 0;
    memset(&m->newer, 0, sizeof m->newer);
    return otable_init(&m->older, nslots);
}

void ing_omap_free(ing_omap *m)
{
    otable_free(&m->older);
    otable_free(&m->newer);
    m->migrating = 0;
    m->migrate_cursor = 0;
}

/* Moves ING_OMAP_MIGRATE_BATCH slots (at most) out of `older` into `newer`,
 * then, if that exhausts `older`, retires it and folds `newer` in as the
 * new `older`. Called at the top of every put/get/del so the cost of a
 * resize is paid a little at a time by whichever operations happen to run
 * during it, instead of all at once by whichever operation triggers it —
 * that is the entire difference between this map and day 1's htab.
 *
 * A slot already migrated could in principle have been re-written since —
 * no: the invariant documented in omap.h (all inserts go to `newer` once
 * migrating) means `older` is read-only from this point on except for this
 * function walking it and ing_omap_del tombstoning it. Nothing else ever
 * writes to it, so a slot examined here is exactly as it was when the
 * migration began or as ing_omap_del left it — never half-updated. */
static void migrate_step(ing_omap *m)
{
    if (!m->migrating)
        return;

    size_t n = m->older.mask + 1;
    int budget = ING_OMAP_MIGRATE_BATCH;
    while (budget-- > 0 && m->migrate_cursor < n) {
        ing_oslot *s = &m->older.slots[m->migrate_cursor++];
        /* Cleared slots must become TOMBSTONE, never EMPTY, and for the
         * exact reason ing_omap_del uses TOMBSTONE instead of EMPTY: a key
         * later in the same collision chain, not yet reached by the
         * cursor, may still be sitting further along in `older`. If this
         * slot went back to EMPTY, that key's otable_find would stop right
         * here — EMPTY means "probe never went past this point" — and
         * report it missing even though it's still there, unmigrated. A
         * TOMBSTONE says "something used to be here" and keeps the probe
         * going, exactly as it does for an explicit delete. `older` is
         * discarded as a whole once the cursor reaches the end, so there's
         * no cost to leaving tombstones behind permanently instead of
         * reclaiming them. */
        if (s->state == ING_SLOT_FULL) {
            /* If a put already landed this key in `newer` (put always
             * targets `newer` once migrating), that copy is newer than
             * this one by definition — drop the stale copy rather than
             * clobbering the fresh value. */
            if (otable_find(&m->newer, s->hcode, s->key, s->klen))
                free(s->key);
            else
                otable_insert_move(&m->newer, s);
            m->older.count--;
            m->older.tombs++;
            s->state = ING_SLOT_TOMBSTONE;
        }
        /* A slot already TOMBSTONE, or already EMPTY, needs no change. */
    }

    if (m->migrate_cursor >= n) {
        free(m->older.slots);
        m->older = m->newer;
        memset(&m->newer, 0, sizeof m->newer);
        m->migrating = 0;
        m->migrate_cursor = 0;
    }
}

/* Begins a migration if the active table (`older`, whether or not a prior
 * migration just finished) has crossed the load threshold. Allocation
 * failure here is not reported to the caller of ing_omap_put — the put
 * that triggered this check has already succeeded — it just means growth
 * is deferred and retried on the next put that crosses the threshold. */
static void maybe_start_migration(ing_omap *m)
{
    if (m->migrating)
        return;
    size_t used = m->older.count + m->older.tombs;
    size_t cap = m->older.mask + 1;
    if (used * ING_OMAP_MAX_LOAD_DEN < cap * ING_OMAP_MAX_LOAD_NUM)
        return;
    if (otable_init(&m->newer, cap * 2) != ING_OK)
        return; /* try again next time; older is still perfectly usable */
    m->migrating = 1;
    m->migrate_cursor = 0;
}

int ing_omap_put(ing_omap *m, const void *key, size_t klen, void *val)
{
    migrate_step(m);

    ing_otable *t = m->migrating ? &m->newer : &m->older;
    int is_update = 0;
    ing_oslot *s = otable_find_slot(t, ing_hash_bytes(key, klen), key, klen, &is_update);
    if (!s)
        return ING_ENOMEM; /* table is completely full; see otable_find_slot */

    if (is_update) {
        s->val = val;
        return ING_OK;
    }

    void *kcopy = malloc(klen);
    if (!kcopy && klen != 0)
        return ING_ENOMEM;
    if (klen)
        memcpy(kcopy, key, klen);

    if (s->state == ING_SLOT_TOMBSTONE)
        t->tombs--;
    s->key = kcopy;
    s->klen = klen;
    s->val = val;
    s->hcode = ing_hash_bytes(key, klen);
    s->state = ING_SLOT_FULL;
    t->count++;

    if (!m->migrating)
        maybe_start_migration(m);
    return ING_OK;
}

int ing_omap_get(ing_omap *m, const void *key, size_t klen, void **out)
{
    migrate_step(m);

    uint64_t h = ing_hash_bytes(key, klen);
    ing_oslot *s = NULL;
    if (m->migrating)
        s = otable_find(&m->newer, h, key, klen);
    if (!s)
        s = otable_find(&m->older, h, key, klen);
    if (!s)
        return ING_ENOTFOUND;
    *out = s->val;
    return ING_OK;
}

int ing_omap_del(ing_omap *m, const void *key, size_t klen)
{
    migrate_step(m);

    uint64_t h = ing_hash_bytes(key, klen);
    ing_oslot *s = NULL;
    ing_otable *t = NULL;

    if (m->migrating) {
        s = otable_find(&m->newer, h, key, klen);
        t = &m->newer;
    }
    if (!s) {
        s = otable_find(&m->older, h, key, klen);
        t = &m->older;
    }
    if (!s)
        return ING_ENOTFOUND;

    /* Cannot simply mark the slot EMPTY: otable_find relies on EMPTY
     * meaning "no key was ever inserted past this point on this probe
     * sequence" to know when to stop looking. If a later key in the same
     * chain probed past this slot and landed further along, marking this
     * one EMPTY again would make that later key unreachable — its lookup
     * would stop right here and report ING_ENOTFOUND even though it is
     * still in the table. TOMBSTONE preserves "keep looking" while still
     * freeing the key. The cost: tombstones accumulate and lengthen every
     * subsequent probe sequence through them until a migration (which
     * never carries tombstones forward — migrate_step only ever copies
     * FULL slots into `newer`) reclaims the space. */
    free(s->key);
    s->key = NULL;
    s->state = ING_SLOT_TOMBSTONE;
    t->count--;
    t->tombs++;
    return ING_OK;
}

size_t ing_omap_size(const ing_omap *m)
{
    return m->older.count + (m->migrating ? m->newer.count : 0);
}
