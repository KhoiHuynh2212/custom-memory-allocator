#ifndef MALLOC_CHUNK
#define MALLOC_CHUNK

#include <sys/types.h>
#include "malloc_config.h"
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

static inline int is_mapped(void *chunk)
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
    size_t prev_foot; /* only valid if the previous physical chunk are freed* /
    size_t head;      /* hold bits if the chunk is free or allocated */
    list list;        /* double links - use only if free */
} mblockptr;

typedef unsigned int bin_index_t;
typedef unsigned int bin_map_t;
typedef unsigned int flag_t;

#define MALLOC_CHUNK_SIZE (sizeof(struct malloc_chunk));

/* The smallest size we can malloc is an aligned minimal chunk */
#define MIN_CHUNK_SIZE ((MALLOC_CHUNK_SIZE + CHUNK_ALIGN_MASK) & ~CHUNK_ALIGN_MASK)

static inline void* chunk_to_mem(void* p) {
    return (void*) ((char*) p + sizeof(size_t) * 2);  /* point just right after the head*/
}

static inline struct malloc_chunk* mem_to_chunk(void* p) {
    return (struct malloc_chunk *) ((char*) p - sizeof(size_t) * 2); /* go back to the pointer of the chunk */
}
void insert_small_chunk(mblockptr *chunk, size_t size);
void insert_large_chunk(mblockptr *chunk, size_t size);

#endif MALLOC_CHUNK