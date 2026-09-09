/* lockfree.h — Day 8: producer-consumer without a lock, and the ABA problem.
 *
 * Three things live here, and the order is the lesson:
 *
 *   1. ing_lfstack        — the Treiber stack. Ten lines. Looks obviously
 *                           correct. Is obviously correct, right up until you
 *                           recycle a node.
 *   2. ing_lfstack_tagged — the same stack with a counter packed alongside
 *                           the pointer, which is the cheapest known defence
 *                           against the bug the first one has.
 *   3. ing_lfqueue        — the Michael-Scott queue: the real multi-producer,
 *                           multi-consumer FIFO that day 7 built with a mutex.
 *
 * WHAT "LOCK-FREE" BUYS AND WHAT IT COSTS
 *
 * Lock-free does not mean fast. It means *no thread's progress depends on
 * another thread being scheduled*: a thread descheduled in the middle of a
 * push cannot block anyone else, which is exactly the property a mutex fails
 * to provide and the reason a lock-free structure is safe to use from a
 * signal handler or across threads of wildly different priority. What it
 * costs is that you can no longer reason about "the operation" as a unit.
 * Every load is potentially stale, every CAS can fail and retry, and — the
 * part that surprises everyone — *you can no longer free anything*, because
 * you can never prove that no other thread is about to dereference the node
 * you are holding. See the reclamation note on ing_lfqueue_pop.
 *
 * MEMORY ORDER
 *
 * Every operation here is written with explicit orderings rather than the
 * seq_cst default, because the defaults hide precisely what this day is
 * about. The rule used throughout: a CAS that *publishes* a node other
 * threads will dereference is `release`; a load that *consumes* a node this
 * thread will dereference is `acquire`. That pairing is what makes the
 * initialising writes to a node visible to the thread that later pops it.
 * Drop the release and the receiving thread can legally observe a node whose
 * `next` field it has not yet seen written — on x86 you will never reproduce
 * it, and on any weakly-ordered machine (aarch64, POWER, RISC-V) it is a
 * Tuesday.
 */
#ifndef ING_LOCKFREE_H
#define ING_LOCKFREE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"

/* Intrusive, like everything else here: the caller owns the storage, so push
 * cannot allocate and therefore cannot fail. That matters more than usual in
 * a lock-free structure — an allocation inside a CAS retry loop would put a
 * lock (malloc's) inside the algorithm and forfeit the property the whole
 * exercise is for. */
typedef struct ing_lfnode {
    /* `next` must be atomic, and the reason is worth more than the two words
     * it costs.
     *
     * It looks thread-local at the moment it is written: push sets it before
     * the node is published, when no other thread can reach the node. But
     * once nodes are *recycled* — pop a node, push it back, which is the
     * normal life of a free list — a thread re-pushing node N writes N->next
     * at the same instant another thread, sitting in its pop window between
     * its load of head and its CAS, reads N->next. That is two threads
     * touching one location with one of them writing.
     *
     * The value the reader gets is stale, and that is fine: its CAS then
     * fails and it retries. What is not fine is that a non-atomic race is
     * undefined behaviour, not merely a wrong value — the compiler is
     * entitled to assume it cannot happen and optimise accordingly. Declaring
     * it atomic makes the stale read well-defined and costs nothing on x86-64
     * (a relaxed load is a plain mov). ThreadSanitizer reports the plain-
     * pointer version immediately; try it. */
    _Atomic(struct ing_lfnode *) next;
} ing_lfnode;

/* ---------------------------------------------------------------------------
 * 1. The Treiber stack, unprotected.
 * ------------------------------------------------------------------------ */

typedef struct {
    _Atomic(ing_lfnode *) head;
} ing_lfstack;

void        ing_lfstack_init(ing_lfstack *s);
void        ing_lfstack_push(ing_lfstack *s, ing_lfnode *node);
ing_lfnode *ing_lfstack_pop(ing_lfstack *s);

/* Test-only seam. ing_lfstack_pop calls this (when non-NULL) at the exact
 * point between reading `head` and attempting the CAS — the window in which
 * ABA happens. It exists so that the corruption can be demonstrated
 * *deterministically* in a test rather than hoped for under load, which is
 * the difference between understanding ABA and having read about it. Nothing
 * outside the test suite may set it. */
extern void (*ing_lfstack_pop_hook)(void *ctx);
extern void  *ing_lfstack_pop_hook_ctx;

/* ---------------------------------------------------------------------------
 * 2. The same stack, ABA-resistant via a tagged pointer.
 *
 * The fix is to make the CAS compare something that changes on every
 * modification, so that a head which has travelled A -> B -> A no longer
 * compares equal to the A that was read. A monotonically increasing counter
 * does that, but the counter and the pointer must be read and written
 * *together, atomically* — a separate counter word solves nothing.
 *
 * Two ways to get 64 bits of pointer and a counter into one atomic:
 *
 *   (a) A 128-bit CAS (`cmpxchg16b` on x86-64, `casp` on aarch64). Correct
 *       and general, but needs -mcx16, may fall back to a libatomic lock, and
 *       requires the whole struct to be 16-byte aligned.
 *   (b) Steal unused pointer bits. x86-64 and aarch64 addresses are canonical
 *       48-bit values sign-extended to 64, so for any pointer a userspace
 *       program can legally hold, the top 16 bits are redundant. Pack a
 *       16-bit tag there and a plain 64-bit CAS suffices.
 *
 * This uses (b) because it is the version you can read. Be clear about what
 * it assumes and where it breaks: 5-level paging (57-bit addresses), ARM's
 * top-byte-ignore, and any pointer-tagging sanitizer all invalidate it, and
 * a 16-bit tag *wraps after 65536 modifications* — so this makes ABA
 * improbable, not impossible. The only actual solutions to the underlying
 * problem are safe-reclamation schemes: hazard pointers, epoch-based
 * reclamation, or RCU.
 * ------------------------------------------------------------------------ */

#define ING_TAG_BITS 16
#define ING_PTR_BITS (64 - ING_TAG_BITS)
#define ING_PTR_MASK (((uint64_t)1 << ING_PTR_BITS) - 1)

typedef struct {
    _Atomic(uint64_t) head;   /* tag in the top 16 bits, pointer in the low 48 */
} ing_lfstack_tagged;

void        ing_lfstack_tagged_init(ing_lfstack_tagged *s);
void        ing_lfstack_tagged_push(ing_lfstack_tagged *s, ing_lfnode *node);
ing_lfnode *ing_lfstack_tagged_pop(ing_lfstack_tagged *s);
/* Exposed so a test can assert the tag really does advance. */
uint16_t    ing_lfstack_tagged_tag(const ing_lfstack_tagged *s);

extern void (*ing_lfstack_tagged_pop_hook)(void *ctx);
extern void  *ing_lfstack_tagged_pop_hook_ctx;

/* ---------------------------------------------------------------------------
 * 3. The Michael-Scott queue: lock-free MPMC FIFO.
 *
 * The structure day 7 built with a mutex and two condition variables, built
 * again with nothing but CAS. Two things make it work and neither is
 * obvious:
 *
 *   - A permanent *dummy* node sits at the head. It exists so that head and
 *     tail are never NULL and never equal-and-empty-in-a-different-way, which
 *     is what removes every special case from push and pop. The value you
 *     dequeue is never the head node's own value — it is the value of
 *     head->next, and the old head is then discarded. This is why the queue
 *     always holds one more node than it holds items.
 *   - Enqueue is *two* CASes (link the node, then advance tail) and therefore
 *     is not atomic. A thread can observe a tail that has fallen one node
 *     behind. The algorithm's answer is that any thread noticing a lagging
 *     tail *helps* by advancing it before proceeding. Lock-free algorithms
 *     are full of this: since you cannot wait for the slow thread, you finish
 *     its work for it.
 *
 * Unbounded: unlike day 7's bounded queue there is no back-pressure, which is
 * a real difference in character, not an omission. A lock-free bounded queue
 * needs a wholly different design (a fixed ring with sequence numbers).
 * ------------------------------------------------------------------------ */

typedef struct ing_lfqnode {
    _Atomic(struct ing_lfqnode *) next;
    void *value;
} ing_lfqnode;

typedef struct {
    _Atomic(ing_lfqnode *) head;
    _Atomic(ing_lfqnode *) tail;
} ing_lfqueue;

/* The caller supplies the dummy, as it supplies every other node: the queue
 * allocates nothing, ever, and therefore frees nothing, ever. That is a
 * deliberate and slightly awkward choice. The alternative — init mallocs the
 * dummy, teardown frees whatever is left — reads more nicely and is wrong,
 * because pop hands retired nodes back to the caller and the queue then has
 * no way to tell a node it allocated from one it was given. One owner for all
 * node memory removes the question. */
void ing_lfqueue_init(ing_lfqueue *q, ing_lfqnode *dummy);

/* Teardown iterator: returns each remaining node (including the dummy) one at
 * a time until NULL, so the caller can dispose of them however it allocated
 * them. NOT thread-safe — like pthread_mutex_destroy, every user must have
 * been quiesced first. */
ing_lfqnode *ing_lfqueue_drain(ing_lfqueue *q);

/* Takes ownership of `node`, which the caller must have allocated. Cannot
 * fail. */
void ing_lfqueue_push(ing_lfqueue *q, ing_lfqnode *node, void *value);

/* ING_OK with *out set, or ING_EAGAIN when empty.
 *
 * `retired` is the interesting parameter and the reason this signature is
 * uglier than day 7's. A successful pop leaves one node — the *previous*
 * dummy — no longer reachable from the queue, and it is handed back here
 * rather than freed internally, because the queue is not in a position to
 * know whether freeing it is safe. It is not: another thread may at this
 * instant be sitting between its own load of `head` and its CAS, holding a
 * pointer to exactly this node and about to dereference it. Freeing here
 * would be a use-after-free that no test on x86 will reliably reproduce.
 *
 * So the decision is pushed to the caller, which is what every real
 * implementation does under a nicer name. Your options are: never free
 * (fine for a fixed pool of nodes recycled forever, which is what the tests
 * do); free only after a quiescent point where no thread can be mid-pop; or
 * implement hazard pointers or epoch-based reclamation, which is extension
 * E-lockfree and is genuinely harder than the queue itself.
 *
 * Pass NULL for `retired` only if you intend to leak. */
int  ing_lfqueue_pop(ing_lfqueue *q, void **out, ing_lfqnode **retired);

#endif /* ING_LOCKFREE_H */
