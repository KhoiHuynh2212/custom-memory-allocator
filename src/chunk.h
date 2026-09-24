#ifndef MALLOC_CHUNK
#define MALLOC_CHUNK

#include <sys/types.h>
#include "config.h"
#include "list.h"

#define CHUNK_ALIGN_MASK (MALLOC_ALIGNMENT - ((size_t)1))

static inline int is_aligned(void *p)
{
    return ((size_t)p & CHUNK_ALIGN_MASK) == 0; // check this address align
}

static inline size_t align_offset(void *p)
{
    return (MALLOC_ALIGNMENT - ((size_t)p & CHUNK_ALIGN_MASK)) & CHUNK_ALIGN_MASK;
}

struct any_chunk
{
    /*must be compatible with malloc_chunk*/
    size_t prev_foot;
    size_t head;
};

#define PREV_INUSE_BIT ((size_t)1)
#define CURR_INUSE_BIT ((size_t)2)

#define INUSE_BITS (PREV_INUSE_BIT | CURR_INUSE_BIT)
#define FLAG_BITS (PREV_INUSE_BIT | CURR_INUSE_BIT)

static inline size_t chunk_size(void *chunk)
{
    return ((struct any_chunk *)chunk)->head & ~FLAG_BITS; // clear all the flag bits to get real size
}

static inline size_t get_foot(void *chunk, size_t size)
{
    return ((struct any_chunk *)((char *)chunk + size))->prev_foot; // jump to footer of current chunk [chunkA][footer of chunkA, chunkB]
}

static inline void set_foot(void *chunk, size_t size)
{
    ((struct any_chunk *)((char *)chunk + size))->prev_foot = size; // set current footer's chunk
}

static inline int curr_inuse(void *chunk)
{
    return ((struct any_chunk *)chunk)->head & CURR_INUSE_BIT;
}

static inline int prev_inuse(void *chunk)
{
    return ((struct any_chunk *)chunk)->head & PREV_INUSE_BIT;
}

static inline int is_use(void *chunk)
{
    return (((struct any_chunk *)chunk)->head & INUSE_BITS) != PREV_INUSE_BIT;
}

static inline int is_mmapped(void *chunk)
{
    return (((struct any_chunk *)chunk)->head & INUSE_BITS) == 0;
}

static inline void clear_prev_inuse(void *chunk)
{
    ((struct any_chunk *)chunk)->head &= ~PREV_INUSE_BIT;
}

static inline void set_size_and_prev_inuse_of_free_chunk(void *chunk, size_t size)
{
    ((struct any_chunk *)chunk)->head = size | PREV_INUSE_BIT;
    set_foot(chunk, size);
}

static inline void set_free_with_prev_inuse(void *chunk, size_t size, void *n)
{
    clear_prev_inuse(n);
    set_size_and_prev_inuse_of_free_chunk(chunk, size);
}

typedef struct malloc_chunk
{
    size_t prev_foot; /* only valid if the previous physical chunk are freed*/
    size_t head;      /* hold bits if the chunk is free or allocated */
    list list;        /* double links - use only if free */
} mblockptr;

typedef unsigned int bin_index_t;
typedef unsigned int bin_map_t;
typedef unsigned int flag_t;

#define MALLOC_CHUNK_SIZE (sizeof(struct malloc_chunk))

#if FOOTERS
#define CHUNK_OVERHEAD (sizeof(size_t) * 2)
#else /* FOOTERS */
#define CHUNK_OVERHEAD (sizeof(size_t))
#endif /* FOOTERS */

/* MMapped chunks need a second word of overhead ... */
#define MMAP_CHUNK_OVERHEAD (sizeof(size_t) * 2)
/* ... and additional padding for fake next-chunk at foot */
#define MMAP_FOOT_PAD (sizeof(size_t) * 4)

/* The smallest size we can malloc is an aligned minimal chunk */
#define MIN_CHUNK_SIZE ((MALLOC_CHUNK_SIZE + CHUNK_ALIGN_MASK) & ~CHUNK_ALIGN_MASK)

static inline void *chunk_to_mem(void *p)
{
    return (void *)((char *)p + sizeof(size_t) * 2); /* point just right after the head*/
}

static inline struct malloc_chunk *mem_to_chunk(void *p)
{
    return (struct malloc_chunk *)((char *)p - sizeof(size_t) * 2); /* go back to the pointer of the chunk */
}

static inline struct malloc_chunk *align_as_chunk(void *p)
{
    return (struct malloc_chunk *)(p + align_offset(chunk_to_mem(p)));
}

/* Bounds on request (not chunk) sizes. */
#define MAX_REQUEST ((-MIN_CHUNK_SIZE) << 2)
#define MIN_REQUEST (MIN_CHUNK_SIZE - CHUNK_OVERHEAD - (size_t)1)

/* pad request bytes into a usable size */
static inline size_t pad_request(size_t req)
{
    return (req + CHUNK_OVERHEAD + CHUNK_ALIGN_MASK) & ~CHUNK_ALIGN_MASK;
}

/* pad request, checking for minimum (but not maximum) */
static inline size_t request_to_size(size_t req)
{
    return req < MIN_REQUEST ? MIN_CHUNK_SIZE : pad_request(req);
}

static inline struct malloc_chunk *chunk_plus_offset(void *chunk, size_t size)
{
    return (struct malloc_chunk *)(((char *)chunk) + size);
}

static inline struct malloc_chunk *chunk_minus_offset(void *chunk, size_t size)
{
    return (struct malloc_chunk *)(((char *)chunk) - size);
}

static inline struct malloc_chunk *next_chunk(void *chunk)
{
    return (struct malloc_chunk *)(((char *)chunk) + (((struct any_chunk *)chunk)->head & ~FLAG_BITS));
}

static inline struct malloc_chunk *prev_chunk(void *chunk)
{
    return (struct malloc_chunk *)(((char *)chunk) - (((struct any_chunk *)chunk)->prev_foot));
}

/* Get the internal overhead associated with chunk p */
static inline size_t overhead_for(void *chunk)
{
    return is_mmapped(chunk) ? MMAP_CHUNK_OVERHEAD : CHUNK_OVERHEAD;
}

/* Return true if malloced space is not necessarily cleared */
static inline int calloc_must_clear(void *chunk)
{
    return !is_mmapped(chunk);
}
/* macros to set up inuse chunks with or without footers */
struct malloc_state;

#if !FOOTERS

static inline void mark_inuse_foot(struct malloc_state *state, void *chunk, size_t size)
{
    (void)state;
    (void)chunk;
    (void)size;
}
/* Set curr_inuse bit and prev_inuse bit of next chunk */
static inline void set_inuse(struct malloc_state *state, void *chunk, size_t size)
{
    (void)state; // unused
    ((struct any_chunk *)chunk)->head = (((struct any_chunk *)chunk)->head & PREV_INUSE_BIT) | size | CURR_INUSE_BIT;
    ((struct malloc_chunk *)(((char *)chunk) + size))->head |= PREV_INUSE_BIT;
}

/* Set curr_inuse and prev_inuse of this chunk and prev_inuse of next chunk */
static inline void set_inuse_and_prev_inuse(struct malloc_state *state, void *chunk, size_t size)
{
    (void)state; // unused
    ((struct any_chunk *)chunk)->head = size | PREV_INUSE_BIT | CURR_INUSE_BIT;
    ((struct malloc_chunk *)(((char *)chunk) + size))->head |= PREV_INUSE_BIT;
}

/* Set size, curr_inuse and prev_inuse bit of this chunk */
static inline void set_size_and_prev_inuse_of_inuse_chunk(struct malloc_state *state, void *chunk, size_t size)
{
    (void)state; // unused
    ((struct any_chunk *)chunk)->head = size | PREV_INUSE_BIT | CURR_INUSE_BIT;
}

#else /* FOOTERS */

/* Set foot of inuse chunk to be xor of mstate and seed */
#define mark_inuse_foot(M, p, s) \
    (((struct malloc_chunk *)((char *)(p) + (s)))->prev_foot = ((size_t)(M) ^ params.magic))

#define get_state_for(p)                                                \
    ((struct malloc_state *)(((struct malloc_chunk *)((char *)(p) +     \
                                                      (chunk_size(p)))) \
                                 ->prev_foot ^                          \
                             params.magic))

#define set_inuse(M, p, s)                                                     \
    ((p)->head = (((p)->head & PREV_INUSE_BIT) | s | CURR_INUSE_BIT),          \
     (((struct malloc_chunk *)(((char *)(p)) + (s)))->head |= PREV_INUSE_BIT), \
     mark_inuse_foot(M, p, s))

#define set_inuse_and_prev_inuse(M, p, s)                                      \
    ((p)->head = (s | PREV_INUSE_BIT | CURR_INUSE_BIT),                        \
     (((struct malloc_chunk *)(((char *)(p)) + (s)))->head |= PREV_INUSE_BIT), \
     mark_inuse_foot(M, p, s))

#define set_size_and_prev_inuse_of_inuse_chunk(M, p, s) \
    ((p)->head = (s | PREV_INUSE_BIT | CURR_INUSE_BIT), \
     mark_inuse_foot(M, p, s))

#endif /* !FOOTERS */

struct malloc_tree_chunk
{
    /* The first four fields must be compatible with malloc_chunk */
    size_t prev_foot;
    size_t head;
    list list;

    struct malloc_tree_chunk *child[2];
    struct malloc_tree_chunk *parent;
    bin_index_t index;
};

/*Helper macros for tree*/
static inline struct malloc_tree_chunk *leftmost_child(struct malloc_tree_chunk *t)
{
    return t->child[0] != 0 ? t->child[0] : t->child[1];
}

#define compute_tree_index(S, I)                                                                  \
    {                                                                                             \
        unsigned int X = S >> TREE_BIN_SHIFT;                                                     \
        if (X == 0)                                                                               \
            I = 0;                                                                                \
        else if (X > 0xFFFF)                                                                      \
            I = NUM_TREE_BINS - 1;                                                                \
        else                                                                                      \
        {                                                                                         \
            unsigned int K = (unsigned)sizeof(X) * __CHAR_BIT__ - 1 - (unsigned)__builtin_clz(X); \
            I = (bin_index_t)((K << 1) + ((S >> (K + (TREE_BIN_SHIFT - 1)) & 1)));                \
        }                                                                                         \
    }

struct malloc_medium_chunk
{
    size_t prev_foot;
    size_t head;

    list list; /* normal fd/bk bin links */
    /* Only used for large blocks: pointer to next larger size.  */
    struct malloc_medium_chunk *fd_nextsize; /* double links -- used only if free. */
    struct malloc_medium_chunk *bk_nextsize;
};

/* generic dispatcher */
void insert_chunk(
    struct malloc_state *state,
    struct malloc_chunk *chunk,
    size_t size);

void unlink_chunk(
    struct malloc_state *state,
    struct malloc_chunk *chunk,
    size_t size);

/* small */
void insert_small_chunk(
    struct malloc_state *state,
    struct malloc_chunk *chunk,
    size_t size);

void unlink_small_chunk(
    struct malloc_state *state,
    struct malloc_chunk *chunk,
    size_t size);

void unlink_first_small_chunk(
    struct malloc_state *state,
    struct malloc_chunk *bin,
    struct malloc_chunk *chunk,
    bin_index_t index);

/* medium */
void insert_medium_chunk(
    struct malloc_state *state,
    struct malloc_medium_chunk *chunk,
    size_t size
);

void unlink_medium_chunk(
    struct malloc_state *state,
    struct malloc_medium_chunk *chunk,
    size_t size
);

void insert_large_chunk(struct malloc_state *, struct malloc_tree_chunk *, size_t);

void unlink_large_chunk(struct malloc_state *, struct malloc_tree_chunk *);

void dispose_chunk(struct malloc_state *, struct malloc_chunk *, size_t);

#endif MALLOC_CHUNK