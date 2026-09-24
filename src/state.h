#ifndef MALLOC_STATE_H
#define MALLOC_STATE_H

#include "config.h"
#include "chunk.h"

/* Bin types, widths and sizes */
#define NUM_SMALL_BINS  (32U)
#define NUM_TREE_BINS   (32U)
#define NUM_MEDIUM_BINS (32u)
#define SMALL_BIN_SHIFT (3U)
#define TREE_BIN_SHIFT  (8U)
#define MIN_LARGE_SIZE ((size_t)1 << TREE_BIN_SHIFT)
#define MAX_SMALL_SIZE (MIN_LARGE_SIZE - (size_t)1)
#define MAX_SMALL_REQUEST (MAX_SMALL_SIZE - CHUNK_ALIGN_MASK - CHUNK_OVERHEAD)

static inline int is_small(size_t size) {
    return (size >> SMALL_BIN_SHIFT) < NUM_SMALL_BINS;
}

static inline bin_index_t small_index(size_t size) {
    return (bin_index_t) (size >> SMALL_BIN_SHIFT);
}

static inline size_t small_index_to_size(bin_index_t index) {
    return index << SMALL_BIN_SHIFT;
}

/* Shift placing maximum resolved bit in a tree_bin at i as sign bit */
static inline bin_index_t leftshift_for_tree_index(bin_index_t i) {
    return i == NUM_TREE_BINS - 1 ? 0 : SIZE_T_BITSIZE - (size_t) 1 - ((i >> 1) + TREE_BIN_SHIFT - 2);
}

/* The size of the smallest chunk held in bin with index i */
static inline bin_index_t minsize_for_tree_index(bin_index_t i) {
    return ((size_t) 1 << ((i >> 1) + TREE_BIN_SHIFT))
           | (((size_t) (i & (size_t) 1)) << ((i >> 1) + TREE_BIN_SHIFT - 1));
} 

struct malloc_state
{
    /* global synchronization */
     // mutex_t lock;

    /* top chunk */
    struct malloc_chunk *top;

    /* unsorted staging */
    struct malloc_chunk unsorted_bin;

    /* small exact bins */
    struct malloc_chunk small_bins[NUM_SMALL_BINS];

    /* medium glibc-style bins */
    struct malloc_medium_chunk medium_bins[NUM_MEDIUM_BINS];

    /* large dlmalloc-style tree bins */
    struct malloc_tree_chunk *tree_bins[NUM_TREE_BINS];

    /* fast lookup for non-empty bins */
    bin_map_t small_map;
    bin_map_t medium_map;
    bin_map_t tree_map;

    /* memory/region accounting */
    size_t footprint;
    size_t max_footprint;

    /* possibly region ownership later */
    struct heap_region *regions;
};
#endif MALLOC_STATE_H
