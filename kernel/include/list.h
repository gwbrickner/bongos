/* A tiny intrusive doubly-linked circular list (ARCHITECTURE §6.2's Page.lru, and the pmm's free
 * lists / per-CPU cache lists). No allocation: every ListNode lives inside the struct it links --
 * the pmm embeds one in every `Page`, so linking a free page costs nothing beyond writing two
 * pointers into memory that's already reserved for it. A list head is just another ListNode, used
 * as an empty sentinel (its own next/prev point to itself when the list is empty). */
#ifndef KERNEL_LIST_H
#define KERNEL_LIST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ListNode {
    struct ListNode *next, *prev;
} ListNode;

/* Turns `node` into an empty list head, or detaches it as a well-formed singleton. No locks; pure.
 */
static inline void listInit(ListNode *node) {
    node->next = node;
    node->prev = node;
}

/* True if `head` (a list head, per listInit) has no linked nodes. No locks; pure. */
static inline bool listEmpty(const ListNode *head) {
    return head->next == head;
}

/* Inserts `node` immediately after `head` (so it becomes the new front of the list `head` roots).
 * `node` must not already be linked into any list. No locks; pure. */
static inline void listPushHead(ListNode *head, ListNode *node) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

/* Inserts `node` immediately before `head` (the new back of the list). No locks; pure. */
static inline void listPushTail(ListNode *head, ListNode *node) {
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

/* Unlinks `node` from whatever list it's in. `node` must currently be linked (calling this on an
 * already-detached node, or on a list head, is a caller bug). KERNEL_DEBUG builds NULL the node's
 * own links afterward so a stray double-remove or a use-after-unlink dereference faults instead of
 * silently corrupting whichever list happens to sit at the stale pointer. No locks; pure. */
static inline void listRemove(ListNode *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
#ifdef KERNEL_DEBUG
    node->next = NULL;
    node->prev = NULL;
#endif
}

/* Removes and returns the front node of the list `head` roots, or NULL if it's empty. No locks;
 * pure. */
static inline ListNode *listPopHead(ListNode *head) {
    if (listEmpty(head)) {
        return NULL;
    }
    ListNode *node = head->next;
    listRemove(node);
    return node;
}

/* Removes and returns the back node of the list `head` roots, or NULL if it's empty. No locks;
 * pure. */
static inline ListNode *listPopTail(ListNode *head) {
    if (listEmpty(head)) {
        return NULL;
    }
    ListNode *node = head->prev;
    listRemove(node);
    return node;
}

/* Recovers the containing struct of a ListNode member -- e.g. LIST_CONTAINER(node, Page, lru). */
#define LIST_CONTAINER(nodePtr, type, member)                                                     \
    ((type *)(void *)((char *)(nodePtr)-offsetof(type, member)))

#endif
