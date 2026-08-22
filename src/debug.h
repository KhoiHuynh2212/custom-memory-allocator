#ifndef DEBUG_H
#define DEBUG_H
#include "internal.h"   

#ifdef DEBUG

void check_heap(void);

void check_bins(void);

void check_top_chunk(struct malloc_state* state);

void check_heap_bin_consistency(void);

void check_malloced_chunk(struct malloc_state*, void *ptr, size_t size);

void check_mmapped_chunk(struct malloc_state*,mblockptr* block);

void check_current_use(struct malloc_state* state, mblockptr* block)

void check_free_chunk(struct malloc_state*, mblockptr*);
#else
#define check_heap() ((void)0)
#define check_bins() ((void)0)
#define check_top_chunk(state) ((void)0)
#define check_heap_bin_consistency()((void) 0)
#define check_malloced_chunk(ptr, size) ((void)0)
#define check_mmapped_chunk(mblockptr) ((void)0)
#define check_current_use(state, mblockptr) ((void)0)

#endif

#endif // DEBUG_H