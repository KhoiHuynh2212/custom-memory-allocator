#ifndef MALLOC_CHUNK
#define MALLOC_CHUNK

#include <sys/types.h>

#include "malloc_config.h"
#include "list.h" 
#define CHUNK_ALIGN_MASK (MALLOC_ALIGNMENT - ((size_t) 1))

static inline int is_aligned(void* p) {
    return ((size_t) p & CHUNK_ALIGN_MASK) == 0; // check this address align
}

static inline size_t align_offset(void *p) {
    return (MALLOC_ALIGNMENT - ((size_t) p & CHUNK_ALIGN_MASK)) & CHUNK_ALIGN_MASK;
} 

#define PREV_INUSE_BIT ((size_t) 1)
#define CURR_INUSE_BIT ((size_t) 2) 

#define INUSE_BITS      (PREV_INUSE_BIT | CURR_INUSE_BIT)
#define FLAG_BITS       (PREV_INUSE_BIT | CURR_INUSE_BIT) 


typedef struct malloc_chunk
{   
    size_t prev_sz; 
    size_t size;
    list list;      // double pointers
} mblockptr; // block header structure 

void insert_small_chunk(mblockptr * chunk, size_t size);
void insert_large_chunk(mblockptr * chunk, size_t size); 


#endif MALLOC_CHUNK 