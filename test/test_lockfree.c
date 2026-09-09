#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "lockfree.h"

/* ===========================================================================
 * Treiber stack — the uncontended behaviour that makes it look fine
 * ======================================================================== */

static void test_stack_basic(void)
{
    ing_lfstack s;
    ing_lfnode a, b, c;

    ing_lfstack_init(&s);
    ING_CHECK(ing_lfstack_pop(&s) == NULL, "pop on an empty stack must be NULL");

    ing_lfstack_push(&s, &a);
    ing_lfstack_push(&s, &b);
    ing_lfstack_push(&s, &c);

    ING_CHECK(ing_lfstack_pop(&s) == &c, "a stack is LIFO: c must come back first");
    ING_CHECK(ing_lfstack_pop(&s) == &b, "then b");
    ING_CHECK(ing_lfstack_pop(&s) == &a, "then a");
    ING_CHECK(ing_lfstack_pop(&s) == NULL, "and then it is empty again");
}

/* ===========================================================================
 * ABA, demonstrated rather than described.
 *
 * The interference runs inside the pop hook, on the popping thread itself, at
 * exactly the instruction where a preempted thread would have been stalled.
 * That makes the corruption deterministic: it reproduces on every run, on
 * every machine, under every optimisation level. Racing two real threads and
 * waiting for the window to be hit would prove the same thing far less often
 * and far less clearly.
 * ======================================================================== */

static ing_lfstack  aba_stack;
static ing_lfnode   node_a, node_b, node_c;
static int          aba_interfered;
static ing_lfnode  *aba_stolen_a, *aba_stolen_b;

static void aba_interfere(void *ctx)
{
    (void)ctx;
    if (aba_interfered)          /* the pops below re-enter this hook */
        return;
    aba_interfered = 1;

    /* Another thread empties the top of the stack and then pushes the
     * original head back. The stack goes A -> B -> C, then B -> C, then
     * C, then A -> C. Head is A both before and after: A, B, A. */
    aba_stolen_a = ing_lfstack_pop(&aba_stack);
    aba_stolen_b = ing_lfstack_pop(&aba_stack);
    ing_lfstack_push(&aba_stack, aba_stolen_a);
}

static void test_stack_aba_corrupts(void)
{
    ing_lfstack_init(&aba_stack);
    aba_interfered = 0;
    aba_stolen_a = aba_stolen_b = NULL;

    ing_lfstack_push(&aba_stack, &node_c);
    ing_lfstack_push(&aba_stack, &node_b);
    ing_lfstack_push(&aba_stack, &node_a);   /* A -> B -> C */

    ing_lfstack_pop_hook = aba_interfere;
    ing_lfnode *got = ing_lfstack_pop(&aba_stack);
    ing_lfstack_pop_hook = NULL;

    ING_CHECK(aba_interfered, "the hook must have fired inside pop");
    ING_CHECK(aba_stolen_a == &node_a && aba_stolen_b == &node_b,
              "the interfering pops must have taken a and b");

    /* The victim read head == A and next == B, then committed head = B after
     * the interference had already handed B to somebody else. The CAS
     * succeeded because A == A, which was never the question we meant to
     * ask. */
    ING_CHECK(got == &node_a,
              "the victim's pop still returns a — the same node the "
              "interfering thread already popped and pushed back");

    /* Tally how many times each node was handed to an owner across the whole
     * scenario. In a correct stack every node is delivered exactly once; that
     * is the entire contract of pop. Here two nodes are delivered twice.
     *
     * A: to the interfering thread, and again to the victim.
     * B: to the interfering thread, and again to whoever drains the stack
     *    next — because the victim committed head = B, resurrecting a node
     *    that had already left. */
    int delivered[3] = {0, 0, 0};
    ing_lfnode *const nodes[3] = { &node_a, &node_b, &node_c };
#define TALLY(p) do { for (int i_ = 0; i_ < 3; i_++) \
                          if ((p) == nodes[i_]) delivered[i_]++; } while (0)

    TALLY(aba_stolen_a);
    TALLY(aba_stolen_b);
    TALLY(got);
    for (ing_lfnode *n = ing_lfstack_pop(&aba_stack); n;
         n = ing_lfstack_pop(&aba_stack))
        TALLY(n);
#undef TALLY

    ING_CHECK(delivered[0] == 2,
              "CORRUPTION: a was delivered to two different owners, "
              "expected the bug to show 2 deliveries, saw %d", delivered[0]);
    ING_CHECK(delivered[1] == 2,
              "CORRUPTION: b was popped by the interfering thread and then "
              "resurrected onto the stack by the victim's stale CAS, so it is "
              "delivered twice; saw %d", delivered[1]);
    ING_CHECK(delivered[2] == 1,
              "c is the only node the bug leaves alone, saw %d deliveries",
              delivered[2]);
}

/* The identical interleaving against the tagged stack. */
static ing_lfstack_tagged  tagged_stack;
static int                 tagged_interfered;
static ing_lfnode         *tagged_stolen_a, *tagged_stolen_b;

static void tagged_interfere(void *ctx)
{
    (void)ctx;
    if (tagged_interfered)
        return;
    tagged_interfered = 1;

    tagged_stolen_a = ing_lfstack_tagged_pop(&tagged_stack);
    tagged_stolen_b = ing_lfstack_tagged_pop(&tagged_stack);
    ing_lfstack_tagged_push(&tagged_stack, tagged_stolen_a);
}

static void test_tagged_survives_aba(void)
{
    ing_lfstack_tagged_init(&tagged_stack);
    tagged_interfered = 0;

    ing_lfstack_tagged_push(&tagged_stack, &node_c);
    ing_lfstack_tagged_push(&tagged_stack, &node_b);
    ing_lfstack_tagged_push(&tagged_stack, &node_a);

    uint16_t before = ing_lfstack_tagged_tag(&tagged_stack);

    ing_lfstack_tagged_pop_hook = tagged_interfere;
    ing_lfnode *got = ing_lfstack_tagged_pop(&tagged_stack);
    ing_lfstack_tagged_pop_hook = NULL;

    ING_CHECK(tagged_interfered, "the hook must have fired inside pop");
    ING_CHECK(ing_lfstack_tagged_tag(&tagged_stack) != before,
              "the tag must have advanced across the interference");

    /* The victim's CAS compared (A, old_tag) against (A, old_tag + 3) and
     * failed, so it re-read and retried against the stack as it actually is:
     * A -> C. It returns A and leaves C. */
    ING_CHECK(got == &node_a, "the retry must still return a");

    ing_lfnode *rest = ing_lfstack_tagged_pop(&tagged_stack);
    ING_CHECK(rest == &node_c,
              "c must still be there and b must NOT have been resurrected, "
              "got %p want %p", (void *)rest, (void *)&node_c);
    ING_CHECK(ing_lfstack_tagged_pop(&tagged_stack) == NULL,
              "and nothing else is left");
}

static void test_tagged_basic(void)
{
    ing_lfstack_tagged s;
    ing_lfnode a, b;

    ing_lfstack_tagged_init(&s);
    ING_CHECK(ing_lfstack_tagged_pop(&s) == NULL, "empty tagged pop is NULL");

    ing_lfstack_tagged_push(&s, &a);
    ing_lfstack_tagged_push(&s, &b);
    ING_CHECK(ing_lfstack_tagged_pop(&s) == &b, "LIFO: b first");
    ING_CHECK(ing_lfstack_tagged_pop(&s) == &a, "then a");
    ING_CHECK(ing_lfstack_tagged_pop(&s) == NULL, "then empty");
}

/* ===========================================================================
 * Contended stress: pop-then-push-back, which is precisely the node-recycling
 * pattern that makes ABA reachable in the first place.
 * ======================================================================== */

#define STRESS_NODES   64
#define STRESS_THREADS 8
#define STRESS_ITERS   20000

static ing_lfstack_tagged  stress_stack;
static ing_lfnode          stress_nodes[STRESS_NODES];

static void *stress_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        ing_lfnode *n = ing_lfstack_tagged_pop(&stress_stack);
        if (n)
            ing_lfstack_tagged_push(&stress_stack, n);
    }
    return NULL;
}

static void test_tagged_stress(void)
{
    pthread_t th[STRESS_THREADS];

    ing_lfstack_tagged_init(&stress_stack);
    for (int i = 0; i < STRESS_NODES; i++)
        ing_lfstack_tagged_push(&stress_stack, &stress_nodes[i]);

    for (int i = 0; i < STRESS_THREADS; i++)
        pthread_create(&th[i], NULL, stress_worker, NULL);
    for (int i = 0; i < STRESS_THREADS; i++)
        pthread_join(th[i], NULL);

    /* Conservation is the whole assertion: every node that went in must come
     * out exactly once. A lost node means a dropped update; a duplicated one
     * means a cycle was spliced into the list. */
    int seen[STRESS_NODES];
    memset(seen, 0, sizeof seen);

    int count = 0, dup = 0, alien = 0;
    for (ing_lfnode *n = ing_lfstack_tagged_pop(&stress_stack); n;
         n = ing_lfstack_tagged_pop(&stress_stack)) {
        ptrdiff_t idx = n - stress_nodes;
        if (idx < 0 || idx >= STRESS_NODES) { alien++; continue; }
        if (seen[idx]++) dup++;
        if (++count > STRESS_NODES * 4) break;   /* a cycle; do not hang */
    }

    ING_CHECK(alien == 0, "every popped node must be one we pushed, %d were not", alien);
    ING_CHECK(dup == 0, "no node may appear twice, %d did", dup);
    ING_CHECK(count == STRESS_NODES,
              "all %d nodes must survive %d concurrent pop/push cycles, found %d",
              STRESS_NODES, STRESS_THREADS * STRESS_ITERS, count);
}

/* ===========================================================================
 * Michael-Scott queue
 * ======================================================================== */

static void test_queue_basic(void)
{
    ing_lfqueue q;
    void *v;
    ing_lfqnode *retired;

    /* One block for every node this test will ever need: three items plus the
     * dummy. Freed as one block at the end, which is only possible because
     * the queue never frees a node itself. */
    ing_lfqnode *n = malloc(4 * sizeof *n);
    ING_CHECK(n != NULL, "malloc");
    if (!n) return;

    ing_lfqueue_init(&q, &n[3]);
    ING_CHECK(ing_lfqueue_pop(&q, &v, &retired) == ING_EAGAIN,
              "pop on an empty queue must be ING_EAGAIN");

    ing_lfqueue_push(&q, &n[0], (void *)1);
    ing_lfqueue_push(&q, &n[1], (void *)2);
    ing_lfqueue_push(&q, &n[2], (void *)3);

    /* FIFO, unlike the stack above — this is the day-7 queue's ordering
     * rebuilt without a mutex. */
    ING_CHECK(ing_lfqueue_pop(&q, &v, &retired) == ING_OK && v == (void *)1,
              "FIFO: 1 must come out first, got %p", v);
    ING_CHECK(ing_lfqueue_pop(&q, &v, &retired) == ING_OK && v == (void *)2,
              "then 2, got %p", v);
    ING_CHECK(ing_lfqueue_pop(&q, &v, &retired) == ING_OK && v == (void *)3,
              "then 3, got %p", v);
    ING_CHECK(ing_lfqueue_pop(&q, &v, &retired) == ING_EAGAIN,
              "then empty again");

    int left = 0;
    while (ing_lfqueue_drain(&q))
        left++;
    ING_CHECK(left == 1,
              "a drained queue still holds exactly its dummy node, found %d",
              left);
    free(n);
}

#define Q_PRODUCERS 4
#define Q_CONSUMERS 4
#define Q_PER_PROD  5000
#define Q_TOTAL     (Q_PRODUCERS * Q_PER_PROD)

static ing_lfqueue       mq;
static _Atomic(int)      mq_consumed;
static _Atomic(int)      mq_counts[Q_TOTAL + 1];

typedef struct {
    int          id;
    ing_lfqnode *nodes;
} producer_arg;

static void *mq_producer(void *arg)
{
    producer_arg *p = arg;
    for (int i = 0; i < Q_PER_PROD; i++) {
        int value = p->id * Q_PER_PROD + i + 1;   /* 1..Q_TOTAL, unique */
        ing_lfqueue_push(&mq, &p->nodes[i], (void *)(intptr_t)value);
    }
    return NULL;
}

static void *mq_consumer(void *arg)
{
    (void)arg;
    /* Retired nodes are simply dropped here: they belong to the producers'
     * arrays and are freed after every thread has been joined, which is the
     * "wait for a quiescent point" reclamation strategy the header describes.
     * Freeing them inside this loop would be the use-after-free. */
    while (atomic_load(&mq_consumed) < Q_TOTAL) {
        void *v;
        ing_lfqnode *retired;
        if (ing_lfqueue_pop(&mq, &v, &retired) == ING_OK) {
            int value = (int)(intptr_t)v;
            if (value >= 1 && value <= Q_TOTAL)
                atomic_fetch_add(&mq_counts[value], 1);
            atomic_fetch_add(&mq_consumed, 1);
        }
    }
    return NULL;
}

static void test_queue_mpmc(void)
{
    pthread_t prod[Q_PRODUCERS], cons[Q_CONSUMERS];
    producer_arg args[Q_PRODUCERS];
    static ing_lfqnode dummy;

    ing_lfqueue_init(&mq, &dummy);
    atomic_store(&mq_consumed, 0);
    for (int i = 0; i <= Q_TOTAL; i++)
        atomic_store(&mq_counts[i], 0);

    for (int i = 0; i < Q_PRODUCERS; i++) {
        args[i].id = i;
        args[i].nodes = malloc(Q_PER_PROD * sizeof *args[i].nodes);
        ING_CHECK(args[i].nodes != NULL, "malloc producer nodes");
        if (!args[i].nodes) return;
    }

    for (int i = 0; i < Q_CONSUMERS; i++)
        pthread_create(&cons[i], NULL, mq_consumer, NULL);
    for (int i = 0; i < Q_PRODUCERS; i++)
        pthread_create(&prod[i], NULL, mq_producer, &args[i]);

    for (int i = 0; i < Q_PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    for (int i = 0; i < Q_CONSUMERS; i++)
        pthread_join(cons[i], NULL);

    ING_CHECK(atomic_load(&mq_consumed) == Q_TOTAL,
              "every produced item must be consumed: %d of %d",
              atomic_load(&mq_consumed), Q_TOTAL);

    int missing = 0, duplicated = 0;
    for (int v = 1; v <= Q_TOTAL; v++) {
        int c = atomic_load(&mq_counts[v]);
        if (c == 0) missing++;
        else if (c > 1) duplicated++;
    }
    ING_CHECK(missing == 0, "no item may be lost, %d were", missing);
    ING_CHECK(duplicated == 0, "no item may be delivered twice, %d were", duplicated);

    /* Every thread has been joined, so this is the quiescent point at which
     * retired nodes finally become safe to release. */
    while (ing_lfqueue_drain(&mq))
        ;
    for (int i = 0; i < Q_PRODUCERS; i++)
        free(args[i].nodes);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("the Treiber stack");
    ING_RUN(test_stack_basic);

    ING_SECTION("ABA: the bug, and the tag that stops it");
    ING_RUN(test_stack_aba_corrupts);
    ING_RUN(test_tagged_basic);
    ING_RUN(test_tagged_survives_aba);
    ING_RUN(test_tagged_stress);

    ING_SECTION("the Michael-Scott queue");
    ING_RUN(test_queue_basic);
    ING_RUN(test_queue_mpmc);

    return ing_test_end();
}
