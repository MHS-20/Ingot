#include "common.h"

const char *ing_strstatus(int status)
{
    switch (status) {
    case ING_OK:        return "ok";
    case ING_ENOMEM:    return "out of memory";
    case ING_ENOTFOUND: return "not found";
    case ING_EEXIST:    return "already exists";
    case ING_EINVAL:    return "invalid argument";
    case ING_EAGAIN:    return "would block";
    case ING_ECLOSED:   return "closed";
    default:            return "unknown status";
    }
}

uint64_t ing_hash_bytes(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603u;      /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211u;                /* FNV prime */
    }
    return h;
}
