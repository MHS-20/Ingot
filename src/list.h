/* list.h — a circular doubly-linked list with a sentinel head, intrusive.
 *
 * "Intrusive" means the link cell lives *inside* the element rather than
 * pointing at it. That buys two things this library depends on:
 *
 *   - enqueue and dequeue allocate nothing, so they cannot fail, so they need
 *     no error path (see day 5's LRU: an eviction on a full cache must not be
 *     able to fail with ENOMEM);
 *   - an element can be in several structures at once, because it can carry
 *     several link cells. Day 5's cache entry is simultaneously in a hash
 *     table (by key) and in this list (by recency), and it is one allocation.
 *
 * The head is a sentinel: it is a link cell that belongs to no element, and
 * it is always present. That is what removes every NULL check from insert and
 * remove — there is no such thing as an empty region of this list, only a
 * list whose sole member is the sentinel pointing at itself.
 */
#ifndef ING_LIST_H
#define ING_LIST_H

#include <stddef.h>

typedef struct ing_link {
    struct ing_link *next, *prev;
} ing_link;

/* Also correct on a bare node: an unlinked node points at itself, which is
 * what makes ing_list_remove idempotent. */
static inline void ing_list_init(ing_link *head)
{
    head->next = head;
    head->prev = head;
}

static inline int ing_list_empty(const ing_link *head)
{
    return head->next == head;
}

/* Splice `node` between the two given neighbours. Every insert is this. */
static inline void ing_list_link(ing_link *prev, ing_link *node, ing_link *next)
{
    node->prev = prev;
    node->next = next;
    prev->next = node;
    next->prev = node;
}

static inline void ing_list_push_front(ing_link *head, ing_link *node)
{
    ing_list_link(head, node, head->next);
}

static inline void ing_list_push_back(ing_link *head, ing_link *node)
{
    ing_list_link(head->prev, node, head);
}

/* Idempotent: removing a node twice, or removing a freshly ing_list_init'd
 * node that was never inserted, is a no-op rather than corruption. The LRU
 * relies on this — a `get` on an entry already at the front does a remove and
 * a push_front without checking which case it is in. */
static inline void ing_list_remove(ing_link *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

/* Move an already-linked node to the front. The whole of "recently used". */
static inline void ing_list_move_front(ing_link *head, ing_link *node)
{
    ing_list_remove(node);
    ing_list_push_front(head, node);
}

#define ing_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Iterate over enclosing structs. Removing the *current* element mid-loop is
 * safe only if you break immediately afterwards, because the loop's step
 * reads cursor->member.next out of a node that remove() has just pointed back
 * at itself — giving you an infinite loop rather than a crash. Use the _safe
 * form when you intend to remove as you go. */
#define ing_list_for_each(cursor, head, type, member)                        \
    for (type *cursor = ing_container_of((head)->next, type, member);        \
         &cursor->member != (head);                                          \
         cursor = ing_container_of(cursor->member.next, type, member))

#define ing_list_for_each_safe(cursor, tmp, head, type, member)              \
    for (type *cursor = ing_container_of((head)->next, type, member),        \
              *tmp = ing_container_of(cursor->member.next, type, member);    \
         &cursor->member != (head);                                          \
         cursor = tmp,                                                       \
         tmp = ing_container_of(tmp->member.next, type, member))

#endif /* ING_LIST_H */
