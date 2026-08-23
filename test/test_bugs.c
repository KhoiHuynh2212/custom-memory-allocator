#include "../src/debug.h"
#include "my-malloc.h"
#include <stdlib.h>
#include <time.h>   

void test_grow_top_topsize_stays_synced(void)
{
    heap_init();

    size_t almost_all = 65526;
    void *a = my_malloc(almost_all);
    check_top_chunk((struct malloc_state*)debug_get_state());

    void *b = my_malloc(500);
    check_top_chunk((struct malloc_state*)debug_get_state());

    printf("test_grow_top_topsize_stays_synced: PASS\n");

    my_free(a);
    my_free(b);
}

void test_top_carve_payload_matches_request(void)   
{
    heap_init();
    void *p = my_malloc(200);
    check_malloc_state((struct malloc_state*)debug_get_state());
    printf("test_top_carve_payload_matches_request: PASS\n");

    my_free(p);
}

void test_no_shrink_below_threshold(void)
{
    heap_init();
    const malloc_state *s = debug_get_state();
    size_t top_size_before = s->topsize;
    void *heap_end_before = s->heap_end;
    assert(top_size_before < (size_t)TRIM_THRESHOLD);
    void *p = my_malloc(16);
    assert(p != NULL);
    my_free(p);
    assert(s->heap_end == heap_end_before);
    assert(s->topsize != TOP_PAD_SIZE);
    assert(s->topsize >= top_size_before);
    printf("test_no_shrink_below_threshold: PASS (topsize=%zu, still below %ld)\n",
           s->topsize, TRIM_THRESHOLD);
}

void test_shrink_lands_on_pad_size(void)
{
    heap_init();
    const malloc_state *st = debug_get_state();

    for (int i = 0; i < 3; i++)
        assert(grow_top(1) != NULL);
    assert(st->topsize >= (size_t)TRIM_THRESHOLD);

    void *guard = my_malloc(16);
    assert(guard != NULL);
    my_free(guard);

    assert(st->topsize == (size_t)TOP_PAD_SIZE);
    assert(st->topchunkptr->payload == (size_t)TOP_PAD_SIZE);
    assert((char *)(st->topchunkptr + 1) + st->topsize == st->heap_end);

    printf("test_no_shrink_below_threshold: PASS (topsize=%zu, still below %ld)\n",
           st->topsize, TRIM_THRESHOLD);
}

void test_descending_order_bin(void)
{
    heap_init();

    const malloc_state *st = debug_get_state();

    void *p = my_malloc(2208);
    assert(p != NULL);

    void *sp1 = my_malloc(64);
    assert(sp1 != NULL);

    void *a = my_malloc(3001);
    assert(a != NULL);

    void *sp2 = my_malloc(64);
    assert(sp2 != NULL);

    void *b = my_malloc(3234);
    assert(b != NULL);

    void *sp3 = my_malloc(64);
    assert(sp3 != NULL);

    void *c = my_malloc(2267);
    assert(c != NULL);
    void *sp4 = my_malloc(64);
    assert(sp4 != NULL);

    void *d = my_malloc(2943);
    assert(d != NULL);
    void *sp5 = my_malloc(64);
    assert(sp5 != NULL);

    long g_scan_before = g_scan_steps;
    printf("Scan before is %ld\n", g_scan_before);
    int idx1 = get_bin(2208);
    assert(idx1 == get_bin(2208));

    my_free(p);
    my_free(a);
    my_free(b);
    my_free(c);
    my_free(d);

    check_malloc_state((struct malloc_state*)st);

    const list *head = &st->bins[idx1];
    assert(list_length(head) == 5);
    list *curr = head->next;
    do
    {
        mblockptr *curr_block = list_entry(curr, mblockptr, list);
        size_t curr_size = curr_block->payload;
        list *next = curr->next;
        if (next != head)
        {
            mblockptr *next_block = list_entry(next, mblockptr, list);
            size_t next_size = next_block->payload;
            assert(curr_size >= next_size);
        }
        curr = next;
    } while (curr != head);

    void *x = my_malloc(3008);

    long g_scan_after = g_scan_steps;
    printf("Scan after is %ld\n", g_scan_after);

    my_free(x);
    assert(g_scan_steps - g_scan_before <= 3);

    my_free(sp1);
    my_free(sp2);
    my_free(sp3);
    my_free(sp4);
    my_free(sp5);

    void *dup1 = my_malloc(2560);
    assert(dup1 != NULL);

    void *sp6 = my_malloc(64);
    assert(sp6 != NULL);

    void *dup2 = my_malloc(2560);
    assert(dup2 != NULL);

    void *sp7 = my_malloc(64);
    assert(sp7 != NULL);

    int idx2 = get_bin(2560);

    my_free(dup1);
    my_free(dup2);

    check_malloc_state((struct malloc_state*)st);

    const list *head2 = &st->bins[idx2];
    assert(list_length(head2) == 2);

    mblockptr *dup1_block = (mblockptr *)dup1 - 1;
    mblockptr *dup2_block = (mblockptr *)dup2 - 1;

    mblockptr *first_in_bin = list_entry(head2->next, mblockptr, list);
    mblockptr *second_in_bin = list_entry(head2->next->next, mblockptr, list);

    assert(first_in_bin == dup1_block);
    assert(second_in_bin == dup2_block);

    my_free(sp6);
    my_free(sp7);
}

#define N 1000
#define BIG_SIZE 136 * 1024 // adjust so it exceeds your mmap threshold
#define SMALL_MIN 16
#define SMALL_MAX 512

void test_non_deterministic_crash(void)
{
    heap_init();
    srand(time(NULL));

    
    void *ptrs[N];
    memset(ptrs, 0, sizeof(ptrs));

    for (int i = 0; i < N; i++) {
        size_t sz = (i % 10 == 0) ? BIG_SIZE : (SMALL_MIN + rand() % SMALL_MAX);
        ptrs[i] = my_malloc(sz);

        if (i > 5 && rand() % 3 == 0)
        {
            int victim = rand() % i;
            if (ptrs[victim] != NULL)
            {
                my_free(ptrs[victim]);
                ptrs[victim] = NULL;
            }
        }
    }

    for (int i = 0; i < N; i++) {
        if (ptrs[i] != NULL)
            my_free(ptrs[i]);
    }
}

int main()
{
    test_non_deterministic_crash(); 

    return 0;
}