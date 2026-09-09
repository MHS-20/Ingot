#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "reactor.h"

#define ING_MAX_EVENTS 64

/* ---------------------------------------------------------------------------
 * Registrations, indexed by file descriptor.
 *
 * An array indexed by fd rather than a hash table, because POSIX requires
 * open() and friends to return the *lowest* unused descriptor. Descriptors are
 * therefore dense and small: a process holding 10k connections has fds roughly
 * 0..10k, so the array is exactly as large as it needs to be and lookup is a
 * bounds check. This is what every production reactor does.
 * ------------------------------------------------------------------------ */
typedef struct {
    ing_io_fn cb;
    void     *arg;
    unsigned  events;
    /* Bumped on every deregistration. See the staleness check in dispatch:
     * this is what stops a closed-and-reopened fd from receiving the previous
     * occupant's events. */
    uint32_t  gen;
    bool      active;
} ing_fd_slot;

typedef struct {
    uint64_t      deadline_ms;
    uint64_t      id;
    ing_timer_fn  cb;
    void         *arg;
    bool          cancelled;
} ing_timer;

struct ing_reactor {
    int          epfd;
    bool         stop;

    ing_fd_slot *slots;
    size_t       nslots;      /* allocated */
    size_t       nactive;     /* registered */

    /* Binary min-heap of timers, ordered by deadline. */
    ing_timer   *timers;
    size_t       ntimers, cap_timers;
    uint64_t     next_timer_id;
};

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------ */

/* CLOCK_MONOTONIC, never CLOCK_REALTIME. A timer scheduled against wall-clock
 * time will fire early or late — potentially by hours — when NTP steps the
 * clock or an operator changes the timezone. Nothing about "fire 50ms from
 * now" should depend on what humans think the time is. */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---------------------------------------------------------------------------
 * Timer min-heap
 *
 * A heap and not a sorted list because the two operations that matter pull in
 * opposite directions: inserting a timer is O(log n) and finding the earliest
 * deadline — which the loop does on *every single turn* to compute its
 * epoll_wait timeout — is O(1) at the root.
 * ------------------------------------------------------------------------ */

static void heap_swap(ing_timer *a, ing_timer *b)
{
    ing_timer t = *a; *a = *b; *b = t;
}

static void heap_up(ing_reactor *r, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (r->timers[parent].deadline_ms <= r->timers[i].deadline_ms)
            break;
        heap_swap(&r->timers[parent], &r->timers[i]);
        i = parent;
    }
}

static void heap_down(ing_reactor *r, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, rt = 2 * i + 2, small = i;
        if (l < r->ntimers &&
            r->timers[l].deadline_ms < r->timers[small].deadline_ms)
            small = l;
        if (rt < r->ntimers &&
            r->timers[rt].deadline_ms < r->timers[small].deadline_ms)
            small = rt;
        if (small == i)
            return;
        heap_swap(&r->timers[i], &r->timers[small]);
        i = small;
    }
}

static int heap_push(ing_reactor *r, ing_timer t)
{
    if (r->ntimers == r->cap_timers) {
        size_t cap = r->cap_timers ? r->cap_timers * 2 : 16;
        ing_timer *p = realloc(r->timers, cap * sizeof *p);
        if (!p)
            return ING_ENOMEM;
        r->timers = p;
        r->cap_timers = cap;
    }
    r->timers[r->ntimers] = t;
    heap_up(r, r->ntimers);
    r->ntimers++;
    return ING_OK;
}

static void heap_pop(ing_reactor *r)
{
    r->ntimers--;
    if (r->ntimers > 0) {
        r->timers[0] = r->timers[r->ntimers];
        heap_down(r, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

ing_reactor *ing_reactor_create(void)
{
    ing_reactor *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;

    /* EPOLL_CLOEXEC so an exec() in a child does not inherit the loop's own
     * descriptor. Cheap, and the omission is a classic fd leak into
     * subprocesses. */
    r->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epfd < 0) {
        free(r);
        return NULL;
    }
    r->next_timer_id = 1;   /* 0 is reserved as "no timer" */
    return r;
}

void ing_reactor_destroy(ing_reactor *r)
{
    if (!r)
        return;
    /* Deliberately does NOT close registered fds: the reactor never owned
     * them. Closing descriptors it was merely watching would be a
     * spectacular action-at-a-distance bug in any program that keeps using
     * one after shutting its loop down. */
    close(r->epfd);
    free(r->slots);
    free(r->timers);
    free(r);
}

void ing_reactor_stop(ing_reactor *r) { r->stop = true; }

size_t ing_reactor_nfds(const ing_reactor *r) { return r->nactive; }

int ing_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return ING_EINVAL;
    /* Read-modify-write, never a bare F_SETFL of O_NONBLOCK: the latter
     * silently discards O_APPEND and every other flag already set. */
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return ING_EINVAL;
    return ING_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------ */

static int ensure_slot(ing_reactor *r, int fd)
{
    if ((size_t)fd < r->nslots)
        return ING_OK;

    size_t cap = r->nslots ? r->nslots : 16;
    while (cap <= (size_t)fd)
        cap *= 2;

    ing_fd_slot *p = realloc(r->slots, cap * sizeof *p);
    if (!p)
        return ING_ENOMEM;
    memset(p + r->nslots, 0, (cap - r->nslots) * sizeof *p);
    r->slots = p;
    r->nslots = cap;
    return ING_OK;
}

static uint32_t to_epoll(unsigned events)
{
    uint32_t e = 0;
    if (events & ING_READABLE) e |= EPOLLIN;
    if (events & ING_WRITABLE) e |= EPOLLOUT;
    return e;
}

/* The identity we hand the kernel is (generation, fd) packed into the 64-bit
 * cookie epoll gives us, not the bare fd and emphatically not a pointer to the
 * slot. A pointer would dangle the moment the slot array is realloc'd by an
 * unrelated registration; the generation is what makes a stale event
 * detectable rather than merely unlikely. */
static uint64_t pack_key(int fd, uint32_t gen)
{
    return ((uint64_t)gen << 32) | (uint32_t)fd;
}

int ing_reactor_add(ing_reactor *r, int fd, unsigned events,
                    ing_io_fn cb, void *arg)
{
    if (fd < 0 || !cb)
        return ING_EINVAL;

    int rc = ensure_slot(r, fd);
    if (rc != ING_OK)
        return rc;

    ing_fd_slot *s = &r->slots[fd];
    if (s->active)
        return ING_EEXIST;

    s->cb = cb;
    s->arg = arg;
    s->events = events;
    s->active = true;

    struct epoll_event ev = {
        .events = to_epoll(events),
        .data = { .u64 = pack_key(fd, s->gen) },
    };
    if (epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        s->active = false;
        return ING_EINVAL;
    }
    r->nactive++;
    return ING_OK;
}

int ing_reactor_mod(ing_reactor *r, int fd, unsigned events)
{
    if (fd < 0 || (size_t)fd >= r->nslots || !r->slots[fd].active)
        return ING_ENOTFOUND;

    ing_fd_slot *s = &r->slots[fd];
    struct epoll_event ev = {
        .events = to_epoll(events),
        .data = { .u64 = pack_key(fd, s->gen) },
    };
    if (epoll_ctl(r->epfd, EPOLL_CTL_MOD, fd, &ev) < 0)
        return ING_EINVAL;
    s->events = events;
    return ING_OK;
}

int ing_reactor_del(ing_reactor *r, int fd)
{
    if (fd < 0 || (size_t)fd >= r->nslots || !r->slots[fd].active)
        return ING_ENOTFOUND;

    ing_fd_slot *s = &r->slots[fd];
    epoll_ctl(r->epfd, EPOLL_CTL_DEL, fd, NULL);
    s->active = false;
    s->cb = NULL;
    s->arg = NULL;
    /* Invalidate every event for this registration that epoll_wait has
     * already copied into the current batch but that we have not dispatched
     * yet. Without this line the loop calls a callback the caller has just
     * deregistered — typically from another callback that has already freed
     * the connection object `arg` points at. That is the characteristic
     * use-after-free of every reactor, and it is not hypothetical: it
     * requires only two fds ready in the same turn, one of whose handlers
     * closes the other. */
    s->gen++;
    r->nactive--;
    return ING_OK;
}

/* ---------------------------------------------------------------------------
 * The loop
 * ------------------------------------------------------------------------ */

int ing_reactor_after(ing_reactor *r, uint64_t delay_ms,
                      ing_timer_fn cb, void *arg, uint64_t *id_out)
{
    if (!cb)
        return ING_EINVAL;

    ing_timer t = {
        .deadline_ms = now_ms() + delay_ms,
        .id = r->next_timer_id++,
        .cb = cb,
        .arg = arg,
        .cancelled = false,
    };
    int rc = heap_push(r, t);
    if (rc != ING_OK)
        return rc;
    if (id_out)
        *id_out = t.id;
    return ING_OK;
}

/* Lazy cancellation: the entry is marked and skipped when it surfaces, rather
 * than being located and removed. Removal would be O(n) to find it (a heap
 * has no index) and the bookkeeping to maintain one is rarely worth it, since
 * a cancelled timer costs only its slot until its deadline passes. The cost
 * to be aware of: a program that schedules and cancels a long-deadline timer
 * in a tight loop grows the heap without bound. */
int ing_reactor_cancel(ing_reactor *r, uint64_t id)
{
    for (size_t i = 0; i < r->ntimers; i++) {
        if (r->timers[i].id == id && !r->timers[i].cancelled) {
            r->timers[i].cancelled = true;
            return ING_OK;
        }
    }
    return ING_ENOTFOUND;
}

/* -1 when there is nothing pending: block indefinitely. */
static int next_timeout(ing_reactor *r, int requested)
{
    while (r->ntimers > 0 && r->timers[0].cancelled)
        heap_pop(r);

    if (r->ntimers == 0)
        return requested;

    uint64_t now = now_ms();
    uint64_t deadline = r->timers[0].deadline_ms;
    int until = (deadline <= now) ? 0 : (int)(deadline - now);

    if (requested < 0)
        return until;
    return until < requested ? until : requested;
}

static int dispatch_timers(ing_reactor *r)
{
    int fired = 0;
    uint64_t now = now_ms();

    while (r->ntimers > 0 && r->timers[0].deadline_ms <= now) {
        ing_timer t = r->timers[0];
        /* Pop BEFORE invoking. The callback is entitled to schedule new
         * timers, cancel others, or stop the loop, and every one of those
         * mutates the heap under us — so the heap must already be in a
         * consistent state that does not include this timer. */
        heap_pop(r);
        if (t.cancelled)
            continue;
        t.cb(r, t.arg);
        fired++;
        now = now_ms();
    }
    return fired;
}

int ing_reactor_poll(ing_reactor *r, int timeout_ms)
{
    struct epoll_event evs[ING_MAX_EVENTS];

    int wait_ms = next_timeout(r, timeout_ms);
    int n = epoll_wait(r->epfd, evs, ING_MAX_EVENTS, wait_ms);
    if (n < 0) {
        /* EINTR is not an error. A signal arriving during the wait is
         * routine, and treating it as a failure is how loops acquire
         * mysterious "it dies under strace / under the debugger / when the
         * window is resized" bugs. */
        if (errno == EINTR)
            n = 0;
        else
            return ING_EINVAL;
    }

    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        uint64_t key = evs[i].data.u64;
        int fd = (int)(uint32_t)key;
        uint32_t gen = (uint32_t)(key >> 32);

        if ((size_t)fd >= r->nslots)
            continue;
        ing_fd_slot *s = &r->slots[fd];

        /* The staleness check. Both halves matter: `active` catches an fd
         * deregistered earlier in this same batch, and the generation catches
         * the nastier case where it was deregistered *and re-registered* —
         * possibly for a completely different connection that happened to be
         * handed the same descriptor number. */
        if (!s->active || s->gen != gen)
            continue;

        unsigned events = 0;
        if (evs[i].events & EPOLLIN)  events |= ING_READABLE;
        if (evs[i].events & EPOLLOUT) events |= ING_WRITABLE;
        /* Folded together on purpose: a caller must handle both, and almost
         * no caller wants to distinguish "peer closed" from "socket broke"
         * at this layer — the recv() that follows will say which. */
        if (evs[i].events & (EPOLLERR | EPOLLHUP)) events |= ING_ERROR;

        s->cb(r, fd, events, s->arg);
        dispatched++;
    }

    dispatched += dispatch_timers(r);
    return dispatched;
}

int ing_reactor_run(ing_reactor *r)
{
    r->stop = false;
    while (!r->stop) {
        int rc = ing_reactor_poll(r, -1);
        if (rc < 0)
            return rc;
        /* Nothing registered and no timers pending means epoll_wait would
         * block forever with no possible waker. Returning is the only sane
         * behaviour; the alternative is a process that hangs at shutdown. */
        if (r->nactive == 0 && r->ntimers == 0)
            break;
    }
    return ING_OK;
}
