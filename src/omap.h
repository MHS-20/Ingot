/* omap.h — open-addressed hash map with incremental (amortized) resizing.
 *
 * Two ways this differs from day 1's htab, both forced by the same design
 * choice: entries live *inline in a flat slot array* instead of being
 * linked through a cell embedded in the caller's struct.
 *
 *   - It cannot be intrusive. There is no "the caller's struct" to embed a
 *     link in; the slot array is the only struct. So the map must own
 *     copies of the keys (and, for open addressing to work at all, the
 *     values just sit in the same slot as the key).
 *   - Insertion can fail. Copying the key is a malloc caller code never
 *     sees, so ing_omap_put returns ING_ENOMEM where ing_htab_insert
 *     couldn't fail at all.
 *
 * The other departure from day 1 is *how* it grows. ing_htab_grow rehashes
 * the whole table in one call — a latency spike proportional to table size,
 * paid by whichever caller triggers it. This map instead keeps two slot
 * arrays, `older` and `newer`, and a migration cursor, and moves a bounded
 * number of slots out of `older` on every subsequent put/get/del until
 * `older` is empty and can be dropped. No single call ever does O(n) work
 * for the resize; the cost is amortized across the calls that follow it,
 * and paid for by whoever happens to make them.
 *
 * The invariant that makes a migration-in-flight safe: once a migration
 * starts, every new key goes into `newer`, never into `older`. That means
 * `older` is strictly non-growing for the rest of the migration — it only
 * ever loses slots (as they are migrated out) or has slots read from it (a
 * lookup falls through to `older` when `newer` doesn't have the key) — so
 * there is no race between "insert a fresh key into older" and "the cursor
 * has already passed that slot," because that insert simply cannot happen.
 */
#ifndef ING_OMAP_H
#define ING_OMAP_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

enum ing_slot_state { ING_SLOT_EMPTY = 0, ING_SLOT_FULL, ING_SLOT_TOMBSTONE };

typedef struct {
    void *key;
    size_t klen;
    void *val;
    uint64_t hcode;
    enum ing_slot_state state;
} ing_oslot;

typedef struct {
    ing_oslot *slots;
    size_t mask;   /* nslots - 1; nslots a power of two */
    size_t count;  /* FULL slots */
    size_t tombs;  /* TOMBSTONE slots */
} ing_otable;

typedef struct {
    ing_otable older, newer;
    int migrating;       /* 0 = single table (older); 1 = migration in flight */
    size_t migrate_cursor; /* next un-migrated slot index in older */
} ing_omap;

int    ing_omap_init(ing_omap *m, size_t nslots);
void   ing_omap_free(ing_omap *m);   /* frees every owned key copy, then both slot arrays */

/* Copies key (klen bytes); does not copy val, which is an opaque pointer the
 * caller keeps ownership of. Overwrites the value (and, in case klen or the
 * bytes differ from any prior copy — they can't for equal keys, but the key
 * copy is always refreshed for clarity) of an existing key rather than
 * duplicating it. */
int    ing_omap_put(ing_omap *m, const void *key, size_t klen, void *val);
int    ing_omap_get(ing_omap *m, const void *key, size_t klen, void **out);
int    ing_omap_del(ing_omap *m, const void *key, size_t klen);
size_t ing_omap_size(const ing_omap *m);

#endif /* ING_OMAP_H */
