/*
 * benchmark.c - throughput/latency/fragmentation benchmark for my-malloc
 *
 * Build (ALWAYS with optimizations on for benchmarking -- an unoptimized
 * build tells you nothing about real performance):
 *
 *   gcc -O2 -Wall -Wextra -std=c11 -Iinclude -g \
 *       -o test/benchmark src/my-malloc.c test/benchmark.c
 *
 * Run:
 *   ./test/benchmark                       (defaults: isolated phases)
 *   ./test/benchmark --ops 200000 --seed 7 --csv results.csv
 *   ./test/benchmark --shared              (old behavior: one shared heap)
 *   ./test/benchmark --help
 *
 * PER-PHASE PROCESS ISOLATION (the default mode):
 *
 *   Each benchmark phase runs in its own freshly fork()'d child process.
 *   The parent NEVER calls heap_init() or touches the custom allocator
 *   itself -- it only forks, waits, and prints -- so every child is
 *   forked from an identical, still-pristine process image. That means
 *   every phase's my_malloc/my_free calls start from:
 *     - heap_start/heap_end freshly set by that child's own heap_init()
 *     - an empty free list (no blocks left over from a previous phase)
 *     - a program break (sbrk) that hasn't been extended by anything
 *       this benchmark did before
 *   Without this, running growth_only right after fixed_churn would let
 *   growth_only quietly reuse free-list capacity that fixed_churn left
 *   behind, and the fragmentation phase's "how many bytes did the heap
 *   grow" measurement would read close to zero simply because an earlier
 *   phase already grew it -- exactly the confusing result you'd get
 *   from a shared-heap run. Isolating each phase into its own process
 *   removes that cross-phase contamination entirely: what you see for
 *   phase N is caused only by phase N.
 *
 *   Pass --shared to disable this and go back to one heap shared across
 *   every phase in a single process (useful if you specifically want to
 *   see how later phases benefit from earlier ones' leftover free
 *   capacity -- that's a legitimate thing to want to measure too, just
 *   a different question than "what does this pattern cost from cold").
 *
 * PRODUCTION TECHNIQUES USED IN THIS FILE:
 *   - per-phase subprocess isolation for a genuinely cold starting
 *     state per benchmark case (see above) -- the same idea behind
 *     "death tests" / isolated benchmark runners in other frameworks
 *   - CLOCK_MONOTONIC timing (immune to wall-clock adjustments)
 *   - CLOCK-OVERHEAD CALIBRATION + ADAPTIVE BATCHED SAMPLING: this
 *     allocator's steady-state operations run in the tens of
 *     nanoseconds. A single clock_gettime() call, even via the fast
 *     vDSO path, commonly costs somewhere in that SAME range. Timing
 *     every individual op the naive way (clock, do the op, clock
 *     again) means the benchmark is substantially measuring its own
 *     timer, not the allocator -- a result that looks impressively
 *     fast and is largely meaningless. This file measures the timer's
 *     own overhead first, then groups operations into batches sized
 *     so each timed interval is at least ~1000x that overhead, and
 *     computes percentiles across batches instead of individual ops.
 *     This is the same technique Google Benchmark and criterion.rs use
 *     for sub-microsecond code, for the same reason. Every result row
 *     prints the batch size and sample count it used, so the
 *     methodology is visible, not hidden behind a clean-looking number.
 *   - CPU AFFINITY PINNING: each phase's process is pinned to CPU 0
 *     before it runs, so a mid-run scheduler migration to a different
 *     core doesn't show up disguised as an allocator latency spike.
 *   - PEAK RSS ACCOUNTING via getrusage(), alongside the existing
 *     sbrk-delta heap-growth accounting -- sbrk tells you what the
 *     allocator asked the OS for, RSS tells you what's actually
 *     resident, which can diverge under different workloads.
 *   - EXPLICIT ALLOCATION-FAILURE COUNTING on every single call in
 *     every phase, for both allocators. A silently-ignored NULL return
 *     that gets passed straight to free() would previously just look
 *     like a fast, successful operation.
 *   - warm-up phase before every measured run (avoids first-touch /
 *     cold-cache / lazy-page-fault skew)
 *   - a compiler-barrier "escape" hatch so -O2 can't dead-code-eliminate
 *     allocations the benchmark never reads from (the classic
 *     DoNotOptimize trick from Google Benchmark)
 *   - multiple realistic workload shapes, not just one loop:
 *       fixed-size churn, random-size churn, growth-only, fragmentation-
 *       inducing alternation, and a realloc-heavy pattern
 *   - deterministic, printed PRNG seed for reproducibility
 *   - machine-readable CSV output alongside the human-readable report,
 *     correctly flushed across process boundaries (see child_finish())
 *   - defensive error handling on every allocation (never assumes
 *     success)
 *
 * BLACK-BOX DISCIPLINE: this file includes ONLY "my-malloc.h" (the
 * public API), never "internal.h". Every measurement here goes through
 * my_malloc/my_free/my_realloc and the public my_malloc_footprint()/
 * my_malloc_align()/my_malloc_mmap_threshold() getters, plus plain
 * POSIX sbrk(0) snapshots (sbrk_mark/sbrk_delta_bytes below) -- never
 * an internal symbol like heap_init(), g_sbrk_calls, or g_scan_steps.
 * (This file previously reached across that boundary in three spots --
 * same bug class already caught once in test_threads.c -- which meant
 * it wouldn't compile against the public header alone. Fixed by relying
 * on my_malloc()'s lazy self-init instead of calling heap_init()
 * directly, and by dropping the per-generation scan-step count in the
 * generational-thrash phase, which has no public equivalent.)
 */

/* Must precede every header below -- glibc gates sbrk(), rand_r(), and
 * cpu_set_t/CPU_ZERO/CPU_SET/sched_setaffinity behind this feature-test
 * macro under strict -std=c11. Same reason internal.h defines it first. */
#define _GNU_SOURCE
#include "my-malloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sched.h>

/* ------------------------------------------------------------------ */
/* Compiler barrier so -O2 can't optimize benchmarked work away        */
/* ------------------------------------------------------------------ */

static inline void escape(void *p)
{
    __asm__ volatile("" : : "g"(p) : "memory");
}

/* ------------------------------------------------------------------ */
/* Timing helpers                                                      */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

typedef struct {
    double mean_ns;
    double p50_ns;
    double p90_ns;
    double p99_ns;
    double max_ns;
    double total_sec;
    double ops_per_sec;
    size_t n;         /* number of timed samples (batches, not raw ops) */
    long batch_size;  /* ops amortized per sample -- 0 if not batched */
} stats_t;

static stats_t compute_stats(double *latencies_ns, size_t n, double total_sec)
{
    stats_t s = {0};
    if (n == 0) return s;

    qsort(latencies_ns, n, sizeof(double), cmp_double);

    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += latencies_ns[i];

    s.n = n;
    s.mean_ns = sum / (double)n;
    s.p50_ns = latencies_ns[(size_t)(n * 0.50)];
    s.p90_ns = latencies_ns[(size_t)(n * 0.90 < n ? n * 0.90 : n - 1)];
    s.p99_ns = latencies_ns[(size_t)(n * 0.99 < n ? n * 0.99 : n - 1)];
    s.max_ns = latencies_ns[n - 1];
    s.total_sec = total_sec;
    s.ops_per_sec = total_sec > 0 ? (double)n / total_sec : 0;
    return s;
}

static void print_stats_row(const char *label, const stats_t *s)
{
    printf("  %-32s %12.0f ops/s  mean=%8.1fns  p50=%8.1fns  p90=%8.1fns  p99=%8.1fns  max=%10.1fns"
           "  [batch=%ld samples=%zu]\n",
           label, s->ops_per_sec, s->mean_ns, s->p50_ns, s->p90_ns, s->p99_ns, s->max_ns,
           s->batch_size, s->n);
}

/* ------------------------------------------------------------------ */
/* Clock-overhead calibration                                          */
/*                                                                      */
/* Measures the mean cost of a back-to-back clock_gettime() pair, plus */
/* the timer's reported resolution. Printed once at startup and used   */
/* by run_batched() (below) to pick a batch size that keeps the        */
/* timer's own cost a negligible fraction of every measured interval.  */
/* ------------------------------------------------------------------ */

typedef struct {
    double overhead_ns;
    double res_ns;
} clock_info_t;

static clock_info_t calibrate_clock(void)
{
    clock_info_t info = {0};

    struct timespec res;
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0) {
        info.res_ns = (double)res.tv_sec * 1e9 + (double)res.tv_nsec;
    }

    /* warm up the vDSO path before measuring it */
    for (int i = 0; i < 2000; i++) {
        double t = now_sec();
        escape(&t);
    }

    enum { SAMPLES = 50000 };
    double total = 0.0;
    for (int i = 0; i < SAMPLES; i++) {
        double a = now_sec();
        double b = now_sec();
        total += (b - a);
    }
    info.overhead_ns = (total / SAMPLES) * 1e9;
    if (info.overhead_ns < 0) info.overhead_ns = 0;
    return info;
}

/* ------------------------------------------------------------------ */
/* CPU affinity pinning                                                */
/*                                                                      */
/* Pins the calling process to CPU 0 so a scheduler-driven migration    */
/* mid-phase doesn't show up disguised as allocator latency. Best-      */
/* effort: a failure here degrades measurement quality but shouldn't    */
/* abort the run, so it's reported as a warning, not a fatal error.     */
/* ------------------------------------------------------------------ */

static void pin_to_cpu0(void)
{
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "benchmark: warning: sched_setaffinity failed (%s) -- "
                        "results may show extra jitter from core migration\n",
                strerror(errno));
    }
#else
    fprintf(stderr, "benchmark: warning: CPU pinning not implemented on this platform\n");
#endif
}

/* ------------------------------------------------------------------ */
/* Peak RSS accounting                                                  */
/* ------------------------------------------------------------------ */

static long peak_rss_kb(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
    return (long)ru.ru_maxrss; /* KB on Linux */
}

/* ------------------------------------------------------------------ */
/* CSV output                                                           */
/* ------------------------------------------------------------------ */

static FILE *g_csv = NULL;

static void csv_header(void)
{
    if (!g_csv) return;
    fprintf(g_csv, "benchmark,allocator,samples,ops_per_sec,mean_ns,p50_ns,p90_ns,p99_ns,max_ns,total_sec,alloc_failures\n");
    fflush(g_csv);
}

static void csv_row(const char *bench, const char *alloc, const stats_t *s, long fail)
{
    if (!g_csv) return;
    fprintf(g_csv, "%s,%s,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.6f,%ld\n",
            bench, alloc, s->n, s->ops_per_sec, s->mean_ns,
            s->p50_ns, s->p90_ns, s->p99_ns, s->max_ns, s->total_sec, fail);
}

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    long ops;
    unsigned seed;
    const char *csv_path;
    int skip_libc;
    int isolate; /* 1 = each phase in its own fresh subprocess (default) */
} config_t;

static void print_usage(const char *prog)
{
    printf("Usage: %s [--ops N] [--seed S] [--csv path] [--no-libc] [--shared] [--help]\n", prog);
    printf("  --ops N     number of operations per benchmark phase (default 50000)\n");
    printf("  --seed S    PRNG seed for reproducible random workloads (default: time-based)\n");
    printf("  --csv path  also write machine-readable results to this CSV file\n");
    printf("  --no-libc   skip the glibc comparison, only benchmark my-malloc\n");
    printf("  --shared    run all phases in ONE process sharing one heap (old behavior);\n");
    printf("              default is to isolate each phase in its own fresh subprocess\n");
    printf("              so every phase measures a cold heap, uncontaminated by earlier\n");
    printf("              phases' leftover free-list capacity or heap growth\n");
    printf("  --help      show this message\n");
    printf("\n");
    printf("Every run also: calibrates clock_gettime() overhead and picks an adaptive\n");
    printf("batch size so timer cost stays negligible, pins each phase to CPU 0, and\n");
    printf("tracks peak RSS and allocation failures. See the file header for details.\n");
}

static config_t parse_args(int argc, char **argv)
{
    config_t cfg = {.ops = 50000, .seed = (unsigned)time(NULL),
                     .csv_path = NULL, .skip_libc = 0, .isolate = 1};

    static struct option long_opts[] = {
        {"ops", required_argument, 0, 'o'},
        {"seed", required_argument, 0, 's'},
        {"csv", required_argument, 0, 'c'},
        {"no-libc", no_argument, 0, 'n'},
        {"shared", no_argument, 0, 'S'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "o:s:c:nSh", long_opts, &idx)) != -1) {
        switch (opt) {
        case 'o': cfg.ops = atol(optarg); break;
        case 's': cfg.seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'c': cfg.csv_path = optarg; break;
        case 'n': cfg.skip_libc = 1; break;
        case 'S': cfg.isolate = 0; break;
        case 'h': print_usage(argv[0]); exit(0);
        default: print_usage(argv[0]); exit(1);
        }
    }
    if (cfg.ops <= 0) {
        fprintf(stderr, "benchmark: --ops must be positive\n");
        exit(1);
    }
    return cfg;
}

/* ------------------------------------------------------------------ */
/* Heap growth accounting (custom allocator only -- sbrk-based)         */
/* ------------------------------------------------------------------ */

static void *sbrk_mark(void) { return sbrk(0); }

static long sbrk_delta_bytes(void *before)
{
    void *after = sbrk(0);
    return (long)((char *)after - (char *)before);
}

/* ------------------------------------------------------------------ */
/* Per-phase context + subprocess runner                                */
/* ------------------------------------------------------------------ */

typedef struct {
    config_t *cfg;
    double *scratch;
    clock_info_t clock;
} phase_ctx_t;

/*
 * child_finish - flush everything this process buffered, then exit
 * without running libc's normal atexit/stdio-cleanup machinery.
 *
 * We use _exit() (not exit()) so a child never re-flushes or otherwise
 * touches state shared with the parent's copy of the same FILE objects.
 * That means we MUST flush explicitly first, or buffered stdout/CSV
 * output from this child would simply vanish.
 */
static void child_finish(int status)
{
    fflush(stdout);
    if (g_csv) fflush(g_csv);
    _exit(status);
}

/*
 * run_phase - execute one benchmark phase.
 *
 * In isolated mode: forks a fresh child, which pins itself to CPU 0
 * and then runs @fn. It does NOT call heap_init() itself -- there's no
 * public declaration for that internal symbol, and there doesn't need
 * to be one: my_malloc()'s first call in @fn lazily initializes the
 * heap, and since this is a freshly forked child, that first call is
 * still guaranteed cold (empty free lists, un-grown program break).
 *
 * In shared mode: just calls @fn directly in the current process, whose
 * heap was already lazily initialized by the first my_malloc() call in
 * an earlier phase (and which was already pinned) at the top of main().
 */
static void run_phase(config_t *cfg, void (*fn)(phase_ctx_t *), phase_ctx_t *ctx)
{
    if (!cfg->isolate) {
        fn(ctx);
        return;
    }

    fflush(stdout);
    if (g_csv) fflush(g_csv);

    pid_t pid = fork();
    if (pid < 0) {
        perror("benchmark: fork");
        fprintf(stderr, "benchmark: falling back to running this phase in-process\n");
        fn(ctx);
        return;
    }
    if (pid == 0) {
        pin_to_cpu0();
        fn(ctx);
        child_finish(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        fprintf(stderr, "benchmark: phase subprocess exited abnormally (status=%d)\n", status);
    }
}

/* ------------------------------------------------------------------ */
/* Batched timing infrastructure                                        */
/*                                                                      */
/* See the CLOCK-OVERHEAD CALIBRATION note in the file header for the   */
/* full rationale. Short version: for operations this fast, timing      */
/* each one individually mostly measures the timer. This groups ops     */
/* into adaptively-sized batches, times each batch as one interval, and */
/* divides by the batch size to get an amortized per-op cost. A short   */
/* probe run estimates per-op cost first so the batch size can be       */
/* chosen to keep timer overhead comfortably below 0.1% of each         */
/* measured interval, without wasting the whole op budget on one giant  */
/* batch (which would leave too few samples for meaningful percentiles).*/
/* ------------------------------------------------------------------ */

typedef void (*op_fn_t)(void *ctx, long i);

static stats_t run_batched(op_fn_t op, void *ctx, long total_ops,
                            double *scratch, double overhead_ns)
{
    stats_t empty = {0};
    if (total_ops <= 0) return empty;

    enum { PROBE = 256 };
    long probe_n = total_ops < PROBE ? total_ops : PROBE;

    double t0 = now_sec();
    for (long i = 0; i < probe_n; i++) op(ctx, i);
    double probe_elapsed = now_sec() - t0;
    double per_op_ns = probe_n > 0 ? (probe_elapsed / (double)probe_n) * 1e9 : 100.0;
    if (per_op_ns < 1.0) per_op_ns = 1.0;

    /* Target: each batch's wall time is >= 1000x the timer's own
     * overhead, so the timer contributes well under 0.1% of what's
     * measured. Floor of 2us/batch guards against a zero/tiny overhead
     * reading producing a degenerate batch size of 1. */
    double target_batch_ns = overhead_ns * 1000.0;
    if (target_batch_ns < 2000.0) target_batch_ns = 2000.0;
    long batch = (long)(target_batch_ns / per_op_ns) + 1;
    if (batch < 16) batch = 16;

    long remaining = total_ops - probe_n;
    if (remaining <= 0) {
        /* whole run fit inside the probe -- report it as one sample */
        scratch[0] = per_op_ns;
        stats_t s = compute_stats(scratch, 1, probe_elapsed);
        s.ops_per_sec = probe_elapsed > 0 ? (double)probe_n / probe_elapsed : 0;
        s.batch_size = probe_n;
        return s;
    }

    long num_batches = remaining / batch;
    if (num_batches < 1) { batch = remaining; num_batches = 1; }

    long i = probe_n;
    double total_elapsed = probe_elapsed;
    size_t n = 0;
    for (long b = 0; b < num_batches; b++) {
        double bt0 = now_sec();
        for (long j = 0; j < batch; j++, i++) op(ctx, i);
        double bt1 = now_sec();
        total_elapsed += (bt1 - bt0);
        scratch[n++] = ((bt1 - bt0) / (double)batch) * 1e9;
    }
    /* leftover ops that didn't fill one final batch -- still executed,
     * just not individually timed, so semantics (e.g. sequential PRNG
     * state, ptrs[i] index coverage) stay identical to an unbatched run */
    for (; i < total_ops; i++) op(ctx, i);

    stats_t s = compute_stats(scratch, n, total_elapsed);
    s.ops_per_sec = total_elapsed > 0 ? (double)total_ops / total_elapsed : 0;
    s.batch_size = batch;
    return s;
}

/* ------------------------------------------------------------------ */
/* Workload 1: fixed-size churn (alloc N, immediately free)             */
/* ------------------------------------------------------------------ */

typedef struct { size_t size; long fail; } churn_ctx_t;

static void op_fixed_churn_custom(void *vctx, long i)
{
    (void)i;
    churn_ctx_t *c = (churn_ctx_t *)vctx;
    void *p = my_malloc(c->size);
    escape(p);
    if (!p) { c->fail++; return; }
    my_free(p);
}

static void op_fixed_churn_libc(void *vctx, long i)
{
    (void)i;
    churn_ctx_t *c = (churn_ctx_t *)vctx;
    void *p = malloc(c->size);
    escape(p);
    if (!p) { c->fail++; return; }
    free(p);
}

static void phase_fixed_churn(phase_ctx_t *ctx)
{
    printf("\n-- Fixed-size churn (64 bytes, my_malloc immediately followed by my_free) --\n");

    for (long i = 0; i < ctx->cfg->ops / 10; i++) {
        void *p = my_malloc(64);
        escape(p);
        my_free(p);
    }

    churn_ctx_t cctx = {.size = 64, .fail = 0};
    stats_t s = run_batched(op_fixed_churn_custom, &cctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
    print_stats_row("my-malloc", &s);
    if (cctx.fail) fprintf(stderr, "  WARNING: my_malloc failed %ld/%ld times in this phase\n", cctx.fail, ctx->cfg->ops);
    csv_row("fixed_churn_64b", "my-malloc", &s, cctx.fail);

    if (!ctx->cfg->skip_libc) {
        for (long i = 0; i < ctx->cfg->ops / 10; i++) {
            void *p = malloc(64);
            escape(p);
            free(p);
        }
        churn_ctx_t lctx = {.size = 64, .fail = 0};
        stats_t sl = run_batched(op_fixed_churn_libc, &lctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
        print_stats_row("glibc malloc", &sl);
        if (lctx.fail) fprintf(stderr, "  WARNING: glibc malloc failed %ld/%ld times in this phase\n", lctx.fail, ctx->cfg->ops);
        csv_row("fixed_churn_64b", "glibc", &sl, lctx.fail);
    }

    printf("  %-32s peak RSS so far: %ld KB\n", "", peak_rss_kb());
}

/* ------------------------------------------------------------------ */
/* Workload 2: random-size churn (simulates a realistic mixed workload) */
/* ------------------------------------------------------------------ */

typedef struct { unsigned state; long fail; } random_ctx_t;

static size_t rand_size(unsigned *state)
{
    return (size_t)(rand_r(state) % 2048) + 1;
}

static void op_random_churn_custom(void *vctx, long i)
{
    (void)i;
    random_ctx_t *c = (random_ctx_t *)vctx;
    size_t sz = rand_size(&c->state);
    void *p = my_malloc(sz);
    escape(p);
    if (!p) { c->fail++; return; }
    my_free(p);
}

static void op_random_churn_libc(void *vctx, long i)
{
    (void)i;
    random_ctx_t *c = (random_ctx_t *)vctx;
    size_t sz = rand_size(&c->state);
    void *p = malloc(sz);
    escape(p);
    if (!p) { c->fail++; return; }
    free(p);
}

static void phase_random_churn(phase_ctx_t *ctx)
{
    printf("\n-- Random-size churn (1..2048 bytes, my_malloc immediately followed by my_free) --\n");

    unsigned warm_state = ctx->cfg->seed;
    for (long i = 0; i < ctx->cfg->ops / 10; i++) {
        void *p = my_malloc(rand_size(&warm_state));
        escape(p);
        my_free(p);
    }

    random_ctx_t cctx = {.state = ctx->cfg->seed, .fail = 0};
    stats_t s = run_batched(op_random_churn_custom, &cctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
    print_stats_row("my-malloc", &s);
    if (cctx.fail) fprintf(stderr, "  WARNING: my_malloc failed %ld/%ld times in this phase\n", cctx.fail, ctx->cfg->ops);
    csv_row("random_churn", "my-malloc", &s, cctx.fail);

    if (!ctx->cfg->skip_libc) {
        warm_state = ctx->cfg->seed;
        for (long i = 0; i < ctx->cfg->ops / 10; i++) {
            void *p = malloc(rand_size(&warm_state));
            escape(p);
            free(p);
        }
        random_ctx_t lctx = {.state = ctx->cfg->seed, .fail = 0};
        stats_t sl = run_batched(op_random_churn_libc, &lctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
        print_stats_row("glibc malloc", &sl);
        if (lctx.fail) fprintf(stderr, "  WARNING: glibc malloc failed %ld/%ld times in this phase\n", lctx.fail, ctx->cfg->ops);
        csv_row("random_churn", "glibc", &sl, lctx.fail);
    }

    printf("  %-32s peak RSS so far: %ld KB\n", "", peak_rss_kb());
}

/* ------------------------------------------------------------------ */
/* Workload 3: growth-only (allocate many blocks, hold them all, then   */
/* free everything at the end -- stresses list search / heap extension) */
/* ------------------------------------------------------------------ */

typedef struct { void **ptrs; long fail; } growth_ctx_t;

static void op_growth_custom(void *vctx, long i)
{
    growth_ctx_t *c = (growth_ctx_t *)vctx;
    void *p = my_malloc(32 + (size_t)(i % 64));
    escape(p);
    if (!p) c->fail++;
    c->ptrs[i] = p;
}

static void phase_growth_only(phase_ctx_t *ctx)
{
    printf("\n-- Growth-only (allocate all, hold, free at the end) --\n");

    void **ptrs = malloc((size_t)ctx->cfg->ops * sizeof(void *));
    if (!ptrs) { fprintf(stderr, "benchmark: OOM allocating bookkeeping array\n"); exit(1); }

    void *before = sbrk_mark();
    growth_ctx_t gctx = {.ptrs = ptrs, .fail = 0};
    stats_t s = run_batched(op_growth_custom, &gctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
    long grown = sbrk_delta_bytes(before);

    print_stats_row("my-malloc", &s);
    printf("  %-32s heap grew %ld bytes during this phase%s\n", "", grown,
           ctx->cfg->isolate ? " (cold start)" : " (heap shared with earlier phases)");
    if (gctx.fail) fprintf(stderr, "  WARNING: my_malloc failed %ld/%ld times in this phase\n", gctx.fail, ctx->cfg->ops);
    printf("  %-32s peak RSS after growth: %ld KB\n", "", peak_rss_kb());
    csv_row("growth_only", "my-malloc", &s, gctx.fail);

    for (long i = 0; i < ctx->cfg->ops; i++) my_free(ptrs[i]);
    free(ptrs);
}

/* ------------------------------------------------------------------ */
/* Workload 4: fragmentation-inducing alternation                       */
/* alloc(small), alloc(large), ..., then free ALL the small ones,       */
/* then try to allocate a big run -- a classic fragmentation stress     */
/* pattern. We report heap growth (sbrk delta) as the overhead metric.  */
/* This isn't a timed/batched phase (it's a one-shot measurement), but  */
/* it now tracks allocation failures and peak RSS like every other one. */
/* ------------------------------------------------------------------ */

static void bench_fragmentation_custom(long pairs)
{
    void *before = sbrk_mark();

    void **small = malloc(sizeof(void *) * (size_t)pairs);
    void **large = malloc(sizeof(void *) * (size_t)pairs);
    if (!small || !large) { fprintf(stderr, "benchmark: OOM in fragmentation setup\n"); exit(1); }

    size_t requested_bytes = 0;
    long fail = 0;
    for (long i = 0; i < pairs; i++) {
        small[i] = my_malloc(24);
        large[i] = my_malloc(512);
        escape(small[i]);
        escape(large[i]);
        if (!small[i] || !large[i]) fail++;
        requested_bytes += 24 + 512;
    }

    /* free every small block, leaving a "swiss cheese" free list of
     * small holes between still-live large blocks */
    for (long i = 0; i < pairs; i++) my_free(small[i]);

    /* now request blocks that are too big to fit any single hole,
     * forcing either heap extension or (if your allocator supported it)
     * would reveal fragmentation-driven OOM under a tighter heap */
    long big_ops = pairs / 4 > 0 ? pairs / 4 : 1;
    for (long i = 0; i < big_ops; i++) {
        void *p = my_malloc(400);
        escape(p);
        if (!p) fail++;
        requested_bytes += 400;
        my_free(p);
    }

    for (long i = 0; i < pairs; i++) my_free(large[i]);

    long grown = sbrk_delta_bytes(before);
    double overhead_pct = requested_bytes > 0
        ? (100.0 * ((double)grown - (double)requested_bytes) / (double)requested_bytes)
        : 0.0;

    printf("  %-32s heap grew %8ld bytes for %8zu bytes requested  (%+.1f%% overhead)\n",
           "fragmentation pattern (custom)", grown, requested_bytes, overhead_pct);
    if (fail) fprintf(stderr, "  WARNING: %ld allocation(s) failed during the fragmentation phase\n", fail);
    printf("  %-32s peak RSS after fragmentation: %ld KB\n", "", peak_rss_kb());

    /* This phase's shape (a single heap-growth measurement, not a
     * timed series) doesn't fit the main timing CSV schema, so it gets
     * its own comment-prefixed line instead of forcing a column-count
     * match with rows that mean something different. */
    if (g_csv) {
        fprintf(g_csv, "# fragmentation: heap_grown_bytes=%ld requested_bytes=%zu alloc_failures=%ld\n",
                grown, requested_bytes, fail);
    }

    free(small);
    free(large);
}

static void phase_fragmentation(phase_ctx_t *ctx)
{
    printf("\n-- Fragmentation pattern (alternating small/large, drain smalls) --\n");
    long pairs = ctx->cfg->ops / 4 > 0 ? ctx->cfg->ops / 4 : 1;
    bench_fragmentation_custom(pairs);
}

/* ------------------------------------------------------------------ */
/* Workload 5: realloc-heavy (simulates a growing dynamic array/buffer) */
/* ------------------------------------------------------------------ */

typedef struct { void *p; size_t cur; long fail; } realloc_ctx_t;

static void op_realloc_growth_custom(void *vctx, long i)
{
    realloc_ctx_t *c = (realloc_ctx_t *)vctx;
    size_t next = c->cur + 8 + (size_t)(i % 32);
    void *np = my_realloc(c->p, next);
    if (!np) { c->fail++; return; }
    c->p = np;
    c->cur = next;
    escape(c->p);
    if (c->cur > (1u << 20)) { /* periodically reset so we don't grow unboundedly */
        my_free(c->p);
        c->p = my_malloc(16);
        c->cur = 16;
    }
}

static void op_realloc_growth_libc(void *vctx, long i)
{
    realloc_ctx_t *c = (realloc_ctx_t *)vctx;
    size_t next = c->cur + 8 + (size_t)(i % 32);
    void *np = realloc(c->p, next);
    if (!np) { c->fail++; return; }
    c->p = np;
    c->cur = next;
    escape(c->p);
    if (c->cur > (1u << 20)) {
        free(c->p);
        c->p = malloc(16);
        c->cur = 16;
    }
}

static void phase_realloc_growth(phase_ctx_t *ctx)
{
    printf("\n-- Realloc-heavy growth pattern --\n");

    realloc_ctx_t cctx = {.p = my_malloc(16), .cur = 16, .fail = 0};
    escape(cctx.p);
    if (!cctx.p) {
        fprintf(stderr, "benchmark: initial my_malloc(16) failed, skipping this phase\n");
        return;
    }
    stats_t s = run_batched(op_realloc_growth_custom, &cctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
    my_free(cctx.p);
    print_stats_row("my-malloc", &s);
    if (cctx.fail) fprintf(stderr, "  WARNING: my_realloc failed %ld/%ld times in this phase\n", cctx.fail, ctx->cfg->ops);
    csv_row("realloc_growth", "my-malloc", &s, cctx.fail);

    if (!ctx->cfg->skip_libc) {
        realloc_ctx_t lctx = {.p = malloc(16), .cur = 16, .fail = 0};
        escape(lctx.p);
        if (!lctx.p) {
            fprintf(stderr, "benchmark: initial malloc(16) failed, skipping glibc side of this phase\n");
        } else {
            stats_t sl = run_batched(op_realloc_growth_libc, &lctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
            free(lctx.p);
            print_stats_row("glibc realloc", &sl);
            if (lctx.fail) fprintf(stderr, "  WARNING: glibc realloc failed %ld/%ld times in this phase\n", lctx.fail, ctx->cfg->ops);
            csv_row("realloc_growth", "glibc", &sl, lctx.fail);
        }
    }

    printf("  %-32s peak RSS so far: %ld KB\n", "", peak_rss_kb());
}

/* ------------------------------------------------------------------ */
/* Workload 6: generational thrashing -- long-lived survivors scattered */
/* among short-lived nursery churn, stressing the rover's next-fit walk */
/* ------------------------------------------------------------------ */

/*
 * Simulates a generational-GC-style access pattern: most allocations in
 * each "generation" die immediately (nursery churn), but a small,
 * randomly-sized fraction gets "promoted" and held alive across every
 * later generation. Those survivors sit scattered through the heap
 * permanently, blocking coalescing around them and forcing
 * find_suitable_block's rover to walk past more and more dead-end free
 * blocks as generations accumulate.
 *
 * We can't read the `rover` pointer directly -- it's static inside
 * my-malloc.c, invisible from here. So this measures its cost
 * indirectly instead of assuming it: the op performed (my_malloc of a
 * random small size) is IDENTICAL in every generation. If per-
 * generation latency trends upward anyway, that extra time is coming
 * from somewhere inside the allocator's search, not from this
 * benchmark doing more work. A flat trend means the rover holds up
 * fine under this pattern; a climbing one means it doesn't.
 *
 * Per-generation heap growth is tracked the same black-box way as
 * every other phase in this file -- a plain sbrk(0) snapshot before
 * and after (sbrk_mark/sbrk_delta_bytes), not the internal
 * g_sbrk_calls counter. There's no public equivalent for the internal
 * g_scan_steps counter (the rover's per-call walk length), so that
 * signal is simply not available from outside my-malloc.c and isn't
 * reported here; the timing trend above is what stands in for it.
 */

typedef struct {
    void **survivors;
    long survivor_count;
    long survivor_cap;
    unsigned state;
    long fail;
} thrash_ctx_t;

static void op_generational_thrash(void *vctx, long i)
{
    (void)i;
    thrash_ctx_t *c = (thrash_ctx_t *)vctx;
    size_t sz = rand_size(&c->state); /* reuses the 1..2048 helper from Workload 2 */
    void *p = my_malloc(sz);
    escape(p);
    if (!p) { c->fail++; return; }

    /* ~3% promotion rate: rare enough that survivors accumulate slowly
     * and realistically, common enough to matter over hundreds of
     * generations. */
    if ((rand_r(&c->state) % 100) < 3 && c->survivor_count < c->survivor_cap) {
        c->survivors[c->survivor_count++] = p;
    } else {
        my_free(p);
    }
}

static void phase_generational_thrash(phase_ctx_t *ctx)
{
    printf("\n-- Generational thrashing (long-lived survivors fragment the rover's walk) --\n");

    enum { GENERATIONS = 200 };
    long ops_per_gen = ctx->cfg->ops / GENERATIONS;
    if (ops_per_gen < 32) ops_per_gen = 32; /* keep each generation big enough to time meaningfully */

    long survivor_cap = GENERATIONS * (ops_per_gen / 20 + 1); /* generous bound at ~5% worst case */
    void **survivors = malloc(sizeof(void *) * (size_t)survivor_cap);
    double *gen_avg_ns = malloc(sizeof(double) * GENERATIONS);
    if (!survivors || !gen_avg_ns) {
        fprintf(stderr, "benchmark: OOM allocating generational-thrash bookkeeping\n");
        exit(1);
    }

    thrash_ctx_t tctx = {.survivors = survivors, .survivor_count = 0,
                          .survivor_cap = survivor_cap, .state = ctx->cfg->seed, .fail = 0};

    /* This intentionally does NOT use run_batched(): that function
     * returns one aggregate stats_t for the whole run, but the entire
     * point here is the *trend across generations*, so each generation
     * is timed as its own interval and kept as a separate sample in the
     * series (see gen_avg_ns[]) instead of being folded into one mean. */
    for (int g = 0; g < GENERATIONS; g++) {
        void *gen_before = sbrk_mark(); /* public sbrk(0) snapshot -- no internal symbols needed */
        double t0 = now_sec();
        for (long i = 0; i < ops_per_gen; i++) {
            op_generational_thrash(&tctx, i);
        }
        double elapsed = now_sec() - t0;
        gen_avg_ns[g] = (elapsed / (double)ops_per_gen) * 1e9;
        long grown_this_gen = sbrk_delta_bytes(gen_before);
        /* One CSV comment-row per generation -- not part of the main
         * timing schema (see the fragmentation phase for the same
         * convention), but exactly the series you'd pull into a
         * spreadsheet to plot the degradation curve over time. */
        if (g_csv) {
            fprintf(g_csv, "# generational_thrash gen=%d avg_ns=%.2f survivors_alive=%ld heap_grown_bytes=%ld\n",
                    g, gen_avg_ns[g], tctx.survivor_count, grown_this_gen);
        }
    }

    /* Compare the first 10% of generations (heap still mostly clean)
     * against the last 10% (heap thoroughly scattered with survivors).
     * That ratio IS the traversal degradation this phase exists to
     * catch. */
    int window = GENERATIONS / 10 > 0 ? GENERATIONS / 10 : 1;
    double early_sum = 0, late_sum = 0;
    for (int g = 0; g < window; g++) early_sum += gen_avg_ns[g];
    for (int g = GENERATIONS - window; g < GENERATIONS; g++) late_sum += gen_avg_ns[g];
    double early_avg = early_sum / window;
    double late_avg = late_sum / window;
    double degradation_x = early_avg > 0 ? late_avg / early_avg : 0;

    double worst_ns = gen_avg_ns[0];
    int worst_gen = 0;
    for (int g = 1; g < GENERATIONS; g++) {
        if (gen_avg_ns[g] > worst_ns) { worst_ns = gen_avg_ns[g]; worst_gen = g; }
    }

    printf("  %-32s %ld survivors alive by the end, %d generations x %ld ops each\n",
           "generational thrash", tctx.survivor_count, (int)GENERATIONS, ops_per_gen);
    printf("  %-32s early (gen 0-%d) avg=%.1fns  late (gen %d-%d) avg=%.1fns  degradation=%.2fx\n",
           "", window - 1, early_avg, GENERATIONS - window, GENERATIONS - 1, late_avg, degradation_x);
    printf("  %-32s worst single generation: gen=%d avg=%.1fns (%.2fx early baseline)\n",
           "", worst_gen, worst_ns, early_avg > 0 ? worst_ns / early_avg : 0);
    if (tctx.fail) fprintf(stderr, "  WARNING: my_malloc failed %ld times during generational thrash\n", tctx.fail);
    if (g_csv) {
        fprintf(g_csv, "# generational_thrash_summary early_avg_ns=%.2f late_avg_ns=%.2f "
                        "degradation_x=%.2f worst_gen=%d worst_ns=%.2f\n",
                early_avg, late_avg, degradation_x, worst_gen, worst_ns);
    }
    printf("  %-32s peak RSS after generational thrash: %ld KB\n", "", peak_rss_kb());

    for (long i = 0; i < tctx.survivor_count; i++) my_free(tctx.survivors[i]);
    free(survivors);
    free(gen_avg_ns);
}

/* ------------------------------------------------------------------ */
/* Workload 7: fragmentation & packing utilization                     */
/* bytes the user actually asked for vs bytes the allocator asked the  */
/* OS for via sbrk -- a direct measure of how well split()/coalesce()  */
/* reuse freed holes for later, differently-sized requests instead of  */
/* just growing the heap every time.                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    void **live;
    long live_count;
    long live_cap;
    unsigned state;
    size_t requested_bytes;
    long fail;
} packing_ctx_t;

static void op_packing_utilization(void *vctx, long i)
{
    (void)i;
    packing_ctx_t *c = (packing_ctx_t *)vctx;

    /* ~60% of the time, free a random already-live block first -- this
     * is what creates holes of assorted sizes for the *next*
     * allocation to (ideally) reuse, rather than the heap simply
     * growing forever. A realistic long-running-process pattern, not
     * the alternating small/large swiss-cheese shape the existing
     * `fragmentation` phase already covers. */
    if (c->live_count > 0 && (rand_r(&c->state) % 100) < 60) {
        long idx = (long)(rand_r(&c->state) % (unsigned)c->live_count);
        my_free(c->live[idx]);
        c->live[idx] = c->live[--c->live_count];
    }

    size_t sz = rand_size(&c->state);
    void *p = my_malloc(sz);
    escape(p);
    if (!p) { c->fail++; return; }
    c->requested_bytes += sz;

    if (c->live_count < c->live_cap) {
        c->live[c->live_count++] = p;
    } else {
        /* bookkeeping array is full -- hold it forever rather than
         * lose the pointer, which would be a real leak, not a
         * benchmark artifact. */
        my_free(p);
        c->requested_bytes -= sz;
    }
}

static void phase_packing_utilization(phase_ctx_t *ctx)
{
    printf("\n-- Fragmentation & packing utilization (requested vs sbrk'd bytes) --\n");

    long cap = ctx->cfg->ops;
    void **live = malloc(sizeof(void *) * (size_t)cap);
    if (!live) { fprintf(stderr, "benchmark: OOM allocating packing bookkeeping array\n"); exit(1); }

    void *before = sbrk_mark();
    packing_ctx_t pctx = {.live = live, .live_count = 0, .live_cap = cap,
                           .state = ctx->cfg->seed, .requested_bytes = 0, .fail = 0};

    stats_t s = run_batched(op_packing_utilization, &pctx, ctx->cfg->ops, ctx->scratch, ctx->clock.overhead_ns);
    long grown = sbrk_delta_bytes(before);

    print_stats_row("my-malloc", &s);
    if (grown > 0) {
        double packing_pct = 100.0 * (double)pctx.requested_bytes / (double)grown;
        double overhead_pct = 100.0 * ((double)grown - (double)pctx.requested_bytes) / (double)grown;
        printf("  %-32s requested=%8zu bytes  heap grew=%8ld bytes  packing=%.1f%%  overhead=%.1f%%\n",
               "", pctx.requested_bytes, grown, packing_pct, overhead_pct);
        if (g_csv) {
            fprintf(g_csv, "# packing_utilization requested_bytes=%zu heap_grown_bytes=%ld "
                            "packing_pct=%.2f overhead_pct=%.2f\n",
                    pctx.requested_bytes, grown, packing_pct, overhead_pct);
        }
    } else {
        printf("  %-32s requested=%8zu bytes  heap grew=%8ld bytes  "
               "(fully absorbed by heap_init()'s initial reserve -- nothing to compute overhead against)\n",
               "", pctx.requested_bytes, grown);
        if (g_csv) {
            fprintf(g_csv, "# packing_utilization requested_bytes=%zu heap_grown_bytes=%ld "
                            "packing_pct=NA overhead_pct=NA\n",
                    pctx.requested_bytes, grown);
        }
    }
    if (pctx.fail) fprintf(stderr, "  WARNING: my_malloc failed %ld/%ld times in this phase\n", pctx.fail, ctx->cfg->ops);
    printf("  %-32s peak RSS after packing test: %ld KB\n", "", peak_rss_kb());
    csv_row("packing_utilization", "my-malloc", &s, pctx.fail);

    for (long i = 0; i < pctx.live_count; i++) my_free(pctx.live[i]);
    free(live);
}

/* ------------------------------------------------------------------ */
/* Phase registry -- add new phases here, nowhere else.                 */
/*                                                                       */
/* Same idea as the TestCase registry in test-basic.c/test-edge-       */
/* cases.c: a phase_fn written but never added to this table would       */
/* previously mean editing main() by hand and risking a typo or a       */
/* forgotten call. Now adding a phase is one line here.                 */
/* ------------------------------------------------------------------ */

typedef void (*phase_fn)(phase_ctx_t *);

typedef struct {
    const char *name;
    phase_fn fn;
} PhaseCase;

static const PhaseCase phases[] = {
    {"fixed_churn",           phase_fixed_churn},
    {"random_churn",          phase_random_churn},
    {"growth_only",           phase_growth_only},
    {"fragmentation",         phase_fragmentation},
    {"realloc_growth",        phase_realloc_growth},
    {"generational_thrash",   phase_generational_thrash},
    {"packing_utilization",   phase_packing_utilization},
};

int main(int argc, char **argv)
{
    config_t cfg = parse_args(argc, argv);

    if (cfg.csv_path) {
        g_csv = fopen(cfg.csv_path, "w");
        if (!g_csv) {
            fprintf(stderr, "benchmark: could not open %s for writing: %s\n",
                    cfg.csv_path, strerror(errno));
        } else {
            csv_header();
        }
    }

    /* Calibrated once in the parent -- this measures the timer itself,
     * not the allocator, so it's safe to compute before any fork() and
     * share the result with every child via the copied phase_ctx_t. */
    clock_info_t clk = calibrate_clock();

    printf("my-malloc benchmark suite\n");
    printf("  ops per phase : %ld\n", cfg.ops);
    printf("  seed          : %u  (reuse with --seed %u to reproduce)\n", cfg.seed, cfg.seed);
    printf("  libc compare  : %s\n", cfg.skip_libc ? "disabled" : "enabled");
    printf("  phase mode    : %s\n", cfg.isolate
           ? "isolated (each phase in its own fresh subprocess, cold heap, pinned to CPU 0)"
           : "shared (one heap across all phases, --shared was passed)");
    printf("  timer         : overhead=%.1fns/call-pair  resolution=%.1fns\n",
           clk.overhead_ns, clk.res_ns);
    printf("                  (see [batch=.. samples=..] on each row -- batching keeps\n");
    printf("                  the timer's own cost negligible relative to what's measured)\n");
    printf("  align=%zu  mmap_threshold=%zu  (from the public my_malloc_align()/"
           "my_malloc_mmap_threshold() API)\n",
           my_malloc_align(), my_malloc_mmap_threshold());

    double *scratch = malloc(sizeof(double) * (size_t)cfg.ops);
    if (!scratch) { fprintf(stderr, "benchmark: could not allocate scratch buffer\n"); return 1; }

    if (!cfg.isolate) {
        /* Shared mode: the one process that runs every phase shares the
         * one and only heap, exactly like the old behavior. There's no
         * explicit heap_init() call here -- the first my_malloc() in
         * phase_fixed_churn (the first phase in the registry) lazily
         * initializes it, same as an isolated child would. Pin once
         * here since there's no per-phase fork to pin inside. */
        pin_to_cpu0();
    }

    phase_ctx_t ctx = {.cfg = &cfg, .scratch = scratch, .clock = clk};

    size_t num_phases = sizeof(phases) / sizeof(phases[0]);
    for (size_t i = 0; i < num_phases; i++) {
        run_phase(&cfg, phases[i].fn, &ctx);
    }

    printf("\nDone.");
    if (g_csv) {
        printf(" Results also written to CSV.");
        fclose(g_csv);
    }
    printf("\n");

    free(scratch);
    return 0;
}