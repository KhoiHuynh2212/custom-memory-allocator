#include <assert.h>
#include "debug.h"

#ifdef DEBUG

static void check_any_chunk(struct malloc_state* state, mblockptr* block)
{
    assert(ok_address(state, block));
    assert(is_aligned(block->payload));
    assert(is_aligned((char *)(block + 1)));
}

void check_top_chunk(struct malloc_state* state)
{
    
    assert(state->topchunkptr != NULL);

    size_t sz = state->topchunkptr->payload;

    check_any_chunk(state, state->topchunkptr);

    assert((char *)(state->topchunkptr + 1) + state->topsize == state->heap_end);
    assert(sz == state->topsize);
    assert(sz > 0);
    assert(is_free(state->topchunkptr));
    assert(!is_mmap(state->topchunkptr));
    assert(!list_is_linked(&state->topchunkptr->list));
}

void check_mmapped_chunk(struct malloc_state* state, mblockptr *block)
{
    (void)state; // not needed for this check yet; kept for signature consistency
    assert(block != NULL);
    size_t sz = block->payload;

    assert(is_mmap(block));
    assert(!is_free(block));
    assert(sz >= MMAP_THRESHOLD);
    assert(is_aligned(sz));
    assert(is_aligned((char *)(block + 1)));
    assert(((uintptr_t)block % (uintptr_t)LINUX_PAGE) == 0);

    size_t total = HEADER_SIZE + sz + FOOTER_SIZE;
    assert((total % (size_t)LINUX_PAGE) == 0);

    size_t *footer = (size_t *)((char *)(block + 1) + sz);
    assert(*footer == sz);
}

/* check every free chunk sitting in the bins is well-formed and lives in the bin its own size maps to */
static void check_bins(struct malloc_state *state)
{
    mblockptr *curr;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_for_each_entry(curr, &state->bins[i], list)
        {
            check_free_chunk(state, curr); 

            assert(curr->payload != 0);
            size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
            assert(get_bin(curr->payload) == i);
            assert(curr->payload == *footer);
            assert(curr != state->topchunkptr);
        }
    }
}

/* walk the heap chunk by chunk, checking headers/footers and the no-two-adjacent-free-chunks invariant */
static void check_heap(struct malloc_state *state)
{
    mblockptr *curr = (mblockptr *)state->heap_start;

    while (curr != (mblockptr *)state->topchunkptr && (char *)curr < state->heap_end)
    {
        check_any_chunk(state, curr);

        assert(curr->payload != 0);
        mblockptr *next = BLOCK_NEXT_HEADER(curr, curr->payload);
        assert((char *)next <= state->heap_end);
        size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
        assert(curr->payload == *footer);

        if (next != state->topchunkptr)
        {
            assert(!(is_free(curr) && is_free(next)));
        }
        else
        {
            assert(!is_free(curr));
        }
        curr = next;
    }
}

static int bin_find(struct malloc_state *state, mblockptr *target)
{
    int idx = get_bin(target->payload);
    mblockptr *curr;
 
    list_for_each_entry(curr, &state->bins[idx], list)
    {
        if (curr == target)
            return 1;
    }
    return 0;
}
 
static void check_heap_bin_consistency(struct malloc_state *state)
{
    mblockptr *curr = (mblockptr *)state->heap_start;
 
    while (curr != (mblockptr *)state->topchunkptr && (char *)curr < state->heap_end)
    {
        if (is_free(curr))
        {
            assert(bin_find(state, curr)); // free chunk must be registered in its bin
        }
        else
        {
            assert(!bin_find(state, curr)); // inuse chunk must not still be linked in a bin
        }
 
        curr = BLOCK_NEXT_HEADER(curr, curr->payload);
    }
}


void check_malloc_state(struct malloc_state *state)
{
    check_top_chunk(state);
    check_bins(state);
    check_heap(state);
    check_heap_bin_consistency(state);
}

void check_current_use(struct malloc_state* state, mblockptr* block) {
    check_any_chunk(state, block);
    assert(!is_free(block));
    if(is_mmap(block)) {
        check_mmapped_chunk(state, block);
    }
}

void check_malloced_chunk(struct malloc_state* state, void *ptr, size_t size)
{   
    if (ptr == NULL)
        return;

    mblockptr *block = (mblockptr *)ptr - 1;
    size_t request_size = align_up(size);

    assert(is_aligned(ptr));
    assert(!is_free(block));

    if (is_mmap(block))
    {
        check_mmapped_chunk(state, block);
        assert(block->payload >= size); // mmap rounds up to page
    }
    else
    {
        check_any_chunk(state, block);

        assert(block->payload >= request_size);
        assert(block->payload < request_size + MINBLOCKSIZE);

        size_t *footer = (size_t *)((char *)(block + 1) + block->payload);
        assert(*footer == block->payload);
    }
}

void check_free_chunk(struct malloc_state* state, mblockptr* block) {
    size_t sz = chunk_size(block);
    check_any_chunk(state, block);
    assert(is_free(block));
    assert(!is_mmap(block));

    if(block != state->topchunkptr) {
        if(sz >= MINBLOCKSIZE) {
            assert(is_aligned(sz));
            assert(block->list.next->prev == &block->list);
            assert(block->list.prev->next == &block->list);
        }
    }
}
#endif