#ifndef DEBUG_H
#define DEBUG_H
#include "internal.h"   

#ifdef DEBUG

void check_top_chunk(struct malloc_state* state);

void check_malloc_state(struct malloc_state* state);

void check_malloced_chunk(struct malloc_state* state, void *ptr, size_t size);

void check_mmapped_chunk(struct malloc_state* state, mblockptr* block);

void check_current_use(struct malloc_state* state, mblockptr* block);

void check_free_chunk(struct malloc_state* state, mblockptr* block);
#else

#define check_top_chunk(state) ((void)0)
#define check_malloced_chunk(state, ptr, size) ((void)0)
#define check_mmapped_chunk(state, block) ((void)0)
#define check_current_use(state, block) ((void)0)
#define check_malloc_state(state) ((void)0)
#define check_free_chunk(state, block) ((void)0)
#endif

#endif // DEBUG_H