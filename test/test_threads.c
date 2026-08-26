#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "my-malloc.h"

#define NUM_STRESS_THREADS   8
#define ITERATIONS_PER_THREAD 5000
#define MAX_SMALL_SIZE        2048     // stays well under the mmap threshold
#define LARGE_ALLOC_EVERY     97       // occasionally cross the mmap threshold
#define REALLOC_THREADS       4
#define REALLOC_ITERATIONS    2000

static atomic_llong g_checks_run = 0;
static atomic_llong g_checks_failed = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        atomic_fetch_add(&g_checks_run, 1);                                 \
        if (!(cond)) {                                                      \
            atomic_fetch_add(&g_checks_failed, 1);                          \
            fprintf(stderr, "CHECK FAILED (%s:%d): ", __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

static bool is_aligned(const void *p)
{
    return ((uintptr_t)p % my_malloc_align()) == 0;
}

// Fill `n` bytes at `p` with a pattern derived from a tag and the byte
// offset, so two different allocations produce distinguishable content.
static void fill_pattern(unsigned char *p, size_t n, uint64_t tag)
{
    for (size_t i = 0; i < n; i++)
    {
        p[i] = (unsigned char)((tag + i * 2654435761u) & 0xFF);
    }
}

static bool check_pattern(const unsigned char *p, size_t n, uint64_t tag)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned char expected = (unsigned char)((tag + i * 2654435761u) & 0xFF);
        if (p[i] != expected)
        {
            return false;
        }
    }
    return true;
}

// Phase 1: single-threaded edge cases, run before any concurrency so
// failures here are trivial to diagnose.

static void run_edge_case_tests(void)
{
    void *p;
    size_t mmap_threshold = my_malloc_mmap_threshold();

    p = my_malloc(0);
    CHECK(p == NULL, "my_malloc(0) should return NULL, got %p", p);

    p = my_realloc(NULL, 64);
    CHECK(p != NULL, "my_realloc(NULL, 64) should behave like malloc");
    CHECK(is_aligned(p), "realloc(NULL, ..) result misaligned: %p", p);
    if (p)
    {
        fill_pattern(p, 64, 0x1234);
        CHECK(check_pattern(p, 64, 0x1234), "pattern mismatch after fresh realloc(NULL, n)");
        void *p2 = my_realloc(p, 0);
        CHECK(p2 == NULL, "my_realloc(ptr, 0) should return NULL (acts as free)");
    }

    p = my_calloc(16, 32);
    CHECK(p != NULL, "my_calloc(16, 32) failed");
    if (p)
    {
        unsigned char *bytes = (unsigned char *)p;
        bool all_zero = true;
        for (size_t i = 0; i < 16 * 32; i++)
        {
            if (bytes[i] != 0) { all_zero = false; break; }
        }
        CHECK(all_zero, "my_calloc did not zero-initialize memory");
        my_free(p);
    }

    // overflow: num * size overflows size_t
    p = my_calloc((size_t)-1, 2);
    CHECK(p == NULL, "my_calloc overflow should return NULL, got %p", p);

    // request right at the mmap threshold boundary
    p = my_malloc(mmap_threshold);
    CHECK(p != NULL, "my_malloc(mmap_threshold) failed");
    CHECK(is_aligned(p), "mmap-path allocation misaligned: %p", p);
    if (p)
    {
        fill_pattern((unsigned char *)p, mmap_threshold, 0xABCD);
        CHECK(check_pattern((unsigned char *)p, mmap_threshold, 0xABCD), "pattern mismatch on large allocation");
        my_free(p);
    }

    printf("[edge cases] done (%lld checks so far, %lld failed)\n",
           (long long)atomic_load(&g_checks_run), (long long)atomic_load(&g_checks_failed));
}

// Phase 2: concurrent malloc/free stress with pattern verification,
// mixing small (sbrk-path) and occasional large (mmap-path) requests.
typedef struct
{
    int thread_id;
} stress_arg;

static void *stress_worker(void *arg_)
{
    stress_arg *arg = (stress_arg *)arg_;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(arg->thread_id * 7919 + 1);
    size_t mmap_threshold = my_malloc_mmap_threshold();

    // Keep a small rolling set of live allocations per thread to increase
    // the chance of interleaved alloc/free patterns exposing bugs.
    enum { LIVE_SLOTS = 8 };
    void *live[LIVE_SLOTS] = {0};
    size_t live_size[LIVE_SLOTS] = {0};
    uint64_t live_tag[LIVE_SLOTS] = {0};

    for (int iter = 0; iter < ITERATIONS_PER_THREAD; iter++)
    {
        int slot = rand_r(&seed) % LIVE_SLOTS;

        // Free whatever currently occupies this slot (verifying first).
        if (live[slot] != NULL)
        {
            CHECK(check_pattern((unsigned char *)live[slot], live_size[slot], live_tag[slot]),
                  "corruption detected: thread %d slot %d size %zu",
                  arg->thread_id, slot, live_size[slot]);
            my_free(live[slot]);
            live[slot] = NULL;
        }

        size_t size;
        if (iter % LARGE_ALLOC_EVERY == 0)
        {
            // occasionally cross into mmap territory
            size = mmap_threshold + (rand_r(&seed) % 4096);
        }
        else
        {
            size = 1 + (rand_r(&seed) % MAX_SMALL_SIZE);
        }

        void *p = my_malloc(size);
        if (p == NULL)
        {
            // Under heavy concurrent load an allocation failure isn't
            // necessarily a bug (e.g. OOM), so just skip this round.
            continue;
        }

        CHECK(is_aligned(p), "thread %d: unaligned pointer %p (size %zu)",
              arg->thread_id, p, size);

        uint64_t tag = ((uint64_t)arg->thread_id << 48) ^ ((uint64_t)iter << 16) ^ size;
        fill_pattern((unsigned char *)p, size, tag);
        // Immediate read-back catches gross overlap bugs quickly.
        CHECK(check_pattern((unsigned char *)p, size, tag),
              "thread %d: pattern mismatch immediately after fill (size %zu)",
              arg->thread_id, size);

        live[slot] = p;
        live_size[slot] = size;
        live_tag[slot] = tag;
    }

    // Drain remaining live allocations for this thread.
    for (int i = 0; i < LIVE_SLOTS; i++)
    {
        if (live[i] != NULL)
        {
            CHECK(check_pattern((unsigned char *)live[i], live_size[i], live_tag[i]),
                  "corruption detected at drain: thread %d slot %d", arg->thread_id, i);
            my_free(live[i]);
        }
    }

    return NULL;
}

// Phase 3: concurrent realloc stress -- grow and shrink repeatedly,
// verifying preserved bytes survive each resize.

static void *realloc_worker(void *arg_)
{
    stress_arg *arg = (stress_arg *)arg_;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(arg->thread_id * 104729 + 3);
    size_t mmap_threshold = my_malloc_mmap_threshold();

    size_t size = 1 + (rand_r(&seed) % 256);
    void *p = my_malloc(size);
    CHECK(p != NULL, "realloc worker %d: initial malloc failed", arg->thread_id);
    if (p == NULL) return NULL;

    uint64_t tag = ((uint64_t)arg->thread_id << 40) ^ 0x5A5A5A5A;
    fill_pattern((unsigned char *)p, size, tag);

    for (int iter = 0; iter < REALLOC_ITERATIONS; iter++)
    {
        // Alternate between growing (sometimes past the mmap threshold) and
        // shrinking, to exercise split/coalesce/expand/mmap-remap paths.
        size_t new_size;
        int choice = rand_r(&seed) % 3;
        if (choice == 0)
        {
            new_size = size + 1 + (rand_r(&seed) % 512);
        }
        else if (choice == 1)
        {
            new_size = (size > 8) ? (size / 2) : size;
        }
        else
        {
            new_size = mmap_threshold + (rand_r(&seed) % 2048);
        }
        if (new_size == 0) new_size = 1;

        void *np = my_realloc(p, new_size);
        CHECK(np != NULL, "realloc worker %d: realloc to %zu failed (from %zu)",
              arg->thread_id, new_size, size);
        if (np == NULL)
        {
            // Original block is still valid per realloc contract on failure.
            break;
        }
        CHECK(is_aligned(np), "realloc worker %d: unaligned result %p", arg->thread_id, np);

        size_t preserved = (new_size < size) ? new_size : size;
        CHECK(check_pattern((unsigned char *)np, preserved, tag),
              "realloc worker %d: data not preserved across realloc %zu -> %zu",
              arg->thread_id, size, new_size);

        p = np;
        size = new_size;
        // Re-tag and re-fill the (possibly larger) buffer for the next round.
        tag = tag * 1000003 + iter;
        fill_pattern((unsigned char *)p, size, tag);
    }

    my_free(p);
    return NULL;
}

int main(void)
{

    printf("=== Phase 1: single-threaded edge cases ===\n");
    run_edge_case_tests();

    printf("=== Phase 2: concurrent malloc/free stress (%d threads x %d iters) ===\n",
           NUM_STRESS_THREADS, ITERATIONS_PER_THREAD);
    {
        pthread_t threads[NUM_STRESS_THREADS];
        stress_arg args[NUM_STRESS_THREADS];
        for (int i = 0; i < NUM_STRESS_THREADS; i++)
        {
            args[i].thread_id = i;
            int rc = pthread_create(&threads[i], NULL, stress_worker, &args[i]);
            CHECK(rc == 0, "pthread_create failed for stress worker %d (rc=%d)", i, rc);
        }
        for (int i = 0; i < NUM_STRESS_THREADS; i++)
        {
            pthread_join(threads[i], NULL);
        }
    }
    printf("[stress] done (%lld checks so far, %lld failed)\n",
           (long long)atomic_load(&g_checks_run), (long long)atomic_load(&g_checks_failed));

    printf("=== Phase 3: concurrent realloc stress (%d threads x %d iters) ===\n",
           REALLOC_THREADS, REALLOC_ITERATIONS);
    {
        pthread_t threads[REALLOC_THREADS];
        stress_arg args[REALLOC_THREADS];
        for (int i = 0; i < REALLOC_THREADS; i++)
        {
            args[i].thread_id = i;
            int rc = pthread_create(&threads[i], NULL, realloc_worker, &args[i]);
            CHECK(rc == 0, "pthread_create failed for realloc worker %d (rc=%d)", i, rc);
        }
        for (int i = 0; i < REALLOC_THREADS; i++)
        {
            pthread_join(threads[i], NULL);
        }
    }

    long long total = atomic_load(&g_checks_run);
    long long failed = atomic_load(&g_checks_failed);
    printf("\n=== SUMMARY: %lld checks run, %lld failed ===\n", total, failed);

    if (failed > 0)
    {
        fprintf(stderr, "FAIL: %lld check(s) failed\n", failed);
        return 1;
    }

    printf("PASS: all checks succeeded\n");
    return 0;
}