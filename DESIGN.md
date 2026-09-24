Allocator Redesign — DESIGN.md

Target completion date: 2026-10-31

1. Purpose

This allocator is a systems-learning project.

The redesign is not intended to reproduce dlmalloc or glibc exactly. The goal is to build a coherent allocator whose design decisions are understood, measured, and justified.

References:

dlmalloc: chunk layout, boundary tags, PREV_INUSE/CURR_INUSE state, top chunk, exact/small-bin ideas, tree-based large-chunk search, region ideas.

glibc malloc: unsorted-bin staging / deferred classification, binning behavior after unsorted reuse attempts, mmap policy references, allocator invariants, and selected hardening concepts.

Hybrid experiment policy: keep dlmalloc-style explicit current/previous in-use state, remove dlmalloc designated-victim (dv), and use a glibc-inspired unsorted bin as the recent-free staging mechanism.

Our benchmarks: decide which mechanisms are actually worth keeping.

The project intentionally stops before full production-level concurrency.

2. Scope

In scope

whole-chunk size accounting

prev_foot

packed size flags

PREV_INUSE-style neighbor state

free-list metadata stored inside free chunk payload

exact small bins

sorted medium-size bins

large-chunk tree structure

unsorted / recent-free staging

top chunk

explicit backing-memory ownership

mmap for huge allocations

stronger allocator invariants

corrected fragmentation metrics

multi-thread benchmark using the current global lock

performance comparison against glibc

Explicitly out of scope before 2026-10-31

per-thread arenas

arena migration

NUMA-aware allocation

fully lock-free structures

production-grade tcache

glibc-level hardening

drop-in libc replacement compatibility

ABI compatibility

allocator tuning APIs

The project should end in a coherent, stable state before introducing these features.

3. Design Principles

3.1 Correctness before fast paths

correctness
    ↓
invariants
    ↓
measurement
    ↓
optimization

3.2 One canonical unit: whole chunk size

The redesign uses total chunk size as the canonical internal unit.

This should simplify:

next-chunk calculation

previous-chunk calculation

splitting

coalescing

top-chunk growth

minimum-chunk rules

bin indexing

4. Chunk Layout

The physical chunk representation intentionally follows dlmalloc so the project can be compared directly against the reference implementation. The experiment changes free-space policy, not the basic chunk encoding.

4.1 Allocated chunk

+----------------------+
| prev_foot            |
+----------------------+
| head: size | flags   |
+----------------------+
| user payload         |
| ...                  |
+----------------------+

Base metadata:

typedef struct chunk {
    size_t prev_foot;
    size_t head;
} chunk;

Low-bit flags follow dlmalloc:

bit 0: PREV_INUSE
bit 1: CURR_INUSE

No separate IS_MMAPPED flag is introduced. A directly mmapped chunk follows the dlmalloc encoding in which both in-use bits are clear. Normal free chunks are coalesced, so they do not remain adjacent to another normal free chunk; this preserves the distinction between an ordinary free chunk and the special mmapped encoding.

PREV_INUSE records the state of the physically previous chunk. CURR_INUSE redundantly records the state of the current chunk. Keeping both follows the dlmalloc representation and makes current-state checks direct while preserving O(1) neighbor coalescing.

4.2 Free chunk

When a chunk is free, the allocator owns the old user payload and can reuse it for free-list metadata:

+----------------------+
| prev_foot            |
+----------------------+
| head: size | flags   |
+----------------------+
| fd / list.next       |
+----------------------+
| bk / list.prev       |
+----------------------+
| remaining free space |
+----------------------+

The fd/bk pointers are overlaid on storage that was user payload while the chunk was allocated, so allocated chunks do not pay permanent free-list pointer overhead.

5. Boundary Tags and PREV_INUSE

For adjacent chunks:

[A][B]

If A is free:

B.prev_foot = size(A)
B.PREV_INUSE = 0

If A is allocated:

B.PREV_INUSE = 1

Backward coalescing becomes:

if (!prev_inuse(curr)) {
    chunk *prev = prev_chunk(curr);
}

The logical footer of A is represented physically by B.prev_foot. When B.PREV_INUSE == 0, B.prev_foot contains the size needed to move backward to A. The allocator also keeps CURR_INUSE in A.head so A's current state can be checked directly.

For adjacent normal chunks [A][B], the invariant is:

- if A is allocated: A.CURR_INUSE = 1 and B.PREV_INUSE = 1
- if A is free: A.CURR_INUSE = 0, B.PREV_INUSE = 0, and B.prev_foot = size(A)

These two state views are redundant by design and must remain consistent. The debug checker should verify this relationship.

6. Free Chunk Organization

The allocator intentionally uses different structures for different workloads:

small
    ↓
exact bins

medium
    ↓
sorted doubly linked bins

large
    ↓
tree structure

huge
    ↓
direct mmap

This is an experimental hybrid design, not a clone of one allocator.

7. Small Chunks

Small chunks use exact-size classes.

Properties:

direct size-class mapping

no best-fit search inside a class

simple insertion/removal

low constant cost

Initial candidate cutoff:

small <= 1024 bytes

8. Medium Chunks

Medium chunks use sorted doubly linked bins.

Reason:

simple metadata

cheap unlink

cheap coalescing integration

likely short lists

strong practical locality

Initial candidate range:

1 KiB < medium <= 32 KiB

Metrics:

average scan steps

p99 scan steps

max scan steps

allocation latency

9. Large Chunks

Large reusable chunks use a tree-based structure.

Reason:

more size variation

long-lived fragmentation

best-fit matters more

long linear scans should be avoided

Questions to resolve:

exact tree key

duplicate-size handling

insertion invariant

deletion invariant

best-fit search

movement from unsorted staging into the tree

Initial candidate range:

32 KiB < large < MMAP_THRESHOLD

10. Huge Chunks

Huge allocations bypass the central heap:

request >= MMAP_THRESHOLD
    ↓
mmap

On free:

munmap

Initial threshold:

128 KiB

11. Unsorted Bin / Recent-Free Staging

Medium and large chunks may enter a temporary unsorted bin after coalescing. This follows the glibc idea that recently returned chunks are given an immediate reuse opportunity before being classified into regular bins. The project intentionally uses this in place of dlmalloc's designated-victim (dv) fast path.

free
  ↓
coalesce
  ↓
unsorted bin
  ↓
malloc checks recent chunks
  ↓
reuse if suitable
  ↓
otherwise classify into DLL/tree

This is deferred classification.

The unsorted bin is not a permanent home. It is a staging structure for deferred classification.

Bounded scanning

Initial policy:

#define UNSORTED_SCAN_LIMIT 8

inspect at most N recent chunks

reuse a fitting chunk

move rejected chunks toward canonical structures

Small chunks may bypass unsorted staging because exact-bin classification is already cheap.

Design choice: no dlmalloc designated-victim (dv). Recent-reuse behavior is intentionally concentrated in the unsorted bin so benchmark results can attribute hits, scans, and rebins to one staging mechanism rather than overlapping fast paths.

12. Top Chunk

The top chunk remains:

free

outside normal bins

physically at the end of the current normal region

used when reusable structures cannot satisfy a request

search reusable structures
        ↓
no fit
        ↓
carve from top
        ↓
top too small
        ↓
ask memory provider for more backing memory

13. Backing Memory Ownership

The redesign must distinguish:

allocator metadata ownership
region ownership
process-wide resources
physical-page residency

Allocator policy should not call OS memory mechanisms everywhere directly.

Candidate abstraction:

void *sys_grow(size_t size);
void *sys_map(size_t size);
int   sys_unmap(void *ptr, size_t size);

Possible region representation:

typedef enum {
    REGION_SBRK,
    REGION_MMAP
} region_kind;

typedef struct heap_region {
    char *base;
    size_t size;
    region_kind kind;
    struct heap_region *next;
} heap_region;

Ownership proof:

base <= ptr < base + size

14. Allocation Path

my_malloc(size)
    |
    v
request_to_chunk_size()
    |
    v
small exact bin?
    |
    +---- hit ----> unlink → return
    |
    v
recent_free?
    |
    +---- fit ----> reuse/split → return
    |
    v
medium DLL / large tree
    |
    +---- fit ----> unlink/split → return
    |
    v
top chunk
    |
    +---- enough --> carve → return
    |
    v
memory provider

Huge requests go directly to mmap.

15. Free Path

my_free(ptr)
    |
    v
validate pointer / ownership
    |
    v
mark free
    |
    v
coalesce backward
    |
    v
coalesce forward
    |
    +---- touches top ----> merge into top
    |
    +---- small ----------> exact bin
    |
    +---- medium/large ---> recent_free

Mmapped huge chunks go to munmap.

16. Realloc

Preferred order:

1. shrink in place
2. grow into next free chunk
3. grow into top chunk
4. optionally grow backward
5. allocate-copy-free fallback

All request normalization should share one overflow-safe helper.

17. Bin Discovery

The allocator should avoid linear probing across many known-empty bins.

Target:

find next non-empty bin

without scanning every intermediate index.

A bitmap or equivalent index should be measured before/after implementation.

18. Debug Invariants

The debug checker should become an executable specification.

Important checks:

chunk size aligned

chunk size >= minimum

next chunk remains inside owning region

prev_foot valid when PREV_INUSE == 0

no two adjacent normal free chunks remain uncoalesced

every canonical free chunk appears exactly once

allocated chunks never appear in free structures

small-bin chunks match exact class

medium bins stay sorted

tree invariants hold

unsorted chunks are not also in canonical structures

CURR_INUSE of each normal chunk agrees with the next chunk's PREV_INUSE

top chunk is outside normal bins

top reaches its region boundary

mmapped chunks never enter central bins

19. Benchmark Plan

Core metrics

ops/sec
mean
p50
p90
p99
max
allocation failures
peak RSS
allocator footprint

Search metrics

bin probes
list scan steps
tree search depth
unsorted hit rate
unsorted scan length
rebin count

Fragmentation metrics

Track:

current live requested bytes
peak live requested bytes
peak allocator footprint

Use cumulative allocation traffic only as a workload metric, not a fragmentation metric.

20. PDN / Concurrency Experiment

Before ending the project, use PDN concepts to measure the scalability limit of the current global-lock allocator.

Benchmark:

1 thread
2 threads
4 threads
8 threads

Measure:

throughput

p50 / p99 latency

lock contention

memory footprint

The goal is to demonstrate why shared allocator state limits scalability.

No per-thread arena implementation is required.

21. Explicit Stop Point

Final architecture for this phase:

OS memory provider
        |
        v
regions
        |
        v
global allocator state + global lock
        |
        +-- small exact bins
        |
        +-- recent-free staging
        |
        +-- medium sorted bins
        |
        +-- large tree
        |
        +-- top chunk
        |
        +-- mmap huge allocations

Future work only:

thread-local cache
        ↓
multiple arenas
        ↓
per-thread / per-CPU ownership

Those belong to a later phase, not the 2026-10-31 target.

22. Milestones to 2026-10-31

Milestone 1 — Close current search bug

fix find_suitable_block

ensure larger bins are searched correctly

add regression tests

Milestone 2 — Stabilize dlmalloc-style chunk metadata

canonical total chunk size

prev_foot / head representation

PREV_INUSE + CURR_INUSE packed flags

retain dlmalloc-style mmapped encoding without a separate mmap flag

no separate explicit footer for normal allocated chunks

move free-list pointers into free payload

Concepts:

boundary tags

ownership

metadata lifetime

Milestone 3 — Rebuild split/coalesce/realloc invariants

split using total chunk sizes

backward coalescing through prev_foot

forward coalescing through physical next chunk

stable realloc behavior

debug checker passing

Milestone 4 — Rebuild free-space structures

exact small bins

medium sorted bins

large tree

bounded unsorted-bin staging

remove dlmalloc dv path

Concept:

workload-dependent data structures

Milestone 5 — Ownership / memory provider

isolate OS memory acquisition

make backing-memory ownership explicit

improve pointer validation

preserve mmap huge path

Concept:

resource ownership and provenance

Milestone 6 — Benchmark and tune

correct fragmentation metrics

measure scan steps / tree depth

measure unsorted hit rate
unsorted scan length
rebin count

tune size thresholds

compare against glibc

Milestone 7 — PDN scalability study

benchmark 1 / 2 / 4 / 8 threads

document global-lock scalability

identify what future arenas would solve

do not implement arenas

23. Reference and Study Resources

Primary implementation references:

- Doug Lea malloc / dlmalloc source: chunk boundary tags, PREV_INUSE/CURR_INUSE representation, top chunk, small bins, tree bins, and designated-victim behavior used as a comparison point.
- glibc malloc source (`malloc/malloc.c`): unsorted bin behavior, regular-bin classification, chunk consistency checks, top handling, and mmap decisions.
- GNU C Library manual — The GNU Allocator: high-level explanation of arenas, coalescing, and mmap-backed large allocations.
- GNU C Library malloc tunables: useful when studying bounded unsorted scanning, tcache interaction, mmap thresholds, and arena behavior.

Recommended glibc study order:

1. Read the `malloc_chunk` layout and size/flag macros in `malloc/malloc.c`.
2. Read the `unsorted_chunks` comment and the unsorted-bin loop in `_int_malloc`.
3. Trace `_int_free_merge_chunk` and `_int_free_create_chunk` to see coalescing and insertion into unsorted/regular bins.
4. Read `unlink_chunk` and large-bin insertion to understand integrity invariants.
5. Read the GNU allocator manual and tunables only after the source path is understood.
6. Treat tcache, fastbins, multiple arenas, and advanced hardening as later reference material; they are not implementation targets for this phase.

Experimental boundary for this project:

- Adopt: glibc-inspired unsorted staging / deferred classification.
- Retain: dlmalloc-style prev_foot/head layout, PREV_INUSE + CURR_INUSE encoding, mmapped-chunk encoding, top chunk, and tree-based large-chunk search.
- Remove: dlmalloc designated victim (dv).
- Defer: tcache, fastbins, per-thread arenas, NUMA policy, and production-grade hardening.

24. Definition of Done

The redesign is complete when:

all core allocation APIs work

chunk metadata consistently uses the dlmalloc-style prev_foot/head representation

small / medium / large / huge paths are distinct

unsorted staging is bounded and measured

large-tree invariants pass debug checks

ownership assumptions are explicit

fragmentation metrics are corrected

single-thread and multi-thread results are documented

no per-thread arena work is required

DESIGN.md matches the implementation

the allocator is stable by 2026-10-31

At that point, development pauses.

The next project should introduce new systems concepts rather than expanding this allocator indefinitely.