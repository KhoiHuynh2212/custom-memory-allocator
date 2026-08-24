#include "../include/my-malloc.h"
#include "../src/debug.h"
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>


typedef struct {
    int checks_run;
    int checks_failed;
} SharedCounters;

static SharedCounters *counters;
static int current_test_failed = 0;

#define CHECK(cond, msg)                                              \
    do {                                                               \
        counters->checks_run++;                                       \
        if (cond) {                                                    \
            printf("  [PASS] %s\n", msg);                              \
        } else {                                                       \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);         \
            counters->checks_failed++;                                 \
            current_test_failed++;                                    \
        }                                                               \
    } while (0)

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} TestCase;

static void fill_pattern(void *buf, size_t n, unsigned char pattern)
{
    memset(buf, pattern, n);
}

static bool verify_pattern(const void *buf, size_t n, unsigned char pattern)
{
    const unsigned char *bytes = buf;
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] != pattern) return false;
    }
    return true;
}

static void test_basic_alloc(void)
{
    void *p = my_malloc(64);
    CHECK(p != NULL, "malloc(64) returns non-NULL");

    fill_pattern(p, 64, 0xAB);
    CHECK(verify_pattern(p, 64, 0xAB), "allocated memory is fully writable");

    my_free(p);
    CHECK(1, "my_free() does not crash");
}

static void test_malloc_zero(void)
{
    void *p = my_malloc(0);
    CHECK(p == NULL, "malloc(0) returns NULL");
}

static void test_distinct_blocks_no_overlap(void)
{
    void *a = my_malloc(32);
    void *b = my_malloc(32);
    void *c = my_malloc(32);

    CHECK(a && b && c, "three allocations succeed");
    CHECK(a != b && b != c && a != c, "pointers are distinct");

    fill_pattern(a, 32, 0x11);
    fill_pattern(b, 32, 0x22);
    fill_pattern(c, 32, 0x33);

    CHECK(verify_pattern(a, 32, 0x11) &&
          verify_pattern(b, 32, 0x22) &&
          verify_pattern(c, 32, 0x33),
          "writes to one block don't clobber neighbors");

    my_free(a);
    my_free(b);
    my_free(c);
}

static void test_free_list_reuse(void)
{
    /* A guard allocation right after p1 is required: without it, freeing
     * p1 as the very first allocation would coalesce straight into the
     * top chunk (next == topchunkptr) and never touch a bin at all — the
     * test would still pass, but for the wrong reason (top-chunk recarve,
     * not free-list/bin reuse). The guard keeps `next` non-top so free(p1)
     * is forced through insert_small_chunk() into a real bin. */
    void *p1 = my_malloc(128);
    CHECK(p1 != NULL, "first malloc(128) succeeds");

    void *guard = my_malloc(128);
    CHECK(guard != NULL, "guard allocation succeeds");

    my_free(p1);

    void *p2 = my_malloc(128);
    CHECK(p2 == p1, "malloc(128) after free reuses the same address from its bin");

    my_free(p2);
    my_free(guard);
}

static void test_calloc_zeroes(void)
{
    size_t n = 50;
    unsigned char *p = my_calloc(n, sizeof(unsigned char));
    CHECK(p != NULL, "calloc succeeds");
    CHECK(verify_pattern(p, n, 0x00), "all bytes are zero");

    void *huge = my_calloc((size_t)-1, 2);
    CHECK(huge == NULL, "calloc detects multiplication overflow");

    my_free(p);
}

static void test_realloc_grow_preserves_data(void)
{
    char *p = my_malloc(16);
    CHECK(p != NULL, "malloc(16) succeeds");
    memcpy(p, "hello world!!", 14);

    char *p2 = my_realloc(p, 256);
    CHECK(p2 != NULL, "realloc to larger size succeeds");
    CHECK(memcmp(p2, "hello world!!", 14) == 0, "original data preserved after grow");

    my_free(p2);
}

static void test_realloc_shrink(void)
{
    char *p = my_malloc(256);
    CHECK(p != NULL, "malloc(256) succeeds");
    memcpy(p, "shrink me", 10);

    char *p2 = my_realloc(p, 16);
    CHECK(p2 != NULL, "realloc to smaller size succeeds");
    CHECK(memcmp(p2, "shrink me", 10) == 0, "data preserved after shrink");

    my_free(p2);
}

static void test_realloc_null_acts_like_malloc(void)
{
    void *p = my_realloc(NULL, 40);
    CHECK(p != NULL, "realloc(NULL, 40) returns non-NULL");
    my_free(p);
}

static void test_realloc_zero_acts_like_free(void)
{
    void *p = my_malloc(40);
    void *r = my_realloc(p, 0);
    CHECK(r == NULL, "realloc(ptr, 0) returns NULL");
}

static void test_coalescing(void)
{
    void *a = my_malloc(64);
    void *b = my_malloc(64);
    void *c = my_malloc(64);
    CHECK(a && b && c, "three allocations for coalesce test succeed");

    my_free(a);
    my_free(b); /* freeing a then b should merge them into one free block */

    void *big = my_malloc(64 + 64 + HEADER_SIZE + FOOTER_SIZE - 8);
    CHECK(big != NULL, "allocation fitting only in the merged a+b region succeeds");
    CHECK(big == a, "merged region reused starting at freed block 'a'");

    my_free(big);
    my_free(c);
}

static void test_alignment(void)
{
    /* Odd, non-power-of-two sizes are the ones most likely to expose an
     * off-by-one in ALIGN_UP(). Every pointer malloc hands back must be
     * usable to store *any* type, including things like long double or
     * SSE types that need max_align_t alignment — misaligned access can
     * crash on some architectures, not just be "a bit slow." */
    void *p1 = my_malloc(1);
    void *p2 = my_malloc(3);
    void *p3 = my_malloc(17);
    CHECK(p1 && p2 && p3, "odd-sized allocations succeed");
    CHECK(((uintptr_t)p1 % align) == 0, "malloc(1) pointer is properly aligned");
    CHECK(((uintptr_t)p2 % align) == 0, "malloc(3) pointer is properly aligned");
    CHECK(((uintptr_t)p3 % align) == 0, "malloc(17) pointer is properly aligned");

    my_free(p1);
    my_free(p2);
    my_free(p3);
}

static void test_mmap_large_alloc(void)
{
    /* Anything at or above MMAP_THRESHOLD takes a completely different
     * code path (mmap/munmap instead of the sbrk free-list). That path
     * is easy to leave under-tested since the small-allocation tests
     * never touch it at all. */
    size_t big_size = MMAP_THRESHOLD + 4096;

    void *p = my_malloc(big_size);
    CHECK(p != NULL, "allocation above mmap threshold succeeds");

    fill_pattern(p, big_size, 0x77);
    CHECK(verify_pattern(p, big_size, 0x77), "large mmap'd allocation is fully writable");

    my_free(p);
    CHECK(1, "freeing (munmap) a large allocation does not crash");
}

static void test_realloc_crosses_mmap_threshold(void)
{
    /* This exercises the boundary between the two allocation strategies:
     * realloc() starts on a small sbrk'd block, then has to grow past
     * MMAP_THRESHOLD, which means it can't just extend in place — it has
     * to switch strategies entirely (alloc new mmap region, copy, free
     * the old sbrk block). That handoff is exactly where data-copying
     * bugs like wrong copy size tend to hide. */
    const char *msg = "cross the threshold";
    char *p = my_malloc(64);
    CHECK(p != NULL, "small malloc(64) succeeds");
    memcpy(p, msg, strlen(msg) + 1); /* +1 for the null terminator */

    char *p2 = my_realloc(p, MMAP_THRESHOLD + 4096);
    CHECK(p2 != NULL, "realloc growing past the mmap threshold succeeds");
    CHECK(memcmp(p2, msg, strlen(msg) + 1) == 0,
          "data preserved when growing across into mmap territory");

    my_free(p2);
}

static void test_stress(void)
{
    enum { N = 200 };
    void *ptrs[N] = {0};
    size_t sizes[N] = {0};
    unsigned seed = 12345;

    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245 + 12345;
        size_t sz = (seed % 500) + 1;
        ptrs[i] = my_malloc(sz);
        sizes[i] = sz;
        if (ptrs[i]) fill_pattern(ptrs[i], sz, (unsigned char)(i & 0xFF));
    }

    int alloc_ok = 1;
    for (int i = 0; i < N; i++) {
        if (!ptrs[i]) { alloc_ok = 0; break; }
    }
    CHECK(alloc_ok, "200 varied-size allocations all succeeded");

    int data_ok = 1;
    for (int i = 0; i < N; i++) {
        if (!verify_pattern(ptrs[i], sizes[i], (unsigned char)(i & 0xFF))) {
            data_ok = 0;
            break;
        }
    }
    CHECK(data_ok, "no cross-contamination between the 200 blocks");

    for (int i = 0; i < N; i += 2) {
        my_free(ptrs[i]);
        ptrs[i] = NULL;
    }

    int realloc_ok = 1;
    for (int i = 0; i < N; i += 2) {
        seed = seed * 1103515245 + 12345;
        size_t sz = (seed % 500) + 1;
        ptrs[i] = my_malloc(sz);
        if (!ptrs[i]) { realloc_ok = 0; break; }
        fill_pattern(ptrs[i], sz, 0x5A);
        sizes[i] = sz;
    }
    CHECK(realloc_ok, "re-allocating into freed holes succeeds");

    for (int i = 0; i < N; i++) {
        my_free(ptrs[i]);
    }
    CHECK(1, "freeing all remaining blocks does not crash");
}


 // Registry — add new tests here


static const TestCase tests[] = {
    {"basic malloc/free",                 test_basic_alloc},
    {"malloc(0)",                         test_malloc_zero},
    {"distinct non-overlapping blocks",   test_distinct_blocks_no_overlap},
    {"free list reuse",                   test_free_list_reuse},
    {"calloc zero-initializes",           test_calloc_zeroes},
    {"realloc grow preserves data",       test_realloc_grow_preserves_data},
    {"realloc shrink",                    test_realloc_shrink},
    {"realloc(NULL, size) == malloc",     test_realloc_null_acts_like_malloc},
    {"realloc(ptr, 0) == free",           test_realloc_zero_acts_like_free},
    {"adjacent free blocks coalesce",     test_coalescing},
    {"pointer alignment",                 test_alignment},
    {"mmap path for large allocations",   test_mmap_large_alloc},
    {"realloc crossing mmap threshold",   test_realloc_crosses_mmap_threshold},
    {"stress: random malloc/free/realloc",test_stress},
};

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    heap_init();

    /* Shared memory so every forked child's CHECK() results are visible
     * back in the parent once the child exits. */
    counters = mmap(NULL, sizeof(SharedCounters), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    counters->checks_run = 0;
    counters->checks_failed = 0;

    size_t num_tests = sizeof(tests) / sizeof(tests[0]);
    int suites_failed = 0;
    int suites_crashed = 0;

    for (size_t i = 0; i < num_tests; i++) {
        printf("\n== %s ==\nn", tests[i].name);
        fflush(stdout); 

        pid_t pid = fork();
        if (pid == 0) {
            current_test_failed = 0;
            tests[i].fn();
            _exit(current_test_failed ? 1 : 0);
        }

        int status;
        waitpid(pid, &status, 0);

        if (WIFSIGNALED(status)) {
            printf("  [CRASH] test terminated by signal %d (%s)\n",
                   WTERMSIG(status), strsignal(WTERMSIG(status)));
            suites_crashed++;
            suites_failed++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            suites_failed++;
        }
    }

    printf("\n=====================================\n");
    printf("REPORT\n");
    printf("%d/%d checks passed across %zu test suites\n",
           counters->checks_run - counters->checks_failed, counters->checks_run, num_tests);
    printf("%d suite%s failed", suites_failed, suites_failed == 1 ? "" : "s");
    if (suites_crashed) {
        printf(" (%d crashed the test process)", suites_crashed);
    }
    printf("\n=====================================\n");

    return suites_failed == 0 ? 0 : 1;
}