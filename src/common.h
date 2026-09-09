/* common.h — status codes and the one hash function everything shares.
 *
 * Ingot is a library of primitives, not a framework: nothing here allocates on
 * your behalf without saying so, and no module depends on any other except
 * where the header says it does (zset needs skiplist and htab; lru needs
 * htab and list).
 */
#ifndef ING_COMMON_H
#define ING_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* Every fallible call returns one of these. Zero is success, negatives are
 * errors, so `if (rc < 0)` is always the right test and `rc > 0` is free for
 * a module that wants to return a count. */
enum ing_status {
    ING_OK        =  0,
    ING_ENOMEM    = -1,  /* allocation failed */
    ING_ENOTFOUND = -2,  /* key absent */
    ING_EEXIST    = -3,  /* key present when it had to be absent */
    ING_EINVAL    = -4,  /* caller error: bad argument, bad state */
    ING_EAGAIN    = -5,  /* would block; try again later */
    ING_ECLOSED   = -6,  /* the queue/limiter has been shut down */
};

const char *ing_strstatus(int status);

/* FNV-1a, 64-bit. Not cryptographic and not seeded, which matters: an
 * attacker who can choose keys can force every one of them into the same
 * bucket and turn an O(1) table into an O(n) list. Real servers use a seeded
 * SipHash for exactly this reason. Provided here so that days 1 and 2 are
 * about the table rather than about the mixing function. */
uint64_t ing_hash_bytes(const void *data, size_t len);

#endif /* ING_COMMON_H */
