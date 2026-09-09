#include "harness.h"
#include "htab.h"
#include "list.h"

#include <stdlib.h>
#include <string.h>

/* Exercises ing_container_of: the ing_hnode is embedded, not standalone. */
struct entry {
    ing_hnode node;
    char key[32];
    int val;
};

static bool entry_eq(const ing_hnode *a, const ing_hnode *b)
{
    struct entry *ea = ing_container_of(a, struct entry, node);
    struct entry *eb = ing_container_of(b, struct entry, node);
    return strcmp(ea->key, eb->key) == 0;
}

static struct entry *make_entry(const char *key, int val, uint64_t hcode_override, int use_override)
{
    struct entry *e = malloc(sizeof *e);
    strncpy(e->key, key, sizeof e->key - 1);
    e->key[sizeof e->key - 1] = '\0';
    e->val = val;
    e->node.hcode = use_override ? hcode_override : ing_hash_bytes(key, strlen(key));
    return e;
}

static ing_hnode *lookup_key(const ing_htab *t, const char *key)
{
    struct entry probe;
    strncpy(probe.key, key, sizeof probe.key - 1);
    probe.key[sizeof probe.key - 1] = '\0';
    probe.node.hcode = ing_hash_bytes(key, strlen(key));
    return ing_htab_lookup(t, &probe.node, entry_eq);
}

static void test_basic(void)
{
    ing_htab t;
    ING_CHECK(ing_htab_init(&t, 4) == ING_OK, "init failed");

    struct entry *a = make_entry("alpha", 1, 0, 0);
    struct entry *b = make_entry("beta", 2, 0, 0);
    ing_htab_insert(&t, &a->node);
    ing_htab_insert(&t, &b->node);
    ING_CHECK(ing_htab_size(&t) == 2, "size should be 2, got %zu", ing_htab_size(&t));

    ing_hnode *found = lookup_key(&t, "alpha");
    ING_CHECK(found && ing_container_of(found, struct entry, node)->val == 1,
              "alpha lookup wrong");

    found = lookup_key(&t, "missing");
    ING_CHECK(found == NULL, "missing key should not be found");

    ing_hnode *removed = ing_htab_remove(&t, &a->node, entry_eq);
    ING_CHECK(removed == &a->node, "remove should return the alpha node");
    ING_CHECK(ing_htab_size(&t) == 1, "size should drop to 1 after remove");
    ING_CHECK(lookup_key(&t, "alpha") == NULL, "alpha should be gone after remove");

    free(a);
    free(b);
    ing_htab_free(&t);
}

static void test_missing_and_empty(void)
{
    ing_htab t;
    ING_CHECK(ing_htab_init(&t, 8) == ING_OK, "init failed");
    ING_CHECK(lookup_key(&t, "nope") == NULL, "lookup on empty table must miss");
    ING_CHECK(ing_htab_remove(&t, &(ing_hnode){.hcode = 1}, entry_eq) == NULL,
              "remove on empty table must miss");
    ing_htab_free(&t); /* freeing an empty table must not crash */
}

/* Forces several distinct keys into the SAME bucket by giving them the
 * SAME hcode. This is the only way to actually exercise chain walking —
 * without it every key would land in its own bucket and removal's
 * "pointer to the pointer that points at me" trick would never be tested
 * on anything but a one-element chain. */
static void test_collisions(void)
{
    ing_htab t;
    ING_CHECK(ing_htab_init(&t, 4) == ING_OK, "init failed");

    struct entry *e[5];
    const char *names[5] = {"one", "two", "three", "four", "five"};
    for (int i = 0; i < 5; i++) {
        e[i] = make_entry(names[i], i, 42, 1); /* all collide on hcode 42 */
        ing_htab_insert(&t, &e[i]->node);
    }
    ING_CHECK(ing_htab_size(&t) == 5, "collision insert count wrong");

    for (int i = 0; i < 5; i++) {
        struct entry probe;
        strcpy(probe.key, names[i]);
        probe.node.hcode = 42;
        ing_hnode *found = ing_htab_lookup(&t, &probe.node, entry_eq);
        ING_CHECK(found == &e[i]->node, "collision lookup missed %s", names[i]);
    }

    /* Remove the middle of the chain, then the head, then the tail — all
     * three positions must work through the same generic loop. */
    ing_hnode *r = ing_htab_remove(&t, &e[2]->node, entry_eq);
    ING_CHECK(r == &e[2]->node, "remove middle failed");
    r = ing_htab_remove(&t, &e[4]->node, entry_eq); /* last inserted = chain head */
    ING_CHECK(r == &e[4]->node, "remove head failed");
    r = ing_htab_remove(&t, &e[0]->node, entry_eq); /* first inserted = chain tail */
    ING_CHECK(r == &e[0]->node, "remove tail failed");
    ING_CHECK(ing_htab_size(&t) == 2, "size after removals wrong");

    for (int i = 0; i < 5; i++)
        free(e[i]);
    ing_htab_free(&t);
}

static void test_grow_preserves_elements(void)
{
    ing_htab t;
    ING_CHECK(ing_htab_init(&t, 4) == ING_OK, "init failed");

    enum { N = 10000 };
    struct entry **es = malloc(N * sizeof *es);
    for (int i = 0; i < N; i++) {
        char key[32];
        snprintf(key, sizeof key, "key-%d", i);
        es[i] = make_entry(key, i, 0, 0);
        ing_htab_insert(&t, &es[i]->node);
        if (i % 500 == 0)
            ING_CHECK(ing_htab_grow(&t) == ING_OK, "grow failed at i=%d", i);
    }
    ING_CHECK(ing_htab_size(&t) == N, "size should be %d, got %zu", N, ing_htab_size(&t));

    int all_found = 1;
    for (int i = 0; i < N; i++) {
        char key[32];
        snprintf(key, sizeof key, "key-%d", i);
        if (lookup_key(&t, key) == NULL)
            all_found = 0;
    }
    ING_CHECK(all_found, "every inserted key must survive growth");

    for (int i = 0; i < N; i++)
        free(es[i]);
    free(es);
    ing_htab_free(&t);
}

static void test_delete_then_reinsert(void)
{
    ing_htab t;
    ING_CHECK(ing_htab_init(&t, 4) == ING_OK, "init failed");

    struct entry *a = make_entry("gamma", 7, 0, 0);
    ing_htab_insert(&t, &a->node);
    ing_hnode *removed = ing_htab_remove(&t, &a->node, entry_eq);
    ING_CHECK(removed == &a->node, "remove failed before reinsert");
    ING_CHECK(ing_htab_size(&t) == 0, "size should be 0 after remove");

    a->val = 99;
    ing_htab_insert(&t, &a->node);
    ing_hnode *found = lookup_key(&t, "gamma");
    ING_CHECK(found && ing_container_of(found, struct entry, node)->val == 99,
              "reinsert did not stick");
    ING_CHECK(ing_htab_size(&t) == 1, "size should be 1 after reinsert");

    free(a);
    ing_htab_free(&t);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("basic insert/lookup/remove");
    ING_RUN(test_basic);

    ING_SECTION("missing keys and empty table");
    ING_RUN(test_missing_and_empty);

    ING_SECTION("forced collisions exercise chaining");
    ING_RUN(test_collisions);

    ING_SECTION("growth preserves every element");
    ING_RUN(test_grow_preserves_elements);

    ING_SECTION("delete then reinsert");
    ING_RUN(test_delete_then_reinsert);

    return ing_test_end();
}
