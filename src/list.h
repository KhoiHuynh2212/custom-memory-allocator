#pragma once

#include <stddef.h>
#include <assert.h>
#include <stdbool.h>

/*
 * struct list - intrusive circular doubly linked list node

 * Embed this inside any struct you want to link. The list has no separate
 * head type — one node is designated the anchor (sentinel). An empty list
 * is an anchor whose next and prev both point back to itself.
*/
typedef struct list
{
    struct list *next;
    struct list *prev;
} list;

/*
 * LIST_INIT - static initializer for an anchor node
 *
 * Sets next and prev to point at the node itself, making it a valid
 * empty list. Use this for static/global declarations:
 */
#define LIST_INIT(_var) {.next = &(_var), .prev = &(_var)} // initialize struct list

/*
 * list_init - runtime initializer for an anchor node
 *
 * Same effect as LIST_INIT but works on a pointer, for nodes allocated
 * at runtime. Returns @what so it can be used in an expression.
 */
static inline list *list_init(list *what)
{
    *what = (list)LIST_INIT(*what);
    return what;
}

/*
 * list_entry_offset - recover a parent struct pointer from an embedded list*
 *
 * Subtracts @offset bytes from the list node pointer to get back to the
 * start of the containing struct. This is the container_of pattern.
 *
 * Returns NULL if @what is NULL.
 *
 * Don't call this directly — use the list_entry() macro instead.
 */
static inline void *container_of(list *what, size_t offset)
{
    if (what)
    {
        // cast list* - > void* then -> uintptr_t (becomes an integer)
        return (void *)(((char *)(void *)what) - offset);
    }
    return NULL;
}

/**
 * list_entry() - get parent container of list entry
 * @_what:              list entry, or NULL
 * @_t:                 type of parent container
 * @_m:                 member name of list entry in @_t
 *
 * If the list entry @_what is embedded into a surrounding structure, this will
 * turn the list entry pointer @_what into a pointer to the parent container
 * (using offsetof(3), or sometimes called container_of(3)).
 *
 * If @_what is NULL, this will also return NULL.
 *
 * Return: Pointer to parent container, or NULL.
 */
#define list_entry(_what, _t, _m) \
    ((_t *)container_of((_what), offsetof(_t, _m)))

/*
 * list_is_linked - check whether a node is currently in a list
 *
 * A node is unlinked (isolated) when both next and prev point to itself —
 * the state left by list_init() and list_unlink(). Returns true if either
 * pointer points elsewhere, meaning the node has neighbors.
 */
static inline bool list_is_linked(const list *what)
{
    return what && (what->next != what || what->prev != what);
}

/*
 * list_is_empty - check whether an anchor node's list is empty
 *
 * An anchor with no members has next == prev == itself (the LIST_INIT state).
 * Equivalent to !list_is_linked(). Callers pass the anchor, not a data node.
 */

static inline bool list_is_empty(const list *what)
{
    return !list_is_linked(what); // only anchor node is empty
}

/*
 * list_add_before - insert a node immediately before another node
 *
 * @pos: existing node in a list
 * @node: detached node to insert
 *
 * Inserts @node directly before @pos.
 *
 * Preconditions:
 *   - @pos must be linked into a valid list.
 *   - @node must not currently be linked into any list.
 *
 * Postconditions:
 *   - @node becomes linked.
 *   - @node occupies the position immediately before @pos.
 *
 * Primitive list operation.
 */
static inline void list_add_before(list *pos, list *node)
{
#ifdef DEBUG
    assert(pos);
    assert(pos->next);
    assert(pos->prev);
    assert(pos->prev->next == pos);  /* pos must be in a valid list */

    assert(node);
    assert(node->next);
    assert(node->prev);
    assert(node->next == node && node->prev == node); /* node must be detached */
#endif

    pos->prev->next = node;
    node->prev = pos->prev;
    pos->prev = node;
    node->next = pos;
}

static inline void list_add_tail(list *head, list *node)
{
    list_add_before(head, node);
}

/*
 * list_add_after - insert a node immediately after another node
 *
 * @pos: existing node in a list
 * @node: detached node to insert
 *
 * Inserts @node directly after @pos.
 *
 * Preconditions:
 *   - @pos must be linked into a valid list.
 *   - @node must not currently be linked into any list.
 *
 * Postconditions:
 *   - @node becomes linked.
 *   - @node occupies the position immediately after @pos.
 *
 * Primitive list operation.
 */
static inline void list_add_after(list *pos, list *node)
{
#ifdef DEBUG
    assert(pos);
    assert(pos->next);
    assert(pos->prev);
    assert(pos->prev->next == pos);  /* pos must be in a valid list */

    assert(node);
    assert(node->next);
    assert(node->prev);
    assert(node->next == node && node->prev == node); /* node must be detached */
#endif

    pos->next->prev = node;
    node->next = pos->next;
    node->prev = pos;
    pos->next = node;
}

static inline void list_push_front(list *head, list *node)
{
    list_add_after(head, node);
}

/*
 * list_add_between - insert a node between two adjacent nodes
 *
 * @left: node immediately before insertion point
 * @right: node immediately after insertion point
 * @node: detached node to insert
 *
 * Inserts @node between @left and @right.
 *
 * Preconditions:
 *   - @left->next == @right
 *   - @right->prev == @left
 *   - @node must not currently be linked into any list.
 *
 * Postconditions:
 *   - @left <-> @node <-> @right
 *
 * Lowest-level insertion primitive.
 */
static inline void list_add_between(list *left, list *right, list *node)
{
#ifdef DEBUG
    assert(left);
    assert(right);
    assert(left->next == right);   /* left and right must be adjacent */
    assert(right->prev == left);

    assert(node);
    assert(node->next == node && node->prev == node); /* node must be detached */
#endif

    node->next = right;
    node->prev = left;
    left->next = node;
    right->prev = node;
}

/*
 * list_unlink_stale - remove a node without reinitializing it
 *
 * @node: linked node to remove
 *
 * Removes @node from its current list.
 *
 * Preconditions:
 *   - @node must currently be linked into a valid list.
 *
 * Postconditions:
 *   - Neighboring nodes are reconnected.
 *   - @node retains its old next/prev pointers.
 *
 * Internal primitive used when caller manages node state manually.
 */
static inline void list_unlink_stale(list *node)
{
#ifdef DEBUG
    assert(node->next->prev == node);
    assert(node->prev->next == node);
#endif

    node->prev->next = node->next;
    node->next->prev = node->prev;
}

/*
 * list_unlink - remove a node and reinitialize it
 *
 * @node: linked node to remove
 *
 * Removes @node from its current list and returns it to the
 * detached state produced by list_init().
 *
 * Preconditions:
 *   - None. Safe to call on detached nodes.
 *
 * Postconditions:
 *   - @node is self-linked.
 *   - list_is_linked(@node) returns false.
 *
 * Preferred public removal operation.
 */
static inline void list_unlink(list *node)
{

    if (list_is_linked(node))
    {
        node->prev->next = node->next;
        node->next->prev = node->prev;

        list_init(node);
    }
}

/*
 * list_pop_front - remove and return the first node
 *
 * @head: list anchor
 *
 * Removes the node immediately after @head.
 *
 * Returns:
 *   - First node in the list.
 *   - NULL if the list is empty.
 *
 * Container operation.
 */
static inline list *list_pop_front(list *head)
{
    if (list_is_empty(head))
    {
        return NULL;
    }

    list *pop = head->next;

    list_unlink(pop);

    return pop;
}

/*
 * list_pop_back - remove and return the last node
 *
 * @head: list anchor
 *
 * Removes the node immediately before @head.
 *
 * Returns:
 *   - Last node in the list.
 *   - NULL if the list is empty.
 *
 * Container operation.
 */
static inline list *list_pop_back(list *head)
{
    if (list_is_empty(head))
    {
        return NULL;
    }

    list *node = head->prev;

    list_unlink(node);

    return node;
}

/*
 * list_replace - replace one node with another
 *
 * @old: linked node to replace
 * @new: detached replacement node
 *
 * Replaces @old in-place while preserving list ordering.
 *
 * Preconditions:
 *   - @old must be linked into a valid list.
 *   - @new must not currently be linked into any list.
 *
 * Postconditions:
 *   - @new occupies @old's former position.
 *   - @old remains unchanged but detached from the list.
 *
 * Useful for replacing embedded container objects without
 * changing surrounding list topology.
 */
static inline void list_replace(list *old, list *news)
{

#ifdef DEBUG
    assert(old);
    assert(list_is_linked(old));                              /* old must be in a list */
    assert(old->prev->next == old && old->next->prev == old); /* neighbors must agree */

    assert(news);
    assert(news->next == news && news->prev == news); /* news must be detached */
#endif

    news->next = old->next;
    news->prev = old->prev;

    old->prev->next = news;
    old->next->prev = news;
}
/*
 * list_replace_init - replace a node and detach the old one
 *
 * @old: linked node to replace
 * @new: detached replacement node
 *
 * Equivalent to:
 *
 *     list_replace(old, new);
 *     list_init(old);
 *
 * Postconditions:
 *   - @new occupies @old's position.
 *   - @old becomes self-linked.
 */
static inline void list_replace_init(list *old, list *news)
{

    list_replace(old, news);
    list_init(old);
}

/*
 * list_swap - exchange the positions of two nodes
 *
 * @entry1: first linked node
 * @entry2: second linked node
 *
 * Swaps the positions of two nodes while preserving the
 * ordering of all other nodes.
 *
 * Preconditions:
 *   - Both nodes must be linked.
 *   - Nodes may belong to the same or different lists
 *     if such usage is supported.
 *
 * Postconditions:
 *   - @entry1 occupies @entry2's former position.
 *   - @entry2 occupies @entry1's former position.
 *
 * Node contents are not modified; only list topology changes.
 */
static inline void list_swap(list *entry1, list *entry2)
{

#ifdef DEBUG
    assert(entry1);
    assert(entry2);
    assert(entry1 != entry2);        /* swapping a node with itself is a bug */
    assert(list_is_linked(entry1));
    assert(list_is_linked(entry2));
#endif

    list *pos = entry2->prev;

    list_unlink_stale(entry2);

    list_replace(entry1, entry2);

    if (pos == entry1)
        pos = entry2;
    list_add_after(pos, entry1);
}
/**
 * list_first() -  check if the node is first element
 * @node:           node to check
 * @head:           sentinal node
 *
 * This check if the node is first. Return true or false
 *
 */
static inline bool list_is_first(const list *node, const list *head)
{
    return node->prev == head;
}

/**
 * list_last() -   check if the node is last element
 * @node:           node to check
 * @head:           sentinal node
 *
 * This check if the node is last. Return true or false
 *
 */
static inline bool list_is_last(const list *node, const list *head)
{
    return node->next == head;
}

/* list_splice() - splice one list into another
 * @target:     the list to splice into
 * @source:     the list to splice
 *
 * This removes all the entries from @source and splice them into @target.
 * The order of the two lists is preserved and the source is appended
 * to the end of target.
 *
 * On return, the source list will be empty.
 */

static inline void list_splice(list *target, list *source)
{
#ifdef DEBUG
    assert(target);
    assert(source);
    assert(target != source);        /* swapping a node with itself is a bug */
    assert(list_is_linked(target));
    assert(list_is_linked(source));
#endif

    if (!list_is_empty(source))
    {
        /* attach front of @source to the tail of @target*/
        source->next->prev = target->prev;
        target->prev->next = source->next;
        /* attach the tail of @source to the target head */
        source->prev->next = target;
        target->prev = source->prev;

        // clear the target
        *source = (list)LIST_INIT(*source);
    }
}

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != head; (pos) = pos->next)

#define list_for_each_safe(pos, n, head)          \
    for ((pos) = (head)->next, (n) = (pos)->next; \
         (pos) != (head);                         \
         (pos) = (n), (n) = (pos)->next)

#define list_for_each_entry(entry, list_head, list_member)              \
    for ((entry) = list_entry((list_head)->next,                       \
                               __typeof__(*(entry)), list_member);     \
         &(entry)->list_member != (list_head);                         \
         (entry) = list_entry((entry)->list_member.next,               \
                               __typeof__(*(entry)), list_member))

static inline size_t list_length(const list* head) {

    size_t cnt = 0;
    const list * iter;
    list_for_each(iter, head) {
        ++cnt;
    }
    return cnt;
}