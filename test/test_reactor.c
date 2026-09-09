#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "harness.h"
#include "reactor.h"

/* ===========================================================================
 * Readiness
 * ======================================================================== */

typedef struct {
    int      calls;
    unsigned last_events;
    char     buf[64];
    ssize_t  nread;
} io_record;

static void record_read(ing_reactor *r, int fd, unsigned events, void *arg)
{
    io_record *rec = arg;
    rec->calls++;
    rec->last_events = events;
    rec->nread = read(fd, rec->buf, sizeof rec->buf - 1);
    if (rec->nread > 0)
        rec->buf[rec->nread] = '\0';
    ing_reactor_stop(r);
}

static void test_readable(void)
{
    int sv[2];
    ING_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    ing_reactor *r = ing_reactor_create();
    ING_CHECK(r != NULL, "reactor_create must succeed");
    if (!r) return;

    ING_CHECK(ing_set_nonblocking(sv[0]) == ING_OK, "set_nonblocking");

    io_record rec = {0};
    ING_CHECK(ing_reactor_add(r, sv[0], ING_READABLE, record_read, &rec) == ING_OK,
              "add must succeed");
    ING_CHECK(ing_reactor_nfds(r) == 1, "one fd registered");
    ING_CHECK(ing_reactor_add(r, sv[0], ING_READABLE, record_read, &rec) == ING_EEXIST,
              "re-adding the same fd must be ING_EEXIST");

    write(sv[1], "hello", 5);
    ing_reactor_run(r);

    ING_CHECK(rec.calls == 1, "the callback must fire exactly once, fired %d", rec.calls);
    ING_CHECK(rec.last_events & ING_READABLE, "the event must be ING_READABLE");
    ING_CHECK(rec.nread == 5 && memcmp(rec.buf, "hello", 5) == 0,
              "the callback must be able to read the 5 bytes written, got %zd '%s'",
              rec.nread, rec.buf);

    ING_CHECK(ing_reactor_del(r, sv[0]) == ING_OK, "del must succeed");
    ING_CHECK(ing_reactor_nfds(r) == 0, "no fds left");
    ING_CHECK(ing_reactor_del(r, sv[0]) == ING_ENOTFOUND,
              "deleting twice must be ING_ENOTFOUND");

    ing_reactor_destroy(r);
    close(sv[0]);
    close(sv[1]);
}

static int writable_calls;

static void on_writable(ing_reactor *r, int fd, unsigned events, void *arg)
{
    (void)fd; (void)arg;
    writable_calls++;
    ING_CHECK(events & ING_WRITABLE, "expected a writable event");
    ing_reactor_stop(r);
}

static void test_writable_and_mod(void)
{
    int sv[2];
    ING_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }
    ing_set_nonblocking(sv[0]);

    writable_calls = 0;
    /* Registered for READABLE only, and nothing is readable, so a poll with a
     * short timeout must dispatch nothing. */
    ing_reactor_add(r, sv[0], ING_READABLE, on_writable, NULL);
    ING_CHECK(ing_reactor_poll(r, 20) == 0,
              "a poll with no readiness and no timers dispatches nothing");

    /* An empty socket is essentially always writable, so switching the
     * interest mask must make the very next turn fire. This is the mechanism
     * behind "only register for WRITABLE while you have buffered output". */
    ING_CHECK(ing_reactor_mod(r, sv[0], ING_WRITABLE) == ING_OK, "mod must succeed");
    ing_reactor_run(r);
    ING_CHECK(writable_calls == 1, "the writable callback must fire, fired %d",
              writable_calls);

    ING_CHECK(ing_reactor_mod(r, 9999, ING_WRITABLE) == ING_ENOTFOUND,
              "mod on an unregistered fd must be ING_ENOTFOUND");

    ing_reactor_destroy(r);
    close(sv[0]);
    close(sv[1]);
}

/* ===========================================================================
 * The characteristic reactor bug: one callback deregisters another fd whose
 * event is already sitting in the batch epoll_wait just returned.
 * ======================================================================== */

static int  victim_fd;
static int  victim_calls;
static int  killer_calls;

static void killer_cb(ing_reactor *r, int fd, unsigned events, void *arg)
{
    (void)events; (void)arg;
    char b[16];
    read(fd, b, sizeof b);
    killer_calls++;
    /* Stands in for the real-world case: a control connection whose handler
     * tears down a peer connection, freeing the object that peer's callback
     * argument points at. */
    ing_reactor_del(r, victim_fd);
}

static void victim_cb(ing_reactor *r, int fd, unsigned events, void *arg)
{
    (void)r; (void)fd; (void)events; (void)arg;
    victim_calls++;
}

static void test_del_during_dispatch(void)
{
    int a[2], b[2];
    ING_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0, "socketpair a");
    ING_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0, "socketpair b");

    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }

    ing_set_nonblocking(a[0]);
    ing_set_nonblocking(b[0]);
    killer_calls = victim_calls = 0;
    victim_fd = b[0];

    ing_reactor_add(r, a[0], ING_READABLE, killer_cb, NULL);
    ing_reactor_add(r, b[0], ING_READABLE, victim_cb, NULL);

    /* Make BOTH ready before polling, so epoll_wait returns both in a single
     * batch and the deregistration necessarily happens while the victim's
     * event is already in flight. */
    write(a[1], "x", 1);
    write(b[1], "y", 1);

    ing_reactor_poll(r, 100);

    ING_CHECK(killer_calls == 1, "the killer callback must run, ran %d", killer_calls);
    ING_CHECK(victim_calls == 0,
              "a callback deregistered earlier in the same batch must NOT be "
              "invoked — this is the use-after-free the generation counter "
              "exists to prevent; it ran %d times", victim_calls);

    ing_reactor_destroy(r);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

/* ===========================================================================
 * Timers
 * ======================================================================== */

static int   timer_order[8];
static int   timer_n;
static void *timer_last_arg;

static void note_timer(ing_reactor *r, void *arg)
{
    (void)r;
    if (timer_n < 8)
        timer_order[timer_n++] = (int)(intptr_t)arg;
    timer_last_arg = arg;
}

static void test_timers_fire_in_deadline_order(void)
{
    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }

    timer_n = 0;
    /* Scheduled out of order on purpose: the heap, not the insertion order,
     * decides. */
    ing_reactor_after(r, 60, note_timer, (void *)(intptr_t)3, NULL);
    ing_reactor_after(r, 10, note_timer, (void *)(intptr_t)1, NULL);
    ing_reactor_after(r, 35, note_timer, (void *)(intptr_t)2, NULL);

    ing_reactor_run(r);   /* returns once no fds and no timers remain */

    ING_CHECK(timer_n == 3, "all three timers must fire, %d did", timer_n);
    ING_CHECK(timer_n == 3 && timer_order[0] == 1 && timer_order[1] == 2 &&
              timer_order[2] == 3,
              "timers must fire in deadline order, got %d,%d,%d",
              timer_order[0], timer_order[1], timer_order[2]);

    ing_reactor_destroy(r);
}

static void test_timer_cancel(void)
{
    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }

    uint64_t keep = 0, drop = 0;
    timer_n = 0;
    ing_reactor_after(r, 10, note_timer, (void *)(intptr_t)1, &drop);
    ing_reactor_after(r, 25, note_timer, (void *)(intptr_t)2, &keep);

    ING_CHECK(drop != 0 && keep != 0 && drop != keep,
              "every timer must get a distinct non-zero id");
    ING_CHECK(ing_reactor_cancel(r, drop) == ING_OK, "cancel must succeed");
    ING_CHECK(ing_reactor_cancel(r, drop) == ING_ENOTFOUND,
              "cancelling twice must be ING_ENOTFOUND");
    ING_CHECK(ing_reactor_cancel(r, 987654) == ING_ENOTFOUND,
              "cancelling an unknown id must be ING_ENOTFOUND");

    ing_reactor_run(r);

    ING_CHECK(timer_n == 1, "only the surviving timer fires, %d fired", timer_n);
    ING_CHECK(timer_n == 1 && timer_order[0] == 2,
              "and it must be the one that was not cancelled");

    ing_reactor_destroy(r);
}

static void rearm_cb(ing_reactor *r, void *arg)
{
    int *countdown = arg;
    if (--(*countdown) > 0)
        ing_reactor_after(r, 1, rearm_cb, countdown, NULL);
}

static void test_timer_can_reschedule_itself(void)
{
    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }

    /* A timer callback mutating the heap it was dispatched from is the
     * ordinary case, not an edge case — it is how every periodic timer is
     * built. */
    int countdown = 5;
    ing_reactor_after(r, 1, rearm_cb, &countdown, NULL);
    ing_reactor_run(r);

    ING_CHECK(countdown == 0,
              "a self-rescheduling timer must run to completion, %d left",
              countdown);
    ing_reactor_destroy(r);
}

static void test_timeout_is_bounded_by_next_deadline(void)
{
    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }

    timer_n = 0;
    ing_reactor_after(r, 20, note_timer, (void *)(intptr_t)7, NULL);

    /* Asked to block indefinitely with no fds registered at all. It must
     * still return promptly, because the pending timer caps the wait — that
     * capping is the only reason a loop can sleep forever and still be
     * punctual. Without it this call hangs and the harness kills the group. */
    int n = ing_reactor_poll(r, -1);
    ING_CHECK(n == 1, "the due timer must be dispatched, dispatched %d", n);
    ING_CHECK(timer_n == 1, "and its callback must have run");

    ing_reactor_destroy(r);
}

/* ===========================================================================
 * End to end: an echo server on the loop
 * ======================================================================== */

typedef struct { int done; char got[128]; size_t len; } echo_state;

static void echo_cb(ing_reactor *r, int fd, unsigned events, void *arg)
{
    echo_state *st = arg;
    if (events & ING_ERROR) { ing_reactor_del(r, fd); ing_reactor_stop(r); return; }

    char buf[64];
    /* Read until EAGAIN: the fd may hold more than one message, and a
     * level-triggered loop that reads once per event still works but a
     * single read of a partial buffer is the habit that breaks the moment
     * anyone switches to edge-triggered. */
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        write(fd, buf, (size_t)n);            /* echo it back */
        if (st->len + (size_t)n < sizeof st->got) {
            memcpy(st->got + st->len, buf, (size_t)n);
            st->len += (size_t)n;
        }
    }
    st->done = 1;
    ing_reactor_stop(r);
}

static void test_echo_roundtrip(void)
{
    int sv[2];
    ING_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    ing_reactor *r = ing_reactor_create();
    if (!r) { ING_CHECK(0, "reactor_create"); return; }
    ing_set_nonblocking(sv[0]);

    echo_state st = {0};
    ing_reactor_add(r, sv[0], ING_READABLE, echo_cb, &st);

    const char *msg = "the reactor pattern";
    write(sv[1], msg, strlen(msg));
    ing_reactor_run(r);

    ING_CHECK(st.done, "the echo handler must have run");
    ING_CHECK(st.len == strlen(msg) && memcmp(st.got, msg, st.len) == 0,
              "the server must have received the message intact");

    char back[128];
    ssize_t n = read(sv[1], back, sizeof back);
    ING_CHECK(n == (ssize_t)strlen(msg) && memcmp(back, msg, (size_t)n) == 0,
              "and echoed it back verbatim, got %zd bytes", n);

    ing_reactor_destroy(r);
    close(sv[0]);
    close(sv[1]);
}

static void test_nonblocking_helper(void)
{
    int sv[2];
    ING_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    int before = fcntl(sv[0], F_GETFL, 0);
    ING_CHECK((before & O_NONBLOCK) == 0, "a fresh socket is blocking");
    ING_CHECK(ing_set_nonblocking(sv[0]) == ING_OK, "set_nonblocking succeeds");
    ING_CHECK((fcntl(sv[0], F_GETFL, 0) & O_NONBLOCK) != 0, "and it took effect");
    ING_CHECK(ing_set_nonblocking(-1) == ING_EINVAL, "a bad fd is ING_EINVAL");

    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    ing_test_begin();

    ING_SECTION("readiness");
    ING_RUN(test_readable);
    ING_RUN(test_writable_and_mod);
    ING_RUN(test_nonblocking_helper);

    ING_SECTION("deregistration during dispatch");
    ING_RUN(test_del_during_dispatch);

    ING_SECTION("timers");
    ING_RUN(test_timers_fire_in_deadline_order);
    ING_RUN(test_timer_cancel);
    ING_RUN(test_timer_can_reschedule_itself);
    ING_RUN(test_timeout_is_bounded_by_next_deadline);

    ING_SECTION("end to end");
    ING_RUN(test_echo_roundtrip);

    return ing_test_end();
}
