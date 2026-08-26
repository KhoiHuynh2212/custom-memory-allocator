# my-malloc — Session Log

A dlmalloc-style allocator (`gm` global state, sbrk-based small/large bins keyed by `get_bin()`, mmap for large requests, a global mutex, boundary-tag headers/footers). This replaces the earlier Vietnamese-language log, which predates everything below — that one stopped right after the original `list_unlink` NULL-write crash and the first `try_expand` redesign proposal. Both of those landed; this log picks up from there.

## 1. Original crash chain (confirmed fixed)

- **`list_unlink` NULL-write SEGV:** `split()` unconditionally called `list_unlink(&block->list)` on blocks that were never linked into any bin (live allocations, or fresh top-chunk carves). Fixed by removing that call from `split()` — every caller already owns unlinking.
- **Follow-on "allocated-never-on-list" invariant failure:** blocks carved from a freshly created `gm.topchunkptr` never had `list_init()` called on them. Fixed by adding `list_init()` right after each new top-chunk header is created (both in `my_malloc`'s grow-top branch and `try_expand`'s top-absorb branch).

## 2. `try_expand()` — the missing-return bug (confirmed fixed)

**Symptom:** ASan `READ` SEGV in `my_realloc:475`, address `0x820` (garbage, not NULL).

**Root cause:** a refactor nested the backward-merge logic entirely inside `if (next_free) { ... }`. When `next_free` was false, the function fell off the end without a `return` — undefined behavior, returns whatever garbage sits in the return register. `my_realloc` treated that garbage as a valid non-NULL `surv` and dereferenced `surv->payload`.

**Fix:** moved the backward/three-way merge outside `if (next_free)` so every path returns. Current shape:

```c
mblockptr *try_expand(mblockptr *curr, size_t new_payload)
{
    mblockptr *next = BLOCK_NEXT_HEADER(curr, curr->payload);
    if (next == gm.topchunkptr) {
        if (new_payload <= curr->payload) return curr;   /* underflow guard, user's own fix */
        /* grow top if needed, absorb, return curr */
        ...
        return curr;
    }

    int next_free = ...;
    int prev_free = ...;
    size_t best_case = curr->payload
                      + (next_free ? REQUEST_CHUNK(next->payload) : 0)
                      + (prev_free ? REQUEST_CHUNK(prev->payload) : 0);
    if (best_case < new_payload) return NULL;   /* computed before touching anything */

    if (next_free) {
        list_unlink(&next->list);
        curr->payload += REQUEST_CHUNK(next->payload);
        set_footer(curr);
        if (curr->payload >= new_payload) return curr;
    }

    /* backward/three-way merge — unconditionally reachable, not nested in next_free */
    list_unlink(&prev->list);
    prev->payload += REQUEST_CHUNK(curr->payload);
    ...
    return prev;
}
```

Key invariant: `best_case` is computed **before** any mutation, so a call that's going to fail (`return NULL`) never partially merges first — no rollback logic needed.

**Takeaway:** `-Werror=return-type` (already implied by `-Wall -Werror` in the Makefile) is the real defense against this bug class — not blanket null-pointer checks. The bug was never a null pointer; checking for null everywhere would not have caught it.

## 3. Two more allocator-side bugs found this round (diagnosed, not yet patched)

- **`find_suitable_block()` fallback loop off-by-one:** `for (int i = idx; i < NUM_BINS - 1; i++)` never scans the last bin. A suitable block sitting only in the largest bin is invisible to the upward scan, so `my_malloc` falls through to `grow_top` unnecessarily. Fix: `i < NUM_BINS`.
- **`my_free()`'s double-free check races outside the lock.** `is_free(block)` is read before `pthread_mutex_lock`, so two threads freeing the *same* pointer concurrently can both pass the check before either sets the free bit — silent bin corruption, not a crash at the check site. Fix: move the check-and-set inside the critical section. Note this only closes the *concurrent* window; it never affected the single-threaded case (a lock only protects against concurrent access, it can't change already-correct sequential logic — calling `my_free(p)` twice from one thread is caught identically either way).
- (Lower priority, noted in passing) `get_bin()` casts `payload` down to `unsigned` (32-bit) before `__builtin_clzl` — only matters for payloads near/above 4 GB.

## 4. New public API: `my_malloc_footprint()`, `my_malloc_align()`, `my_malloc_mmap_threshold()` (applied)

Design decisions:
- **Not `static`** — test files need external linkage.
- **No lock, no `#ifdef DEBUG` guard** — matches dlmalloc's own `malloc_footprint()`: self-reported, "may be stale," a documented no-lock tradeoff (grounded against `gee.cs.oswego.edu/pub/misc/malloc-2.8.4.c`). This is a real public API function, not a debug-only helper.

```c
size_t my_malloc_footprint(void)      { return (size_t)(gm.heap_end - gm.heap_start); }
size_t my_malloc_align(void)          { return (size_t)align; }
size_t my_malloc_mmap_threshold(void) { return (size_t)MMAP_THRESHOLD; }
```

The latter two exist so black-box tests never hardcode a duplicate of `ALIGN`/`MMAP_THRESHOLD` from `internal.h` — one source of truth, queried at runtime, closer to how glibc exposes tunables via `mallopt`/`mallinfo` than a copy-pasted macro.

## 5. `test_heap_shrink_boundary` — rewritten around the glibc `malloc_trim(3)` contract (applied)

**Original bug:** captured `heap_floor = sbrk(0)` after `heap_init()` had already grown the heap, then asserted the final break never dips below it. Full coalescing can legitimately retreat `gm.topchunkptr` back near `heap_start` — correct behavior the test's floor made impossible to satisfy. A first proposed fix (compare against `gm.heap_start`) was also rejected as vacuous.

**Resolution**, grounded in real allocator docs: `malloc_trim(3)` documents only a boolean contract (memory *may* be released, no exact-byte/address guarantee); dlmalloc's `malloc_footprint()` is exactly the self-reported mechanism for this. Rewrote to assert only relative shrink:

```c
size_t footprint_before = my_malloc_footprint();
/* allocate N large sbrk-path blocks, forcing heap growth */
size_t footprint_grown = my_malloc_footprint();
CHECK(footprint_grown > footprint_before, "...");
/* free everything */
size_t footprint_final = my_malloc_footprint();
CHECK(footprint_final < footprint_grown, "footprint shrank -- OS got memory back");
```

Deliberately never compares against `footprint_before` — the allocator isn't obligated to return to its exact starting size, only to shrink from its peak.

## 6. `test_bugs.c` / `test_threads.c` reusability review (applied)

- **`test_bugs.c`** called `check_malloc_state()`, which didn't exist. Implemented (§7). No changes needed to the test file itself once the function is real — including the corruption test (`bb->list.next = 0xdeadbeef` then expect `SIGABRT`), which now works because the checker validates addresses before dereferencing them.
- **`debug.h`** had a missing semicolon after `check_current_use`'s prototype (hard compile error under `-DDEBUG`) — flagged, needs the same treatment as §7's fixes.
- **`test_threads.c`** referenced `MMAP_THRESHOLD`/`ALIGN` directly while including only the public `my-malloc.h` — can't compile as a true black-box test. Fixed: switched to `my_malloc_mmap_threshold()`/`my_malloc_align()` throughout, added an explicit `#include <stdbool.h>` (was relying on a transitive include), removed a direct call to `heap_init()` (internal symbol the public API never promised — `my_malloc()` already lazily initializes on first use). Rewritten file delivered.
- `check_mmapped_chunk`'s apparent signature mismatch (flagged earlier from a stale project-doc copy) turned out to be a non-issue once the user's real, current `debug.c` was reviewed directly — it already takes `(state, block)` consistently and is correct against the mmap-path allocation code in `my_malloc` (page-rounding, footer placement, alignment all check out).

## 7. `check_malloc_state()` — real implementation, grounded and debugged live

A reference "cleaned-up dlmalloc" `debug.c`/`debug.h` pair (uploaded separately, different structs — `malloc_chunk`, `dv`/tree bins) confirmed `check_malloc_state` is a real, standard pattern: a top-level function composing per-structure checks (bins, dv, top chunk) plus a full-heap traversal. That confirmed the *shape* of the design already in progress; adapted to this project's actual structs (`mblockptr`, single `bins[NUM_BINS]` array, no dv/tree split):

```c
static void check_bin_list_safe(struct malloc_state *state, list *head)
{
    /* validates ok_address(n) BEFORE dereferencing -- turns a corrupted
       fd/bk into a clean abort() instead of a raw SIGSEGV */
    list *n = head->next;
    while (n != head) {
        assert(ok_address(state, n));
        assert(n->next->prev == n && n->prev->next == n);   /* glibc's post-2005
             unlink() hardening: P->fd->bk == P / P->bk->fd == P */
        n = n->next;
    }
}

void check_malloc_state(struct malloc_state *state)
{
    for (int i = 0; i < NUM_BINS; i++)
        check_bin_list_safe(state, &state->bins[i]);   /* corruption-safe pass first */

    check_bins(state);                    /* per-bin: get_bin(payload)==i, footer match */
    check_heap(state);                    /* full walk: footers, no-2-adjacent-free */
    check_heap_bin_consistency(state);    /* inuse chunks aren't ghost-linked in a bin */
    check_top_chunk(state);
}
```

**Two real compile errors hit and fixed on the first attempt** (worth remembering as a class of mistake): a stray `const` on just this one function conflicting with every other check function in the file (all non-`const`, including `check_top_chunk`, which then triggered a discarded-qualifiers error) — fixed by dropping `const` to match the file's existing convention; and the first draft reimplemented its own weaker heap-walk instead of reusing `check_bins`/`check_heap`/`check_heap_bin_consistency`, which left all three flagged "defined but not used" under `-Werror` — fixed by actually calling them from `check_malloc_state` instead of duplicating their logic.

**Grounding notes (verified this session, not assumed):**
- glibc's actual unlink hardening is `P->fd->bk == P` and `P->bk->fd == P` — confirmed via [heap-exploitation.dhavalkapil.com](https://heap-exploitation.dhavalkapil.com/attacks/unlink_exploit).
- A literal `do_check_malloc_state` name in canonical dlmalloc source (`malloc-2.8.4.c`) could **not** be confirmed by direct fetch — the composing-function *pattern* is standard and the reference file confirmed it, but that exact name in canonical dlmalloc should be treated as unverified.

## 8. The double-free race, worked through in depth

Filed under "go deeper" this session as a case study in TOCTOU (time-of-check-to-time-of-use) bugs: `my_free`'s `is_free(block)` check runs before the mutex is taken, so two threads freeing the same block concurrently can both observe "not yet free" and both proceed — corrupting a bin (same block linked twice) rather than crashing at the check itself, which is what makes it dangerous (fails silently, surfaces elsewhere later). Fix is collapsing check-then-act into one critical section. Confirmed: this does *not* help same-thread double-free (already caught regardless, since there's no concurrency to race against), and does *not* help the case where thread A frees a block, the memory gets reallocated to someone else, then thread B "frees" it again — no allocator can defend against that; it's a caller bug by definition.

## Still open / not yet applied

- [ ] `find_suitable_block`'s `NUM_BINS - 1` off-by-one (§3)
- [ ] `my_free`'s double-free check moved inside the lock (§3, §8)
- [ ] `get_bin()`'s 32-bit truncation before `__builtin_clzl` (§3, low priority)
- [ ] `debug.h`'s missing semicolon after `check_current_use` (§6)
- [ ] Full rebuild + full suite rerun after everything above lands together, to confirm no regressions
- [ ] Consider hardening `list_unlink()` itself with the same fd/bk consistency assert `check_bin_list_safe` uses, so corruption is caught at the point of use, not only when a debug check happens to run