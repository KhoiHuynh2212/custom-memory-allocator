#ifndef MALLOC_INTERNAL
#define MALLOC_INTERNAL
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "list.h"
#include "../include/my-malloc.h"



#define align _Alignof(max_align_t) // return system align
#define align_up(n) (((n) + align - 1) & ~(align - 1))
#define is_aligned(n) (((size_t)(n) & (align - 1)) == 0)
#define MINBLOCKSIZE (HEADER_SIZE + FOOTER_SIZE + align) // minimum size to split
#define free_bit (1 << 0)                                // bit 0: 1 = free, 0 = allocated
#define mmap_bit (1 << 1)                                // bit 1: 1 = mmapped, 0 = sbrk'd

#define set_free_chunk(b) ((b)->flags |= free_bit)       // set the block is free
#define set_allocated_chunk(b) ((b)->flags &= ~free_bit) // set the block is allocated

#define is_free(b) ((b)->flags & free_bit) // check the block is free
#define is_mmap(b) ((b)->flags & mmap_bit) // check the block is from mmap

#define set_mmap_chunk(b) ((b)->flags |= mmap_bit)  // set the block is from mmap
#define set_chunk(b) ((b)->flags &= ~mmap_bit) // set the block is from heap

extern long g_sbrk_calls;
extern long g_scan_steps;

typedef struct Block_Header
{
    size_t payload;
    unsigned int flags;
    list list;
} mblockptr; // block header structure 


#define CHUNK_SIZE ((size_t)64U * (size_t)1024U) // Chunk size is 64 KB
#define TOP_PAD_SIZE (CHUNK_SIZE / (size_t)4U)    //
#define LINUX_PAGE sysconf(_SC_PAGESIZE)
#define INITIAL_TOP_SIZE ((size_t)64U * (size_t)1024U)
#define SMALL_BIN_MAX 1024
#define NUM_SMALL_BINS 64
#define LARGE_BIN_MIN_EXP 10 // 2^ 10 = 1024
#define LARGE_BIN_MAX_EXP 16 // 2^ 16 = 65536
#define NUM_LARGE_BINS (LARGE_BIN_MAX_EXP - LARGE_BIN_MIN_EXP + 1)
#define NUM_BINS (NUM_SMALL_BINS + NUM_LARGE_BINS)

#define HEADER_SIZE (sizeof(mblockptr))
#define FOOTER_SIZE align_up(sizeof(size_t))
#define align_tag align_up(HEADER_SIZE + FOOTER_SIZE)

#define REQUEST_CHUNK(s) ((s) + (HEADER_SIZE) + (FOOTER_SIZE))
#define ABSORB(s) (REQUEST_CHUNK(s))
#define MMAP_THRESHOLD ((size_t)128U * (size_t)1024U) // Trigger mmap allocation

/** Shrink threshold must stay strictly greater than one grow_top() increment
(CHUNK_SIZE) to avoid heap "flapping": growing by CHUNK_SIZE then
immediately freeing it must NOT trigger an immediate shrink back to the OS,
or every alloc/free pair near this size would cost two sbrk() syscalls.
Expressed as CHUNK_SIZE * 2 (not a fixed byte count) so the margin scales
automatically if CHUNK_SIZE is retuned. Currently: 2 * 64 KB = 128 KB.
**/
#define TRIM_THRESHOLD (CHUNK_SIZE * 2) // 12 KB

#define BLOCK_NEXT_HEADER(curr, payload) \
    ((mblockptr *)((char *)((curr) + 1) + (payload) + FOOTER_SIZE))

#define BLOCK_PREV_HEADER(curr, prev_size) \
    ((mblockptr *)((char *)curr - FOOTER_SIZE - prev_size - HEADER_SIZE))

typedef struct malloc_state {
    mblockptr *topchunkptr; 
    list bins[NUM_BINS];
    size_t topsize; 
    char *heap_start;
    char *heap_end;
} malloc_state;  

#define ok_address(state, a) \
    ((char*)(a) >= (state)->heap_start && (char*)(a) <= (state)->heap_end) 
    
// function prototypes
void heap_init(void);
mblockptr *find_suitable_block(size_t request_size);
mblockptr *grow_top(size_t size);
mblockptr *split(mblockptr *block, size_t request_payload);
mblockptr *coalesce(mblockptr *curr);
mblockptr *try_expand(mblockptr *curr, size_t new_payload);

static inline void set_footer(mblockptr *block)
{        
    size_t *footer =
        (size_t *)((char *)(block + 1) + block->payload);

    *footer = block->payload;
    #ifdef DEBUG
    assert(*footer == block->payload);
    #endif
}

void insert_small_chunk(mblockptr * chunk, size_t size);
void insert_large_chunk(mblockptr * chunk, size_t size);



size_t trim_chunk(mblockptr* block);
size_t get_MSB_bit(size_t x);

int get_bin(size_t payload);

#define chunk_size(b) ((b)->payload)

#ifdef DEBUG
const malloc_state *debug_get_state(void);
#endif


#endif