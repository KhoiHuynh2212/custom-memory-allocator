#include <assert.h>
#include "debug.h"

#ifdef DEBUG
static const malloc_state *state = NULL;

static void ensure_state(void)
{
    if (state == NULL)
        state = debug_get_state();
}

static void check_any_chunk(mblockptr* block)
{
    assert(ok_address(state, block));
    assert(is_aligned(block->payload));
    assert(is_aligned((char *)(block + 1)));
}

void check_top_chunk(struct malloc_state* state)
{
    
    assert(state->topchunkptr != NULL);

    size_t sz = state->topchunkptr->payload;

    check_any_chunk(state->topchunkptr);

    assert((char *)(state->topchunkptr + 1) + state->topsize == state->heap_end);
    assert(sz == state->topsize);
    assert(sz > 0);
    assert(is_free(state->topchunkptr));
    assert(!is_mmap(state->topchunkptr));
    assert(!list_is_linked(&state->topchunkptr->list));
}

void check_mmapped_chunk(mblockptr *block)
{
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

void check_bins(void)
{
    ensure_state();

    mblockptr *curr;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_for_each_entry(curr, &state->bins[i], list)
        {
            check_any_chunk(curr);

            assert(curr->payload != 0);
            size_t *footer = (size_t *)((char *)(curr + 1) + curr->payload);
            assert(is_free(curr));
            assert(get_bin(curr->payload) == i);
            assert(curr->payload == *footer);
            assert(curr != state->topchunkptr);
        }
    }
}

void check_heap(void)
{
    ensure_state();

    mblockptr *curr = (mblockptr *)state->heap_start;

    while (curr != (mblockptr *)state->topchunkptr && (char *)curr < state->heap_end)
    {
        check_any_chunk(curr);

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

void check_heap_bin_consistency(void)
{
    ensure_state();

    size_t free_chunk = 0;
    mblockptr *curr = (mblockptr *)state->heap_start;

    while (curr != (mblockptr *)state->topchunkptr && (char *)curr < state->heap_end)
    {
        if (is_free(curr))
        {
            free_chunk++;
        }

        curr = BLOCK_NEXT_HEADER(curr, curr->payload);
    }

    size_t binned_cnt = 0;

    for (int i = 0; i < NUM_BINS; i++)
    {
        binned_cnt += list_length(&state->bins[i]);
    }

    assert(free_chunk == binned_cnt);
}

void check_current_use(struct malloc_state* state, mblockptr* block) {
    
}
void check_malloced_chunk(struct malloc_state* state,void *ptr, size_t size)
{   
    if (ptr == NULL)
        return;

    mblockptr *block = (mblockptr *)ptr - 1;
    size_t request_size = align_up(size);

    assert(is_aligned(ptr));
    assert(!is_free(block));

    if (is_mmap(block))
    {
        check_mmapped_chunk(block);
        assert(block->payload >= size); // mmap rounds up to page
    }
    else
    {
        check_any_chunk(block);

        assert(block->payload >= request_size);
        assert(block->payload < request_size + MINBLOCKSIZE);

        size_t *footer = (size_t *)((char *)(block + 1) + block->payload);
        assert(*footer == block->payload);
    }
}

#endif