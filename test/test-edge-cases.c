#include "../include/my-malloc.h"
#include "../src/internal.h"
#include "../src/list.h"
#include "../src/debug.h" /* build with -DDEBUG: needs debug_get_state() */
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>

typedef struct
{
    int checks_run;
    int checks_failed;
} SharedCounters;

static SharedCounters *counters;
static int current_test_failed = 0;
static int g_verbose = 0;

static const char *COL_RED = "";
static const char *COL_GREEN = "";
static const char *COL_YELLOW = "";
static const char *COL_RESET = "";

static void enable_color_if_tty(void)
{
    if (isatty(fileno(stdout)))
    {
        COL_RED = "\033[31m";
        COL_GREEN = "\033[32m";
        COL_YELLOW = "\033[33m";
        COL_RESET = "\033[0m";
    }
}

#define CHECK(cond, msg)                                            \
    do                                                              \
    {                                                               \
        counters->checks_run++;                                     \
        if (cond)                                                   \
        {                                                           \
            printf("  %s[PASS]%s %s\n", COL_GREEN, COL_RESET, msg); \
        }                                                           \
        else                                                        \
        {                                                           \
            printf("  %s[FAIL]%s %s  (test-edge-cases.c:%d)\n",     \
                   COL_RED, COL_RESET, msg, __LINE__);              \
            counters->checks_failed++;                              \
            current_test_failed++;                                  \
        }                                                           \
    } while (0)

#define SECTION(name) printf("\n%s== %s ==%s\n", COL_YELLOW, name, COL_RESET)

static void vlog(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("    | ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* run_isolated - run @fn in a forked child so an abort()/segfault in it */
static int run_isolated(void (*fn)(void), int *signo)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        return 0;
    }
    if (pid == 0)
    {
        /* child */
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
    {
        if (signo)
            *signo = WTERMSIG(status);
        return -1;
    }
    if (signo)
        *signo = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ------------------------------------------------------------------ */
/* Helpers */
/* ------------------------------------------------------------------ */

static mblockptr *hdr_of(void *ptr) { return (mblockptr *)ptr - 1; }

static int ptr_is_aligned(const void *ptr)
{
    return ((uintptr_t)ptr % align) == 0;
}

/* watchdog_arm/disarm - guard against an internal infinite loop. */
static void watchdog_fired(int signo)
{
    (void)signo;
    const char msg[] = "\n  [FAIL] watchdog: operation did not return -- suspected infinite loop\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static void watchdog_arm(unsigned seconds)
{
    signal(SIGALRM, watchdog_fired);
    alarm(seconds);
}

static void watchdog_disarm(void)
{
    alarm(0);
}

/* bin_holds_exactly - true if target is the sole entry in its own bin */
static int bin_holds_exactly(const malloc_state *state, mblockptr *target)
{
    int idx = get_bin(target->payload);
    int count = 0;
    int found = 0;
    mblockptr *curr;

    list_for_each_entry(curr, &state->bins[idx], list)
    {
        count++;
        if (curr == target)
            found = 1;
    }

    return (count == 1 && found) ? 1 : 0;
}

/* Fill a buffer with a repeating byte pattern derived from a seed. */
static void fill_pattern(unsigned char *buf, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (unsigned char)(seed + i);
}

static int check_pattern(const unsigned char *buf, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++)
    {
        if (buf[i] != (unsigned char)(seed + i))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Edge case: NULL handling */
/* ------------------------------------------------------------------ */

static void test_null_handling(void)
{
    SECTION("NULL handling");

    my_free(NULL);
    CHECK(1, "my_free(NULL) is a safe no-op");

    void *p = my_realloc(NULL, 32);
    CHECK(p != NULL, "my_realloc(NULL, 32) behaves like my_malloc(32)");
    my_free(p);

    void *z = my_realloc(NULL, 0);
    CHECK(z == NULL, "my_realloc(NULL, 0) returns NULL (no-op, nothing to free)");
}

/* ------------------------------------------------------------------ */
/* Edge case: double free must be caught, not silently corrupt memory */
/* ------------------------------------------------------------------ */

static void child_double_free(void)
{
    void *p = my_malloc(64);
    my_free(p);
    my_free(p); /* should abort() the second time */
    /* If we get here, double-free was NOT detected -> exit nonzero */
    _exit(1);
}

static void test_double_free(void)
{
    SECTION("double free detection (isolated child process)");

    int signo = 0;
    int result = run_isolated(child_double_free, &signo);

    CHECK(result == -1 && signo == SIGABRT,
          "double free is detected and the process aborts (SIGABRT)");
    vlog("child exit path: result=%d signo=%d", result, signo);
}

/* ------------------------------------------------------------------ */
/* Edge case: alignment guarantees */
/* ------------------------------------------------------------------ */

static void test_alignment(void)
{
    SECTION("alignment guarantees");

    size_t sizes[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 63, 100, 255, 4096};
    int all_aligned = 1;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        void *p = my_malloc(sizes[i]);
        if (!p || !ptr_is_aligned(p))
        {
            all_aligned = 0;
            vlog("size=%zu ptr=%p NOT aligned to %zu", sizes[i], p, (size_t)align);
        }
        my_free(p);
    }
    CHECK(all_aligned, "every returned pointer is aligned to align (max_align_t)");
}

/* ------------------------------------------------------------------ */
/* Edge case: zero-size interactions */
/* ------------------------------------------------------------------ */

static void test_zero_size_variants(void)
{
    SECTION("zero-size variants (my_malloc/my_calloc)");

    CHECK(my_malloc(0) == NULL, "my_malloc(0) returns NULL");

    void *a = my_calloc(0, 16);
    CHECK(a == NULL, "my_calloc(0, 16) returns NULL (num*size == 0)");

    void *b = my_calloc(16, 0);
    CHECK(b == NULL, "my_calloc(16, 0) returns NULL (num*size == 0)");

    my_free(a);
    my_free(b);
}

/* ------------------------------------------------------------------ */
/* Edge case: split threshold (MIN_FREE_BLOCK boundary) */
/* ------------------------------------------------------------------ */

static void test_split_threshold(void)
{
    SECTION("split threshold at MIN_FREE_BLOCK boundary");

    /* Get a free block of a known, STABLE payload by allocating it with */
    void *p1 = my_malloc(256);
    void *guard1 = my_malloc(align); /* blocks forward coalescing of p1 */
    CHECK(p1 != NULL && guard1 != NULL, "my_malloc(256) + guard succeed");
    size_t original_payload = hdr_of(p1)->payload;
    my_free(p1);

    /* Request just small enough that leftover < MIN_FREE_BLOCK. */
    size_t tiny_shrink = original_payload - align;
    void *p2 = my_malloc(tiny_shrink);
    CHECK(p2 == p1, "reused the same block for a slightly smaller request");
    CHECK(hdr_of(p2)->payload == original_payload,
          "block was NOT split when the remainder would be < MIN_FREE_BLOCK "
          "(payload left oversized on purpose)");
    vlog("original_payload=%zu tiny_shrink=%zu actual_payload=%zu MIN_FREE_BLOCK=%zu",
         original_payload, tiny_shrink, hdr_of(p2)->payload, (size_t)MINBLOCKSIZE);
    my_free(p2);
    my_free(guard1);

    /* Now request small enough that a split SHOULD happen -- same guard */
    void *p3 = my_malloc(256);
    void *guard2 = my_malloc(align);
    size_t big_payload = hdr_of(p3)->payload;
    my_free(p3);

    size_t big_shrink = big_payload > (MINBLOCKSIZE + align)
                            ? big_payload - MINBLOCKSIZE - align
                            : align;
    void *p4 = my_malloc(big_shrink);
    CHECK(p4 == p3, "reused the same block for the split-eligible request");
    CHECK(hdr_of(p4)->payload < big_payload,
          "block WAS split when the remainder is >= MIN_FREE_BLOCK");
    vlog("big_payload=%zu big_shrink=%zu actual_payload=%zu",
         big_payload, big_shrink, hdr_of(p4)->payload);
    my_free(p4);
    my_free(guard2);
}

/* ------------------------------------------------------------------ */
/* Edge case: forward / backward / three-way coalescing */
/* ------------------------------------------------------------------ */

static void test_forward_coalesce(void)
{
    SECTION("forward coalescing: free(c) then free(b), b absorbs c");

    /* Four adjacent blocks so the merge under test (b absorbing c) is */
    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    void *d = my_malloc(64);
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    my_free(c); /* c isolated: neighbors b (alloc) and d (alloc) -> no merge */
    my_free(b); /* b's forward neighbor c is now free -> FORWARD merge: */

    void *big = my_malloc(64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(big != NULL, "allocation spanning b+c's merged region succeeds");
    CHECK(big == b, "merged region starts at 'b' (forward merge target)");

    my_free(big); /* frees the b+c region (same address as b) */
    my_free(a);
    my_free(d);
}

static void test_backward_coalesce(void)
{
    SECTION("backward coalescing: free(a) then free(b), b merges into a");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64); /* keep allocated so b's forward neighbor is busy */
    CHECK(a && b && c, "three adjacent allocations succeed");

    my_free(a); /* a alone in free list, no free neighbor yet */
    my_free(b); /* b's forward neighbor c is allocated -> no forward merge */

    CHECK(list_is_linked(&hdr_of(a)->list),
          "'a' remains linked in the free list after absorbing 'b' "
          "(not double-added)");

    void *big = my_malloc(64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(big == a, "allocation needing the merged a+b region reuses 'a'");

    my_free(big);
    my_free(c);
}

static void test_three_way_coalesce(void)
{
    SECTION("three-way coalescing: middle free absorbs both neighbors");

    void *a = my_malloc(48);
    void *b = my_malloc(48);
    void *c = my_malloc(48);
    void *d = my_malloc(48); /* trailing allocated block bounds the region */
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    my_free(a); /* isolated free block */
    my_free(c); /* isolated free block (not adjacent to a) */
    my_free(b); /* b sits between two free blocks: */

    void *big = my_malloc(48 * 3 + HEADER_SIZE * 2 + FOOTER_SIZE * 2 - 8);
    CHECK(big == a, "single allocation spans the fully-merged a+b+c region");

    my_free(big);
    my_free(d);
}

/* ------------------------------------------------------------------ */
/* Edge case: allocation bigger than CHUNK_SIZE forces sbrk extension */
/* ------------------------------------------------------------------ */

static void test_large_allocation_extends_heap(void)
{
    SECTION("allocation larger than CHUNK_SIZE (still under MMAP_THRESHOLD)");

    /* BUG FOUND DURING REVIEW: this test originally requested */
    size_t big_size = CHUNK_SIZE + (CHUNK_SIZE / 2); /* 96 KB: > CHUNK_SIZE, < MMAP_THRESHOLD */
    CHECK(big_size < MMAP_THRESHOLD, "sanity: test size stays under MMAP_THRESHOLD");

    void *p = my_malloc(big_size);
    CHECK(p != NULL, "my_malloc(1.5 * CHUNK_SIZE) succeeds via sbrk");
    CHECK(!is_mmap(hdr_of(p)), "allocation actually took the sbrk path, not mmap");

    memset(p, 0x7E, big_size);
    unsigned char *bytes = p;
    int ok = 1;
    for (size_t i = 0; i < big_size; i += 4096)
    { /* spot-check across pages */
        if (bytes[i] != 0x7E)
        {
            ok = 0;
            break;
        }
    }
    CHECK(ok, "large block is fully writable across its extended range");

    my_free(p);
}

/* ------------------------------------------------------------------ */
/* Edge case: exact-fit sbrk allocation must NOT attempt to split */
/* ------------------------------------------------------------------ */

static void test_extend_heap_exact_fit_no_split(void)
{
    SECTION("grow_top: exact-fit allocation in [CHUNK_SIZE, MMAP_THRESHOLD) does not split");

    /* grow_top() picks allocate_size as: */

    size_t request = CHUNK_SIZE + 1000; /* squarely inside the gap */
    CHECK(request >= (size_t)CHUNK_SIZE && align_up(request) < (size_t)MMAP_THRESHOLD,
          "sanity: request lands inside [CHUNK_SIZE, MMAP_THRESHOLD)");

    void *p = my_malloc(request);
    CHECK(p != NULL, "exact-fit allocation in the gap succeeds");

    mblockptr *b = hdr_of(p);
    CHECK(b->payload == align_up(request),
          "payload matches the aligned request exactly -- no split occurred, "
          "confirming this is the exact-fit branch and not a lucky reuse "
          "of a larger free block");
    CHECK(!list_is_linked(&b->list),
          "the exact-fit block is allocated, not sitting on the free list");
    CHECK(!is_mmap(b), "still served from the sbrk heap, not mmap (below MMAP_THRESHOLD)");

    size_t *footer = (size_t *)((char *)(b + 1) + b->payload);
    CHECK(*footer == b->payload, "footer is consistent with the exact-fit payload");

    fill_pattern(p, request, 0x9C);
    CHECK(check_pattern(p, request, 0x9C), "the full exact-fit payload is writable");

    my_free(p);

    /* Same scenario again, but this time via a *second* call after the */
    void *guard = my_malloc(64); /* occupies the freed block, forcing a fresh grow_top() call */
    void *q = my_malloc(CHUNK_SIZE + 500);
    CHECK(q != NULL, "second exact-fit allocation in the gap also succeeds");
    mblockptr *bq = hdr_of(q);
    CHECK(bq->payload == align_up((size_t)(CHUNK_SIZE + 500)),
          "second exact-fit block also lands with no split");

    my_free(q);
    my_free(guard);
}

static void test_many_extensions_stay_consistent(void)
{
    SECTION("repeated heap extension keeps the allocator consistent");

    enum
    {
        N = 64
    };
    void *ptrs[N];
    int all_ok = 1;
    int all_distinct = 1;

    for (int i = 0; i < N; i++)
    {
        ptrs[i] = my_malloc(CHUNK_SIZE / 2); /* forces several sbrk growths */
        if (!ptrs[i])
        {
            all_ok = 0;
            break;
        }
        memset(ptrs[i], (i & 0xFF), CHUNK_SIZE / 2);
    }
    CHECK(all_ok, "64 half-CHUNK_SIZE allocations all succeed across multiple sbrk growths");

    for (int i = 0; i < N && all_distinct; i++)
    {
        for (int j = i + 1; j < N; j++)
        {
            if (ptrs[i] == ptrs[j])
            {
                all_distinct = 0;
                break;
            }
        }
    }
    CHECK(all_distinct, "all 64 allocations are at distinct addresses");

    int data_ok = 1;
    for (int i = 0; i < N; i++)
    {
        unsigned char *b = ptrs[i];
        for (size_t j = 0; j < CHUNK_SIZE / 2; j += 512)
        {
            if (b[j] != (unsigned char)(i & 0xFF))
            {
                data_ok = 0;
                break;
            }
        }
        if (!data_ok)
            break;
    }
    CHECK(data_ok, "data survives intact across the extended heap region");

    for (int i = 0; i < N; i++)
        my_free(ptrs[i]);
}

/* ------------------------------------------------------------------ */
/* Edge case: realloc must actually relocate when it can't expand */
/* ------------------------------------------------------------------ */

static void test_realloc_forced_relocation(void)
{
    SECTION("my_realloc relocates when the next block is allocated");

    void *a = my_malloc(64);
    void *b = my_malloc(64); /* keeps 'a' from expanding forward */
    CHECK(a && b, "two adjacent allocations succeed");

    unsigned char *ca = a;
    fill_pattern(ca, 64, 0x42);

    void *a2 = my_realloc(a, 512);
    CHECK(a2 != NULL, "my_realloc to a larger size succeeds");
    CHECK(a2 != a, "my_realloc relocated the block (couldn't expand in place)");
    CHECK(check_pattern(a2, 64, 0x42), "original 64 bytes preserved after relocation");

    my_free(a2);
    my_free(b);
}

/* ------------------------------------------------------------------ */
/* Edge case: try_expand in-place growth (forward / backward / 3-way) */
/* ------------------------------------------------------------------ */

static void test_try_expand_forward_only(void)
{
    SECTION("try_expand: forward-only merge grows in place, no data move");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c, "three adjacent allocations succeed");

    fill_pattern(b, 64, 0x11);

    my_free(c); /* c becomes free, adjacent to b's forward side */

    void *b2 = my_realloc(b, 64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(b2 != NULL, "realloc requesting a b+c-sized region succeeds");
    CHECK(b2 == b, "forward-only merge does not relocate the block");
    CHECK(check_pattern(b2, 64, 0x11), "original bytes preserved (no move needed)");
    CHECK(!list_is_linked(&hdr_of(b2)->list),
          "merged block is allocated -- not sitting on the free list");

    my_free(a);
    my_free(b2);
}

static void test_try_expand_backward_only(void)
{
    SECTION("try_expand: backward-only merge relocates via memmove, data intact");

    /* SZ is deliberately bigger than HEADER_SIZE + FOOTER_SIZE, so the */
    enum
    {
        SZ = 256
    };

    void *a = my_malloc(SZ);
    void *b = my_malloc(SZ);
    void *c = my_malloc(SZ); /* blocks b's forward neighbor, so only the */
    CHECK(a && b && c, "three adjacent allocations succeed");

    unsigned char *bytes = b;
    for (size_t i = 0; i < SZ; i++)
        bytes[i] = (unsigned char)(i * 7 + 3);

    my_free(a); /* a becomes free, adjacent to b's backward side */

    void *b2 = my_realloc(b, SZ + SZ + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(b2 != NULL, "realloc requesting an a+b-sized region succeeds");
    CHECK(b2 == a, "backward-only merge relocates to the absorbed block's address");

    int intact = 1;
    for (size_t i = 0; i < SZ; i++)
    {
        if (((unsigned char *)b2)[i] != (unsigned char)(i * 7 + 3))
        {
            intact = 0;
            break;
        }
    }
    CHECK(intact, "every byte of the original payload survives the memmove, "
                  "including the region that overlapped with the old header");

    CHECK(!list_is_linked(&hdr_of(b2)->list),
          "absorbed-and-grown block is allocated -- not on the free list");
    CHECK(!is_free(hdr_of(b2)), "merged block's free bit is cleared");

    size_t *footer = (size_t *)((char *)(hdr_of(b2) + 1) + hdr_of(b2)->payload);
    CHECK(*footer == hdr_of(b2)->payload,
          "footer matches payload after backward merge (metadata self-consistent)");

    my_free(b2);
    my_free(c);
}

static void test_try_expand_three_way(void)
{
    SECTION("try_expand: three-way merge (prev+curr+next) relocates correctly");

    void *a = my_malloc(48);
    void *b = my_malloc(48);
    void *c = my_malloc(48);
    void *d = my_malloc(48); /* trailing blocker bounds the region */
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    fill_pattern(b, 48, 0x5A);

    my_free(a); /* isolated free block behind b */
    my_free(c); /* isolated free block ahead of b */

    void *big = my_realloc(b, 48 * 3 + HEADER_SIZE * 2 + FOOTER_SIZE * 2 - 8);
    CHECK(big != NULL, "realloc requesting the full a+b+c region succeeds");
    CHECK(big == a, "three-way merge relocates to the leftmost absorbed block");
    CHECK(check_pattern(big, 48, 0x5A), "original bytes preserved through the three-way merge");
    CHECK(!list_is_linked(&hdr_of(big)->list), "merged block is allocated -- not on the free list");

    my_free(big);
    my_free(d);
}

static void test_try_expand_split_after_merge(void)
{
    SECTION("try_expand: leftover after a merge gets split back into the free list");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c, "three adjacent allocations succeed");

    fill_pattern(b, 64, 0x99);
    my_free(c); /* free neighbor, far bigger than what we'll actually request */

    /* Ask for only slightly more than b's original size. The forward */
    void *b2 = my_realloc(b, 64 + align);
    CHECK(b2 == b, "small growth still expands in place via forward merge");
    CHECK(check_pattern(b2, 64, 0x99), "original bytes preserved");
    CHECK(hdr_of(b2)->payload < 64 + 64 + HEADER_SIZE + FOOTER_SIZE,
          "leftover space was split off, not left folded into b2's payload");

    void *reuse = my_malloc(align);
    CHECK(reuse != NULL, "split-off remainder is available for a fresh allocation");

    my_free(a);
    my_free(b2);
    my_free(reuse);
}

static void test_try_expand_insufficient_falls_back(void)
{
    SECTION("try_expand: merge that still wouldn't fit correctly declines");

    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    void *d = my_malloc(64); /* trailing blocker */
    CHECK(a && b && c && d, "four adjacent allocations succeed");

    fill_pattern(b, 64, 0x33);
    my_free(a);
    my_free(c); /* both neighbors free, but nowhere near enough combined -- */

    void *big = my_realloc(b, 8192);
    CHECK(big != NULL, "realloc for a size far beyond any local merge still succeeds "
                       "(falls back to malloc+copy+free)");
    CHECK(check_pattern(big, 64, 0x33), "original bytes preserved through the fallback path");

    my_free(big);
    my_free(d);
}

static void test_try_expand_no_prev_at_heap_start(void)
{
    SECTION("try_expand: no out-of-bounds read when there is no previous block");

    /* This runs as the first allocator call in its own forked child, which */
    void *p = my_malloc(64);
    void *guard = my_malloc(64); /* blocks the forward path too, so only the */
    CHECK(p && guard, "first two allocations of the heap succeed");

    fill_pattern(p, 64, 0x7A);

    void *p2 = my_realloc(p, 64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(p2 != NULL, "realloc succeeds even though no merge is possible "
                      "(no block exists before heap_start, next is allocated)");
    CHECK(check_pattern(p2, 64, 0x7A), "original bytes preserved regardless of path taken");

    my_free(p2);
    my_free(guard);
}

/* ------------------------------------------------------------------ */
/* Edge case: canary / guard bytes survive unrelated allocator churn */
/* ------------------------------------------------------------------ */

static void test_canary_survival(void)
{
    SECTION("canary bytes at payload edges survive unrelated churn");

    enum
    {
        N = 16,
        SZ = 40
    };
    void *ptrs[N];

    for (int i = 0; i < N; i++)
    {
        ptrs[i] = my_malloc(SZ);
        CHECK(ptrs[i] != NULL, "canary block allocated");
        unsigned char *b = ptrs[i];
        b[0] = (unsigned char)(0xC0 + i);      /* front canary */
        b[SZ - 1] = (unsigned char)(0xD0 + i); /* back canary */
    }

    /* Churn: allocate/free a bunch of unrelated blocks of varying sizes */
    for (int round = 0; round < 8; round++)
    {
        void *tmp[8];
        for (int i = 0; i < 8; i++)
        {
            tmp[i] = my_malloc((size_t)(16 + round * 8 + i * 4));
            if (tmp[i])
                memset(tmp[i], 0xFF, 16 + round * 8 + i * 4);
        }
        for (int i = 0; i < 8; i++)
            my_free(tmp[i]);
    }

    int intact = 1;
    for (int i = 0; i < N; i++)
    {
        unsigned char *b = ptrs[i];
        if (b[0] != (unsigned char)(0xC0 + i) || b[SZ - 1] != (unsigned char)(0xD0 + i))
        {
            intact = 0;
            vlog("canary corrupted for block %d", i);
        }
    }
    CHECK(intact, "all front/back canaries intact after unrelated allocator churn");

    for (int i = 0; i < N; i++)
        my_free(ptrs[i]);
}

/* ------------------------------------------------------------------ */
/* Boundary: heap shrinkage must never retreat past the starting break */
/* ------------------------------------------------------------------ */

static void test_heap_shrink_boundary(void)
{
    SECTION("heap shrinkage never retreats past the starting program break");

    /* heap_init() only runs once, in main(), before any test is forked. */
    void *heap_floor = sbrk(0);

    enum
    {
        N = 3
    };
    void *ptrs[N];
    int all_ok = 1;
    size_t big = MMAP_THRESHOLD - CHUNK_SIZE;

    for (int i = 0; i < N; i++)
    {
        ptrs[i] = my_malloc(big);
        if (!ptrs[i])
            all_ok = 0;
    }
    CHECK(all_ok, "three large (MMAP_THRESHOLD - CHUNK_SIZE) allocations succeed, "
                  "exceeding heap_init()'s initial reserve and forcing new sbrk growth");

    void *heap_grown = sbrk(0);
    CHECK(heap_grown > heap_floor,
          "heap actually grew past the floor (sanity check: the test is exercising growth)");

    /* Free everything. The last free() of a big trailing block is the */
    for (int i = 0; i < N; i++)
    {
        my_free(ptrs[i]);
    }

    void *heap_final = sbrk(0);
    CHECK(heap_final >= heap_floor,
          "program break after full shrinkage never dips below the starting heap address");

    vlog("heap_floor=%p heap_grown=%p heap_final=%p", heap_floor, heap_grown, heap_final);
}

/* ------------------------------------------------------------------ */
/* Threshold transition: MMAP_THRESHOLD routes to mmap, not sbrk */
/* ------------------------------------------------------------------ */

static void test_mmap_threshold_transitions(void)
{
    SECTION("MMAP_THRESHOLD boundary routes to mmap, and resizing tracks it");

    /* One alignment step under threshold: must stay on the sbrk/heap */
    void *below = my_malloc(MMAP_THRESHOLD - align);
    CHECK(below != NULL, "allocation just under MMAP_THRESHOLD succeeds");
    CHECK(!is_mmap(hdr_of(below)),
          "size just under MMAP_THRESHOLD is served from the sbrk heap, not mmap");

    /* Exactly at threshold: this is the documented cutover point. */
    void *at = my_malloc(MMAP_THRESHOLD);
    CHECK(at != NULL, "allocation of exactly MMAP_THRESHOLD succeeds");
    CHECK(is_mmap(hdr_of(at)), "size == MMAP_THRESHOLD is routed to mmap");
    CHECK(ptr_is_aligned(at), "mmap'd payload is still correctly aligned");

    fill_pattern(at, MMAP_THRESHOLD, 0x3C);

    /* Grow further: a correct implementation should extend the existing */
    void *grown = my_realloc(at, MMAP_THRESHOLD * 2);
    CHECK(grown != NULL, "growing an mmap'd block succeeds");
    CHECK(is_mmap(hdr_of(grown)),
          "growing an already-mmap'd block keeps it mmap-backed (mremap path)");
    CHECK(check_pattern(grown, MMAP_THRESHOLD, 0x3C),
          "original bytes survive the grow-resize");

    /* Shrink back under the threshold. Whether your implementation */
    void *shrunk = my_realloc(grown, 64);
    CHECK(shrunk != NULL, "shrinking an mmap'd block down succeeds");
    CHECK(check_pattern(shrunk, 64, 0x3C), "surviving bytes preserved through the shrink");

    my_free(below);
    my_free(shrunk);
}

/* ------------------------------------------------------------------ */
/* Structural poisoning: footer size must always match header payload */
/* ------------------------------------------------------------------ */

/* Reads the boundary-tag footer that sits right after a block's payload */
static size_t block_footer_value(mblockptr *b)
{
    return *(size_t *)((char *)(b + 1) + b->payload);
}

static void test_footer_payload_consistency(void)
{
    SECTION("footer sizes match header payloads (boundary tag correctness)");

    enum
    {
        N = 6
    };
    static const size_t sizes[N] = {8, 40, 128, 500, 1024, 3000};
    void *ptrs[N];

    int footers_ok = 1;
    for (int i = 0; i < N; i++)
    {
        ptrs[i] = my_malloc(sizes[i]);
        if (!ptrs[i])
        {
            footers_ok = 0;
            continue;
        }
        mblockptr *b = hdr_of(ptrs[i]);
        size_t footer = block_footer_value(b);
        if (footer != b->payload)
            footers_ok = 0;
        vlog("requested=%zu payload=%zu footer=%zu", sizes[i], b->payload, footer);
    }
    CHECK(footers_ok,
          "footer size_t immediately after each payload equals that block's header->payload");

    /* Free a couple of interior blocks, which forces split/coalesce to */
    my_free(ptrs[1]);
    my_free(ptrs[3]);

    mblockptr *survivor = hdr_of(ptrs[2]);
    CHECK(survivor->payload == sizes[2],
          "untouched neighbor block's payload is unaffected by neighboring frees");

    int list_ok = 1;
    for (int i = 0; i < N; i++)
    {
        if (i == 1 || i == 3)
            continue;
        if (list_is_linked(&hdr_of(ptrs[i])->list))
            list_ok = 0; /* allocated => never on free list */
    }
    CHECK(list_ok, "remaining allocated blocks are not linked into the free list after neighbor frees");

    /* Direct doubly-linked-list symmetry check on a freed node: this is */
    void *fp = my_malloc(64);
    my_free(fp);
    mblockptr *fb = hdr_of(fp);
    CHECK(fb->list.next->prev == &fb->list && fb->list.prev->next == &fb->list,
          "freed block's list node is symmetrically linked in both directions");

    for (int i = 0; i < N; i++)
    {
        if (i == 1 || i == 3)
            continue;
        my_free(ptrs[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Fuzz-style stress test with checksummed data and mixed operations */
/* ------------------------------------------------------------------ */

typedef struct
{
    void *ptr;
    size_t size;
    unsigned char seed;
    int live;
} tracked_t;

static void test_fuzz_mixed_ops_seeded(unsigned seed)
{
    SECTION("fuzz: mixed my_malloc/my_realloc/my_free with checksum verification");
    printf("  (seed = %u -- rerun with this seed to reproduce exactly)\n", seed);

    srand(seed);

    enum
    {
        SLOTS = 128,
        OPS = 2000
    };
    static tracked_t slots[SLOTS];
    memset(slots, 0, sizeof(slots));

    int corruption_found = 0;
    int op_failures = 0;

    for (int op = 0; op < OPS; op++)
    {
        int idx = rand() % SLOTS;
        int action = rand() % 4; /* 0=alloc,1=free,2=realloc,3=alloc */

        if (slots[idx].live)
        {
            /* verify before mutating/removing */
            if (!check_pattern(slots[idx].ptr, slots[idx].size, slots[idx].seed))
            {
                corruption_found = 1;
            }
        }

        switch (action)
        {
        case 1: /* free */
            if (slots[idx].live)
            {
                my_free(slots[idx].ptr);
                slots[idx].live = 0;
            }
            break;

        case 2: /* realloc (or alloc if empty) */
            if (slots[idx].live)
            {
                size_t newsize = (size_t)(rand() % 2000) + 1;
                void *np = my_realloc(slots[idx].ptr, newsize);
                if (!np)
                {
                    op_failures++;
                    break;
                }
                slots[idx].ptr = np;
                slots[idx].size = newsize;
                slots[idx].seed = (unsigned char)rand();
                fill_pattern(np, newsize, slots[idx].seed);
                break;
            }
            /* fallthrough to alloc if slot was empty */
            __attribute__((fallthrough));
        default:
        { /* 0, 3, and realloc-fallthrough: fresh allocation */
            if (slots[idx].live)
                my_free(slots[idx].ptr);
            size_t sz = (size_t)(rand() % 2000) + 1;
            void *p = my_malloc(sz);
            if (!p)
            {
                op_failures++;
                slots[idx].live = 0;
                break;
            }
            unsigned char sd = (unsigned char)rand();
            fill_pattern(p, sz, sd);
            slots[idx] = (tracked_t){.ptr = p, .size = sz, .seed = sd, .live = 1};
            break;
        }
        }
    }

    /* final integrity pass over everything still live */
    for (int i = 0; i < SLOTS; i++)
    {
        if (slots[i].live)
        {
            if (!check_pattern(slots[i].ptr, slots[i].size, slots[i].seed))
            {
                corruption_found = 1;
            }
        }
    }

    CHECK(!corruption_found, "no data corruption across 2000 randomized ops");
    CHECK(op_failures == 0, "no unexpected allocation failures during fuzz run");

    /* Design invariant: allocated blocks are never linked into the free */
    int invariant_ok = 1;
    for (int i = 0; i < SLOTS; i++)
    {
        if (slots[i].live && list_is_linked(&hdr_of(slots[i].ptr)->list))
        {
            invariant_ok = 0;
        }
    }
    CHECK(invariant_ok, "every live allocation is off the free list "
                        "(allocated-never-on-list invariant holds)");

    for (int i = 0; i < SLOTS; i++)
    {
        if (slots[i].live)
            my_free(slots[i].ptr);
    }
}

/* TestCase.fn is void(*)(void), but the fuzz test wants a seed. This thin */
static void test_fuzz_mixed_ops(void)
{
    test_fuzz_mixed_ops_seeded((unsigned)time(NULL));
}

/* ------------------------------------------------------------------ */
/* Edge case: three-way merge must not leave a ghost/stale list node */
/* ------------------------------------------------------------------ */

static void test_try_expand_three_way_no_ghost_node(void)
{
    SECTION("try_expand three-way merge: no stale list node survives the merge");

    /* Same a/b/c/d shape as test_try_expand_three_way(), but here we */
    void *a = my_malloc(48);
    void *b = my_malloc(48);
    void *c = my_malloc(48);
    void *d = my_malloc(48); /* trailing blocker bounds the region */
    void *e = my_malloc(48); /* stays free the whole time: seeds the ring walk */
    void *guard = my_malloc(align); /* blocks e from being absorbed into the top chunk when freed */
    CHECK(a && b && c && d && e && guard, "six adjacent allocations succeed");

    fill_pattern(b, 48, 0x5A);

    my_free(e); /* known-free anchor to start the ring walk from */
    my_free(a); /* isolated free block behind b */
    my_free(c); /* isolated free block ahead of b */

    watchdog_arm(5);
    void *big = my_realloc(b, 48 * 3 + HEADER_SIZE * 2 + FOOTER_SIZE * 2 - 8);
    watchdog_disarm();

    CHECK(big != NULL, "realloc requesting the full a+b+c region succeeds");
    CHECK(big == a, "three-way merge relocates to the leftmost absorbed block");
    CHECK(check_pattern(big, 48, 0x5A), "original bytes preserved through the three-way merge");
    CHECK(!list_is_linked(&hdr_of(big)->list), "merged block is allocated -- not on the free list");

    watchdog_arm(5);
    int found_exactly_e = bin_holds_exactly((const malloc_state *)debug_get_state(), hdr_of(e));
    watchdog_disarm();

    CHECK(found_exactly_e,
          "'e' is the sole entry in its bin -- no ghost node left behind by the merge, "
          "no lost/duplicated entries");

    my_free(big);
    my_free(d); /* 'd' is adjacent to 'e' and forward-coalesces with it here -- */
    my_free(guard);
}

/* ------------------------------------------------------------------ */
/* Edge case: scattering frees then immediately reallocating must not */
/* ------------------------------------------------------------------ */

static void test_scattered_free_then_realloc_stays_consistent(void)
{
    SECTION("scattered free + immediate malloc stays consistent (no rover in this design -- "
            "the risk here is find_suitable_block()'s bin scan and insert_large_chunk()'s "
            "sorted insert, not a stale scan cursor)");

    /* This test's original version (previous, non-bin design) targeted a */

    enum
    {
        N = 12,
        SZ = 40,
        ROUNDS = 20
    };
    void *ptrs[N];
    unsigned char seeds[N];
    int all_ok = 1;
    int data_ok = 1;

    for (int round = 0; round < ROUNDS; round++)
    {
        for (int i = 0; i < N; i++)
        {
            ptrs[i] = my_malloc(SZ);
            if (!ptrs[i])
            {
                all_ok = 0;
                break;
            }
            seeds[i] = (unsigned char)(round * N + i);
            fill_pattern(ptrs[i], SZ, seeds[i]);
        }
        if (!all_ok)
            break;

        /* Scatter frees so several independent (non-adjacent) free-list */
        for (int i = 0; i < N; i += 2)
        {
            my_free(ptrs[i]);
            ptrs[i] = NULL;
        }

        /* Immediately allocate again -- this is the moment a corrupted */
        watchdog_arm(5);
        void *fresh1 = my_malloc(SZ);
        watchdog_disarm();
        if (!fresh1)
        {
            all_ok = 0;
            break;
        }
        unsigned char fresh1_seed = (unsigned char)(0xE0 + round);
        fill_pattern(fresh1, SZ, fresh1_seed);

        /* Now free one of the *odd* survivors, then allocate again right */
        int victim = 1; /* an odd index, guaranteed still allocated */
        my_free(ptrs[victim]);
        ptrs[victim] = NULL;

        watchdog_arm(5);
        void *fresh2 = my_malloc(SZ);
        watchdog_disarm();
        if (!fresh2)
        {
            all_ok = 0;
            break;
        }
        unsigned char fresh2_seed = (unsigned char)(0xB0 + round);
        fill_pattern(fresh2, SZ, fresh2_seed);

        /* Verify every surviving block's data (not just the freshly */
        for (int i = 0; i < N; i++)
        {
            if (ptrs[i] && !check_pattern(ptrs[i], SZ, seeds[i]))
            {
                data_ok = 0;
            }
        }
        if (!check_pattern(fresh1, SZ, fresh1_seed))
            data_ok = 0;
        if (!check_pattern(fresh2, SZ, fresh2_seed))
            data_ok = 0;

        my_free(fresh1);
        my_free(fresh2);
        for (int i = 0; i < N; i++)
        {
            if (ptrs[i])
            {
                my_free(ptrs[i]);
                ptrs[i] = NULL;
            }
        }
    }

    CHECK(all_ok, "all allocations across every round succeeded (no hang, no failure)");
    CHECK(data_ok, "no data corruption from scattered free/realloc churn");
}

/* ------------------------------------------------------------------ */
/* Edge case: size_t overflow guard at the align_up() boundary */
/* ------------------------------------------------------------------ */

static void test_malloc_size_overflow_guard(void)
{
    SECTION("size_t overflow guard around SIZE_MAX rejects unsatisfiable requests");

    /* my_malloc() rejects anything where `size >= SIZE_MAX - (align - 1)`, */
    size_t just_over = __SIZE_MAX__ - (align - 1); /* first rejected value */
    size_t just_under = just_over - 1;             /* largest nominally-allowed value */

    CHECK(my_malloc(just_over) == NULL,
          "size right at the align_up overflow boundary is rejected");
    CHECK(my_malloc(__SIZE_MAX__) == NULL,
          "SIZE_MAX itself is rejected");

    /* just_under passes the align_up guard, but is still astronomically */
    void *p = my_malloc(just_under);
    if (p != NULL)
    {
        vlog("my_malloc(SIZE_MAX - align) unexpectedly succeeded -- "
             "environment has an implausible amount of virtual memory");
        my_free(p);
    }
    CHECK(1, "near-SIZE_MAX request that passes the guard does not crash the allocator");
}

/* ------------------------------------------------------------------ */
/* BUG (found during this review): integer overflow in the mmap-path */
/* ------------------------------------------------------------------ */

static void test_malloc_mmap_path_integer_overflow(void)
{
    SECTION("KNOWN BUG: mmap-path total_need calculation can overflow silently");

    /* my_malloc()'s overflow guard only protects align_up(size). The */
    size_t evil = __SIZE_MAX__ - align - 8; /* passes align_up's guard */

    void *p = my_malloc(evil);
    if (p == NULL)
    {
        CHECK(1, "mmap path correctly rejects a request whose header+footer "
                 "accounting would overflow size_t");
    }
    else
    {
        mblockptr *b = hdr_of(p);
        vlog("evil malloc succeeded: payload=%zu requested=%zu", b->payload, evil);
        CHECK(b->payload >= evil,
              "if my_malloc() reports success, the buffer must be at least as "
              "large as requested -- a smaller payload means the size_t "
              "addition wrapped and mmap'd an undersized buffer instead of "
              "failing (heap-buffer-overflow-in-waiting for the caller)");
        my_free(p);
    }
}

/* ------------------------------------------------------------------ */
/* Edge case: realloc to the same (already-satisfied) size is a no-op */
/* ------------------------------------------------------------------ */

static void test_realloc_same_size_noop(void)
{
    SECTION("realloc to a size already satisfied by the current block is a no-op");

    void *p = my_malloc(200);
    CHECK(p != NULL, "malloc(200) succeeds");
    fill_pattern(p, 200, 0x61);

    size_t original_payload = hdr_of(p)->payload;

    /* Request exactly the block's current usable payload -- this must */
    void *p2 = my_realloc(p, original_payload);
    CHECK(p2 == p, "realloc to the exact current payload returns the same pointer");
    CHECK(hdr_of(p2)->payload == original_payload,
          "payload is unchanged when the request already fits exactly");
    CHECK(check_pattern(p2, 200, 0x61), "data untouched by the no-op realloc");

    my_free(p2);
}

/* ------------------------------------------------------------------ */
/* Edge case: realloc(ptr, 0) frees -- a second free must still be */
/* ------------------------------------------------------------------ */

static void child_double_free_via_realloc_zero(void)
{
    void *p = my_malloc(48);
    void *r = my_realloc(p, 0); /* frees p internally, returns NULL */
    (void)r;
    my_free(p); /* p is already free -- must abort */
    _exit(1);   /* only reached if double-free went undetected */
}

static void test_double_free_after_realloc_zero(void)
{
    SECTION("realloc(ptr, 0) marks the block free; freeing it again is still caught");

    int signo = 0;
    int result = run_isolated(child_double_free_via_realloc_zero, &signo);

    CHECK(result == -1 && signo == SIGABRT,
          "freeing a pointer that was already released via realloc(ptr, 0) "
          "aborts, same as any other double free");
}

/* ------------------------------------------------------------------ */
/* Concurrency: global_lock must actually protect concurrent callers */
/* ------------------------------------------------------------------ */

#include <pthread.h>

typedef struct
{
    int thread_id;
    int ops;
    int failures;
} thread_arg_t;

static void *concurrent_worker(void *arg)
{
    thread_arg_t *ta = arg;
    unsigned seed = (unsigned)(ta->thread_id * 7919 + 13);

    enum
    {
        SLOTS = 16
    };
    void *ptrs[SLOTS] = {0};
    size_t sizes[SLOTS] = {0};
    unsigned char seeds[SLOTS] = {0};

    for (int op = 0; op < ta->ops; op++)
    {
        seed = seed * 1103515245 + 12345;
        int idx = (int)(seed % SLOTS);
        seed = seed * 1103515245 + 12345;

        if (ptrs[idx])
        {
            if (!check_pattern(ptrs[idx], sizes[idx], seeds[idx]))
            {
                ta->failures++;
            }
            my_free(ptrs[idx]);
            ptrs[idx] = NULL;
        }

        size_t sz = (seed % 400) + 1;
        void *p = my_malloc(sz);
        if (!p)
        {
            ta->failures++;
            continue;
        }
        unsigned char sd = (unsigned char)(ta->thread_id * 31 + op);
        fill_pattern(p, sz, sd);
        ptrs[idx] = p;
        sizes[idx] = sz;
        seeds[idx] = sd;
    }

    for (int i = 0; i < SLOTS; i++)
    {
        if (ptrs[i])
        {
            if (!check_pattern(ptrs[i], sizes[i], seeds[i]))
                ta->failures++;
            my_free(ptrs[i]);
        }
    }

    return NULL;
}

static void test_concurrent_alloc_free(void)
{
    SECTION("thread safety: concurrent malloc/free under global_lock stays consistent");

    /* This is a basic smoke test for the mutex, not a substitute for */
    enum
    {
        NTHREADS = 6,
        OPS_PER_THREAD = 500
    };
    pthread_t threads[NTHREADS];
    thread_arg_t args[NTHREADS];

    watchdog_arm(20);
    for (int i = 0; i < NTHREADS; i++)
    {
        args[i] = (thread_arg_t){.thread_id = i, .ops = OPS_PER_THREAD, .failures = 0};
        int rc = pthread_create(&threads[i], NULL, concurrent_worker, &args[i]);
        CHECK(rc == 0, "thread creation succeeds");
    }

    int total_failures = 0;
    for (int i = 0; i < NTHREADS; i++)
    {
        pthread_join(threads[i], NULL);
        total_failures += args[i].failures;
    }
    watchdog_disarm();

    CHECK(total_failures == 0,
          "no data corruption or allocation failures across concurrent threads");

    /* After all threads finish, the allocator itself should still be */
    void *p = my_malloc(128);
    CHECK(p != NULL, "allocator remains usable after concurrent stress");
    my_free(p);
}

/* ------------------------------------------------------------------ */
/* Regression: realloc growing a near-threshold sbrk block past */
/* ------------------------------------------------------------------ */

static void test_realloc_large_growth_from_near_threshold_uses_mmap(void)
{
    SECTION("realloc: growing a near-threshold sbrk block past MMAP_THRESHOLD converts to mmap");

    /* Regression guard for a real bug found in review: my_realloc()'s */

    /* Consume the entire initial free block so nothing free is left */
    size_t initial_free_payload = MMAP_THRESHOLD - HEADER_SIZE - FOOTER_SIZE;
    void *filler = my_malloc(initial_free_payload);
    CHECK(filler != NULL, "filler consumes the entire initial free block");

    /* Land a legitimate sbrk block just under MMAP_THRESHOLD. */
    void *p = my_malloc(130000);
    CHECK(p != NULL, "near-threshold allocation succeeds");
    CHECK(!is_mmap(hdr_of(p)), "near-threshold allocation is sbrk-backed, as expected");

    fill_pattern(p, 130000, 0x4E);

    /* Grow it well past MMAP_THRESHOLD. */
    void *p2 = my_realloc(p, 200000);
    CHECK(p2 != NULL, "realloc growing past MMAP_THRESHOLD succeeds");

    mblockptr *b2 = hdr_of(p2);
    CHECK(is_mmap(b2),
          "block is now mmap-backed after crossing MMAP_THRESHOLD -- "
          "matches the backing strategy a fresh my_malloc(200000) would use");
    CHECK(b2->payload >= 200000,
          "mmap'd block is at least as large as requested");
    CHECK(check_pattern(p2, 130000, 0x4E),
          "original data survives the sbrk-to-mmap handoff");

    my_free(p2);
    my_free(filler);
}

/* ------------------------------------------------------------------ */
/* Registry -- add new tests here, nowhere else. */
/* ------------------------------------------------------------------ */

typedef void (*test_fn)(void);

typedef struct
{
    const char *name;
    test_fn fn;
} TestCase;

static const TestCase tests[] = {
    {"NULL handling", test_null_handling},
    {"double free detection", test_double_free},
    {"alignment guarantees", test_alignment},
    {"zero-size variants", test_zero_size_variants},
    {"split threshold", test_split_threshold},
    {"forward coalescing", test_forward_coalesce},
    {"backward coalescing", test_backward_coalesce},
    {"three-way coalescing", test_three_way_coalesce},
    {"large allocation extends heap", test_large_allocation_extends_heap},
    {"exact-fit grow_top does not split (CHUNK_SIZE..MMAP_THRESHOLD gap)",
     test_extend_heap_exact_fit_no_split},
    {"many extensions stay consistent", test_many_extensions_stay_consistent},
    {"realloc forced relocation", test_realloc_forced_relocation},
    {"try_expand forward-only merge", test_try_expand_forward_only},
    {"try_expand backward-only merge", test_try_expand_backward_only},
    {"try_expand three-way merge", test_try_expand_three_way},
    {"try_expand split after merge", test_try_expand_split_after_merge},
    {"try_expand insufficient merge falls back", test_try_expand_insufficient_falls_back},
    {"try_expand no-prev heap-start boundary", test_try_expand_no_prev_at_heap_start},
    {"try_expand three-way merge leaves no ghost node", test_try_expand_three_way_no_ghost_node},
    {"scattered free + immediate malloc stays consistent", test_scattered_free_then_realloc_stays_consistent},
    {"canary survival", test_canary_survival},
    {"heap shrink boundary", test_heap_shrink_boundary},
    {"mmap threshold transitions", test_mmap_threshold_transitions},
    {"footer/payload consistency", test_footer_payload_consistency},
    {"size_t overflow guard", test_malloc_size_overflow_guard},
    {"mmap-path integer overflow", test_malloc_mmap_path_integer_overflow},
    {"realloc to same size is a no-op", test_realloc_same_size_noop},
    {"realloc near-threshold growth converts to mmap",
     test_realloc_large_growth_from_near_threshold_uses_mmap},
    {"double free after realloc(ptr,0)", test_double_free_after_realloc_zero},
    {"concurrent alloc/free thread safety", test_concurrent_alloc_free},
    {"fuzz: mixed ops", test_fuzz_mixed_ops},
};

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
        {
            g_verbose = 1;
        }
    }
    enable_color_if_tty();

    /* Unbuffered stdout matters here: every test's child exits via */
    setvbuf(stdout, NULL, _IONBF, 0);

    heap_init();

    /* Shared memory so every forked child's CHECK() results (including */
    counters = mmap(NULL, sizeof(SharedCounters), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    counters->checks_run = 0;
    counters->checks_failed = 0;

    size_t num_tests = sizeof(tests) / sizeof(tests[0]);
    int suites_failed = 0;
    int suites_crashed = 0;

    for (size_t i = 0; i < num_tests; i++)
    {
        fflush(stdout); /* flush before fork so the child doesn't inherit a stale buffer */

        pid_t pid = fork();
        if (pid == 0)
        {
            /* child: run exactly one test, then report pass/fail via exit code */
            current_test_failed = 0;
            tests[i].fn();
            _exit(current_test_failed ? 1 : 0);
        }

        int status;
        waitpid(pid, &status, 0);

        if (WIFSIGNALED(status))
        {
            printf("  %s[CRASH]%s '%s' terminated by signal %d (%s)\n",
                   COL_RED, COL_RESET, tests[i].name,
                   WTERMSIG(status), strsignal(WTERMSIG(status)));
            suites_crashed++;
            suites_failed++;
        }
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        {
            suites_failed++;
        }
    }

    printf("\n=====================================\n");
    if (counters->checks_failed == 0 && suites_failed == 0)
    {
        printf("%s%d/%d checks passed across %zu test suites%s\n",
               COL_GREEN, counters->checks_run, counters->checks_run, num_tests, COL_RESET);
    }
    else
    {
        printf("%s%d/%d checks passed across %zu test suites%s\n",
               COL_RED, counters->checks_run - counters->checks_failed,
               counters->checks_run, num_tests, COL_RESET);
    }
    printf("%d suite%s failed", suites_failed, suites_failed == 1 ? "" : "s");
    if (suites_crashed)
    {
        printf(" (%d crashed the test process)", suites_crashed);
    }
    printf("\n=====================================\n");

    return suites_failed == 0 ? 0 : 1;
}