#include <stdlib.h>

#include "lockfree.h"

void (*ing_lfstack_pop_hook)(void *ctx);
void  *ing_lfstack_pop_hook_ctx;
void (*ing_lfstack_tagged_pop_hook)(void *ctx);
void  *ing_lfstack_tagged_pop_hook_ctx;

/* ===========================================================================
 * 1. Treiber stack
 * ======================================================================== */

void ing_lfstack_init(ing_lfstack *s)
{
    atomic_init(&s->head, NULL);
}

void ing_lfstack_push(ing_lfstack *s, ing_lfnode *node)
{
    /* Relaxed: this load only seeds the retry loop. If it is stale the CAS
     * below fails and hands us the current value, so no correctness rests on
     * it being fresh — and paying for an acquire here would be paying for a
     * guarantee we do not use. */
    ing_lfnode *old = atomic_load_explicit(&s->head, memory_order_relaxed);
    do {
        atomic_store_explicit(&node->next, old, memory_order_relaxed);
        /* Release on success: everything this thread wrote into `node`
         * (including the `next` above) must be visible to the thread that
         * later acquires it in pop. This is the pairing that makes the
         * structure work on a weakly-ordered machine.
         *
         * `_weak` because we are already in a loop: a weak CAS may fail
         * spuriously, and tolerating that is free here and lets the compiler
         * emit a bare LL/SC pair on architectures that have one. */
    } while (!atomic_compare_exchange_weak_explicit(
                 &s->head, &old, node,
                 memory_order_release, memory_order_relaxed));
}

ing_lfnode *ing_lfstack_pop(ing_lfstack *s)
{
    /* Acquire: we are about to dereference `old->next`, so we need to see the
     * writes the pushing thread made before it released it. */
    ing_lfnode *old = atomic_load_explicit(&s->head, memory_order_acquire);

    for (;;) {
        if (old == NULL)
            return NULL;

        /* THE WINDOW.
         *
         * Everything wrong with this function happens between the line above
         * and the CAS below. We are holding `old`, we have decided the new
         * head will be `old->next`, and we have not yet committed. If, in
         * this gap, another thread pops `old`, pops more, and then pushes
         * `old` back, our CAS still succeeds — the pointer compares equal —
         * and we install a `next` that was read from a version of the stack
         * that no longer exists. The stack is now corrupt: it points at a
         * node that has been removed, and typically loses every node pushed
         * in between.
         *
         * That is ABA. Note what it is *not*: it is not a torn read, not a
         * missing barrier, and not something a stronger memory order fixes.
         * The CAS did exactly what it promised. The bug is that pointer
         * equality was never the question we meant to ask — we meant to ask
         * "has the stack changed since I looked", and a raw pointer cannot
         * answer that. ing_lfstack_tagged makes the question answerable. */
        ing_lfnode *next = atomic_load_explicit(&old->next,
                                                memory_order_acquire);

        if (ing_lfstack_pop_hook)
            ing_lfstack_pop_hook(ing_lfstack_pop_hook_ctx);

        if (atomic_compare_exchange_weak_explicit(
                &s->head, &old, next,
                memory_order_acquire, memory_order_acquire))
            return old;
        /* CAS failure refreshed `old` for us; go round again. */
    }
}

/* ===========================================================================
 * 2. Tagged Treiber stack
 * ======================================================================== */

static inline uint64_t ing_pack(ing_lfnode *p, uint16_t tag)
{
    return ((uint64_t)(uintptr_t)p & ING_PTR_MASK)
         | ((uint64_t)tag << ING_PTR_BITS);
}

static inline ing_lfnode *ing_unpack_ptr(uint64_t v)
{
    return (ing_lfnode *)(uintptr_t)(v & ING_PTR_MASK);
}

static inline uint16_t ing_unpack_tag(uint64_t v)
{
    return (uint16_t)(v >> ING_PTR_BITS);
}

void ing_lfstack_tagged_init(ing_lfstack_tagged *s)
{
    atomic_init(&s->head, ing_pack(NULL, 0));
}

uint16_t ing_lfstack_tagged_tag(const ing_lfstack_tagged *s)
{
    return ing_unpack_tag(
        atomic_load_explicit(&s->head, memory_order_relaxed));
}

void ing_lfstack_tagged_push(ing_lfstack_tagged *s, ing_lfnode *node)
{
    uint64_t old = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint64_t new_val;
    do {
        atomic_store_explicit(&node->next, ing_unpack_ptr(old),
                              memory_order_relaxed);
        /* The tag increments on *every* successful modification, push as well
         * as pop. Incrementing only in pop would leave the A-B-A sequence
         * "pop A, pop B, push A" advancing the tag just twice on the popping
         * side while the pushing side contributes nothing — the defence has
         * to cover every transition or it covers none. */
        new_val = ing_pack(node, (uint16_t)(ing_unpack_tag(old) + 1));
    } while (!atomic_compare_exchange_weak_explicit(
                 &s->head, &old, new_val,
                 memory_order_release, memory_order_relaxed));
}

ing_lfnode *ing_lfstack_tagged_pop(ing_lfstack_tagged *s)
{
    uint64_t old = atomic_load_explicit(&s->head, memory_order_acquire);

    for (;;) {
        ing_lfnode *node = ing_unpack_ptr(old);
        if (node == NULL)
            return NULL;

        ing_lfnode *next = atomic_load_explicit(&node->next,
                                                memory_order_acquire);

        if (ing_lfstack_tagged_pop_hook)
            ing_lfstack_tagged_pop_hook(ing_lfstack_tagged_pop_hook_ctx);

        /* Same window as above, and the same interleaving can still occur.
         * The difference is that the CAS now compares pointer *and* tag, so
         * a head that left and came back carries a different tag and the CAS
         * fails — which sends us round the loop to re-read, which is the
         * correct outcome. */
        uint64_t new_val = ing_pack(next,
                                    (uint16_t)(ing_unpack_tag(old) + 1));
        if (atomic_compare_exchange_weak_explicit(
                &s->head, &old, new_val,
                memory_order_acquire, memory_order_acquire))
            return node;
    }
}

/* ===========================================================================
 * 3. Michael-Scott queue
 * ======================================================================== */

void ing_lfqueue_init(ing_lfqueue *q, ing_lfqnode *dummy)
{
    atomic_init(&dummy->next, NULL);
    dummy->value = NULL;
    atomic_init(&q->head, dummy);
    atomic_init(&q->tail, dummy);
}

ing_lfqnode *ing_lfqueue_drain(ing_lfqueue *q)
{
    ing_lfqnode *n = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (!n)
        return NULL;

    atomic_store_explicit(&q->head,
                          atomic_load_explicit(&n->next, memory_order_relaxed),
                          memory_order_relaxed);
    return n;
}

void ing_lfqueue_push(ing_lfqueue *q, ing_lfqnode *node, void *value)
{
    node->value = value;
    atomic_init(&node->next, NULL);

    for (;;) {
        ing_lfqnode *tail = atomic_load_explicit(&q->tail,
                                                 memory_order_acquire);
        ing_lfqnode *next = atomic_load_explicit(&tail->next,
                                                 memory_order_acquire);

        /* Re-read and confirm tail has not moved under us. Without this the
         * `next` we just read might belong to a node that is no longer the
         * tail, and we would be reasoning about a stale pair. */
        if (tail != atomic_load_explicit(&q->tail, memory_order_acquire))
            continue;

        if (next != NULL) {
            /* Tail is lagging: some other thread linked a node and has not
             * yet advanced the tail pointer. We cannot wait for it — it may
             * be descheduled for a millisecond — so we finish its job and
             * retry. This "helping" is the price of lock-freedom, and it is
             * why the CAS below is allowed to fail silently: if the original
             * thread got there first, its update is the one we wanted. */
            atomic_compare_exchange_weak_explicit(
                &q->tail, &tail, next,
                memory_order_release, memory_order_relaxed);
            continue;
        }

        /* Link the new node after the real tail. Release: a consumer that
         * acquires this pointer must see node->value. */
        if (atomic_compare_exchange_weak_explicit(
                &tail->next, &next, node,
                memory_order_release, memory_order_relaxed)) {
            /* The queue is already correct at this point — the node is
             * reachable. Advancing the tail is an optimisation, so its
             * failure is ignored: whoever beat us to it, or whoever notices
             * the lag next, will do it. */
            atomic_compare_exchange_strong_explicit(
                &q->tail, &tail, node,
                memory_order_release, memory_order_relaxed);
            return;
        }
    }
}

int ing_lfqueue_pop(ing_lfqueue *q, void **out, ing_lfqnode **retired)
{
    for (;;) {
        ing_lfqnode *head = atomic_load_explicit(&q->head,
                                                 memory_order_acquire);
        ing_lfqnode *tail = atomic_load_explicit(&q->tail,
                                                 memory_order_acquire);
        ing_lfqnode *next = atomic_load_explicit(&head->next,
                                                 memory_order_acquire);

        if (head != atomic_load_explicit(&q->head, memory_order_acquire))
            continue;

        if (head == tail) {
            /* Both point at the same node. Either the queue is genuinely
             * empty (next == NULL), or a producer has linked a node and not
             * yet advanced tail — in which case the queue is NOT empty and
             * returning ING_EAGAIN here would be a lost item. Help, retry. */
            if (next == NULL) {
                *out = NULL;
                if (retired)
                    *retired = NULL;
                return ING_EAGAIN;
            }
            atomic_compare_exchange_weak_explicit(
                &q->tail, &tail, next,
                memory_order_release, memory_order_relaxed);
            continue;
        }

        /* Read the value BEFORE the CAS. After the CAS succeeds, `next` is
         * the new dummy and another thread may already be popping it and
         * handing its node back to its caller for reuse — at which point
         * next->value is somebody else's memory. Reading it first is not a
         * style preference. */
        void *value = next->value;

        if (atomic_compare_exchange_weak_explicit(
                &q->head, &head, next,
                memory_order_release, memory_order_relaxed)) {
            *out = value;
            /* `head` is the old dummy: unreachable from the queue now, and
             * the caller's problem. See the header for why we do not free
             * it. */
            if (retired)
                *retired = head;
            return ING_OK;
        }
    }
}
