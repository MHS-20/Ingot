/* reactor.h — Day 10: a single-threaded async runtime, which is to say an
 * epoll loop and the bookkeeping that makes it usable.
 *
 * This is the last module and it is the one that reframes the other ten. Days
 * 6 through 9 answered "how do several threads share a structure safely". This
 * one answers a different question: what if you did not have several threads?
 * A reactor handles ten thousand concurrent connections on one thread with no
 * lock anywhere in it, because there is only ever one thing running. nginx,
 * redis, libuv (and therefore node), and the bottom of every async runtime in
 * every language are this file.
 *
 * The trade is explicit and worth stating plainly: you give up the ability to
 * block. A thread pool lets you write `read(fd)` and stop worrying; here, any
 * call that blocks stalls *every* connection, not just its own. That single
 * constraint is what forces callbacks, and then state machines, and then
 * async/await as a way to write state machines without noticing. Every
 * complication in modern async programming descends from it.
 *
 * WHAT THIS IS NOT: thread-safe. A reactor is owned by one thread and its
 * registration calls must be made from that thread (or from inside its own
 * callbacks, which is the same thing). Calling ing_reactor_add from another
 * thread is a data race. Real implementations solve this with a self-pipe or
 * an eventfd that wakes the loop and hands it work; that is left as an
 * extension.
 */
#ifndef ING_REACTOR_H
#define ING_REACTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "common.h"

typedef struct ing_reactor ing_reactor;

/* Reported interest and readiness. These are deliberately not the raw EPOLL*
 * constants: the point of a reactor is to be the only file that knows which
 * multiplexer is underneath, so that swapping epoll for kqueue or io_uring
 * touches one file and no callers. */
#define ING_READABLE (1u << 0)
#define ING_WRITABLE (1u << 1)
/* Never registered for, always delivered. Peer hangup and socket errors
 * arrive whether or not you asked, so a callback that ignores this flag will
 * spin: the fd stays permanently ready and epoll reports it forever. That
 * busy-loop is the single most common bug written against this API. */
#define ING_ERROR    (1u << 2)

typedef void (*ing_io_fn)(ing_reactor *r, int fd, unsigned events, void *arg);
typedef void (*ing_timer_fn)(ing_reactor *r, void *arg);

ing_reactor *ing_reactor_create(void);
void         ing_reactor_destroy(ing_reactor *r);

/* Registration. `events` is a mask of ING_READABLE / ING_WRITABLE.
 *
 * The fd must already be non-blocking — see ing_set_nonblocking, and see the
 * note there about why this is not optional. */
int ing_reactor_add(ing_reactor *r, int fd, unsigned events,
                    ing_io_fn cb, void *arg);
/* Change the interest mask without re-registering. Worth having as its own
 * call because it is the hot path of any writer: you register for WRITABLE
 * only while you have buffered output, and drop it the moment you are
 * flushed, because a socket is writable almost always and leaving the flag on
 * turns the loop into a spin. */
int ing_reactor_mod(ing_reactor *r, int fd, unsigned events);
/* Safe to call from inside a callback, including on the fd being dispatched.
 * See the .c file for what makes that safe. */
int ing_reactor_del(ing_reactor *r, int fd);

/* Timers. `delay_ms` is relative to now. Writes a cancellation handle to
 * `id_out` when it is non-NULL. Fires at most once. */
int ing_reactor_after(ing_reactor *r, uint64_t delay_ms,
                      ing_timer_fn cb, void *arg, uint64_t *id_out);
/* ING_OK, or ING_ENOTFOUND if it already fired or was already cancelled. */
int ing_reactor_cancel(ing_reactor *r, uint64_t id);

/* One turn of the loop: wait for readiness, dispatch I/O callbacks, then
 * dispatch every timer that has come due. `timeout_ms` of -1 means "block
 * until something happens", and is capped internally by the next timer's
 * deadline — which is the whole reason the loop can sleep indefinitely and
 * still be punctual. Returns the number of callbacks dispatched, or a
 * negative ing_status. */
int ing_reactor_poll(ing_reactor *r, int timeout_ms);
/* Turns until ing_reactor_stop. */
int ing_reactor_run(ing_reactor *r);
/* Safe from inside a callback; the current turn finishes first. */
void ing_reactor_stop(ing_reactor *r);

size_t ing_reactor_nfds(const ing_reactor *r);

/* Not a convenience. With edge-triggered epoll a blocking fd will eventually
 * park the entire loop inside a read() that has no more data to give, and
 * with level-triggered it will do the same the moment two callbacks race for
 * the same buffer. Every fd handed to a reactor is non-blocking, always. */
int ing_set_nonblocking(int fd);

#endif /* ING_REACTOR_H */
