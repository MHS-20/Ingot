/* harness.h — a test runner small enough to read in one sitting.
 *
 * Each test group runs in its own forked child under its own alarm(). That is
 * not paranoia: half of this library is concurrency primitives, and the
 * characteristic failure of a broken condition variable or a broken lock-free
 * push is not a wrong answer, it is a *hang*. One global timeout would kill
 * the run at the first deadlock and tell you nothing about the other twenty
 * groups. A child that hangs, deadlocks or segfaults is reported as a failed
 * group and the run continues, so the suite always terminates.
 *
 * Counters live in a shared anonymous mapping so the parent can read what the
 * children wrote.
 */
#ifndef ING_TEST_HARNESS_H
#define ING_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifndef ING_TEST_TIMEOUT
#define ING_TEST_TIMEOUT 10       /* seconds per group */
#endif

typedef struct {
    int passed;
    int failed;
    int groups_failed;
} ing_test_counters;

static ing_test_counters *ing_tc;

static void ing_test_begin(void)
{
    ing_tc = mmap(NULL, sizeof *ing_tc, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ing_tc == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    memset(ing_tc, 0, sizeof *ing_tc);
}

#define ING_CHECK(cond, ...)                                                  \
    do {                                                                      \
        if (cond) {                                                           \
            ing_tc->passed++;                                                 \
        } else {                                                              \
            ing_tc->failed++;                                                 \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

/* A check a stub satisfies is not a pass. Negative assertions ("this bad
 * input must be rejected") are trivially true while the function under test
 * rejects everything, so they are gated on the corresponding positive case
 * working first, and reported as SKIPPED rather than passed. */
#define ING_SKIP_UNLESS(cond, why)                                            \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  SKIP %s (%s)\n", __func__, why);               \
            return;                                                           \
        }                                                                     \
    } while (0)

static void ing_run_group(void (*fn)(void), const char *name)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        alarm(ING_TEST_TIMEOUT);
        fn();
        fflush(NULL);
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        ing_tc->groups_failed++;
        fprintf(stderr, "  FAIL %s: killed by %s%s\n", name, strsignal(sig),
                sig == SIGALRM ? " (deadlock or infinite loop)" : "");
    } else if (WEXITSTATUS(status) != 0) {
        ing_tc->groups_failed++;
        fprintf(stderr, "  FAIL %s: exited %d\n", name, WEXITSTATUS(status));
    }
}

#define ING_RUN(fn) ing_run_group(fn, #fn)

#define ING_SECTION(title) fprintf(stderr, "-- %s\n", (title))

static int ing_test_end(void)
{
    fprintf(stderr, "\n%d passed, %d failed", ing_tc->passed, ing_tc->failed);
    if (ing_tc->groups_failed)
        fprintf(stderr, ", %d group(s) crashed or timed out", ing_tc->groups_failed);
    fputc('\n', stderr);
    return (ing_tc->failed || ing_tc->groups_failed) ? 1 : 0;
}

#endif /* ING_TEST_HARNESS_H */
