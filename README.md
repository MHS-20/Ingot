# Ingot

Eleven data structures and concurrency primitives in C11, written to be read.

Every one of these exists inside libraries you already use — Redis's sorted
set, Java's `ThreadPoolExecutor`, nginx's event loop, the free list under an
allocator. They are hidden there, behind fifteen years of configurability,
portability shims and platform `#ifdef`s. Here each one is on its own, in one
file, with the reasoning written down next to it.

This is a reference implementation, not a library to depend on. 
Build each of these yourself first, then read this to compare 
against a version that had the luxury of hindsight.

## The eleven

| # | Module | What it is | The idea it exists for |
|---|---|---|---|
| 1 | `htab` | Chained hash table, intrusive | The link lives *in* the element, so insert allocates nothing and cannot fail |
| 2 | `omap` | Open addressing + incremental resize | Amortising the rehash so no single call is O(n) |
| 3 | `skiplist` | Probabilistic ordered structure | A coin flip replacing rebalancing — and `span`, which buys O(log n) rank |
| 4 | `zset` | Sorted set = skip list + hash table | One entry in two structures at once; O(1) score *and* O(log n) rank |
| 5 | `lru` | O(1) cache | Hash table for lookup, intrusive list for recency; eviction that cannot fail |
| 6 | `threadpool` | Fixed workers over a task queue | Counting *in-flight* work, not queued work |
| 7 | `bqueue` | Bounded producer-consumer | Two condition variables, and why one deadlocks |
| 8 | `lockfree` | Treiber stack, tagged stack, MS queue | ABA — demonstrated deterministically, then defeated |
| 9 | `rwlock` | Reader-writer lock, by hand | Which class you choose to starve |
| 10 | `reactor` | Single-threaded epoll loop | Ten thousand connections, no locks, and no blocking allowed |
| 11 | `ratelimit` | Token bucket + sliding window | Computing state lazily from a clock instead of running a timer |

They are ordered as a curriculum, not as a dependency graph. The real
dependencies are few: `zset` needs `skiplist` and `htab`, `lru` needs `htab`
and `list.h`, and nothing else needs anything.

## Conventions

- **C11**, `gnu11` for the POSIX surface. Linux — `epoll` in module 10 is not
  portable and no attempt is made to pretend otherwise.
- **`ing_` on everything.** Header guards are `ING_<NAME>_H`.
- **Status codes, not `errno`.** Fallible calls return an `int` from
  `enum ing_status`; `rc < 0` is always the error test. Calls that cannot fail
  return `void`, and several of them cannot fail *by construction* — which is
  usually the most interesting fact about them.
- **Intrusive where it matters.** `htab`, `list`, `lockfree` and everything
  built on them embed their link cells in the caller's struct. That is what
  lets a `zset` entry be in two structures at once, and what lets an LRU
  eviction be infallible.
- **Comments say why.** If a comment explains what a line does, it is a bad
  comment and should be deleted. The ones here are about what breaks if you do
  it the other way.

## Building

```bash
make          # build every test binary into build/
make test     # run them all
make memcheck # valgrind, for modules 1-5
make tsan     # ThreadSanitizer, for modules 6-11
make clean
```

There is no library artifact and no `main`. The tests are the program.

### Which checker for which module

This matters more than it sounds, and using the wrong one gives you false
confidence.

- **Modules 1-5 are single-threaded**: use `make memcheck`. Valgrind finds the
  leaks, the invalid frees and the reads past the end of a bucket array.
- **Modules 6-11 are concurrent**: use `make tsan`. Valgrind serialises
  threads and will therefore *never* show you a data race; ThreadSanitizer
  exists for exactly this and it is not optional. A concrete example from this
  repo: `ing_lfnode.next` looks like plain thread-local memory at the moment
  it is written, and is a genuine race once nodes are recycled. Valgrind is
  silent. TSan reports it on the first run. The comment on that field explains
  the reasoning.

Each test group runs in its own forked child under its own `alarm()`, because
the characteristic failure of a broken lock is a hang rather than a wrong
answer, and a single global timeout would kill the run at the first deadlock
and tell you nothing about the other twenty groups.

## Reading order

If you are here to study rather than to build, the shortest path to the
interesting parts:

1. `src/list.h` — twenty lines, and the argument for intrusive structures that
   the rest of the repo leans on.
2. `src/zset.h` — the header comment, for why two structures beat one.
3. `src/lockfree.c` — `ing_lfstack_pop`, and the comment on the window between
   the load and the CAS.
4. `src/reactor.c` — `ing_reactor_del` and the staleness check in dispatch:
   thirty lines that are the difference between a working event loop and one
   that intermittently calls a callback on freed memory.
