# my-malloc — Context (reset 2026-09-21)

Supersedes all prior logs (the Vietnamese notes, `malloc-review-2026-08-28.md`,
and the old `context.md` bug-fix session log). Those items either already
landed or are explicitly deferred — see §5. This doc is forward-looking only:
what's being built, what's been decided, and the deadline it's built against.

---

## 0. Deadline & scope contract

- **Hard stop: 2026-10-31.** Buffer window 10-27 to 10-31 — no new work starts there.
- Goal: implement and stress-test a 3-tier free-list design (small exact bins
  / designated victim / tree bins) on top of a dlmalloc-style boundary-tag
  chunk representation, then **stop**. Not aiming to ship a production
  allocator — aiming to understand the design well enough to defend it in an
  interview, ahead of an embedded/avionics track (RTOS/FreeRTOS study starts
  after this deadline, not before).
- **Locking: single global mutex over one `malloc_state`.** No arena, no
  per-thread state. Decided 2026-09-21 — revisit only if a future project
  explicitly needs concurrent arenas.

---

## 1. Header refactor — status (`malloc_chunk.h`)

Field-at-a-time, each batch compiled clean before the next (same discipline
as the original refactor plan).

**Landed:**
- `struct malloc_chunk` — fixed two compile-blocking bugs: an unterminated
  `/* ... * /` comment (space before the `/`) that silently swallowed the
  `head` field declaration; a stray `;` inside `#define MALLOC_CHUNK_SIZE`
  that broke `MIN_CHUNK_SIZE`'s expansion. Also `#endif MALLOC_CHUNK` → bare
  `#endif` (extra-tokens warning under `-Wextra`).
- `next_chunk` / `prev_chunk` (+ `chunk_plus_offset` / `chunk_minus_offset`),
  ported verbatim from dlmalloc's `chunk.h`. **Unguarded invariant:**
  `prev_chunk` is only valid when `prev_inuse()` is false on the current
  chunk — no runtime assert catches a wrong call yet (candidate for
  `debug.c` later).

**In flight (next 2 batches, in order):**
1. `CHUNK_OVERHEAD` — missing, and blocks `MAX_REQUEST` / `MIN_REQUEST` /
   `pad_request` / `request_to_size` (already pasted in, not yet compiling):
   ```c
   #if FOOTERS
   #define CHUNK_OVERHEAD (sizeof(size_t) * 2)
   #else
   #define CHUNK_OVERHEAD (sizeof(size_t))
   #endif
   ```
   `FOOTERS = 0` for this project → 8 bytes/inuse-chunk, down from the old
   design's fixed 48. This is the actual overhead win the refactor exists for.
2. `set_inuse` / `set_inuse_and_prev_inuse` — replaces `set_allocated_chunk`.
   Must also flip the **next** chunk's `PREV_INUSE_BIT`; the old design's
   flag was independent per-chunk, this one isn't.

**Not started:**
- `insert_small_chunk` / `insert_large_chunk` — dlmalloc's originals write
  `p->fd`/`p->bk` directly; this project's chunk uses an embedded `list list`
  instead, so these need translating through `list_add_after`/`list_unlink`,
  not copied verbatim.
- `set_size_and_prev_inuse_of_inuse_chunk`.
- `mark_inuse_foot` / the `FOOTERS`-gated machinery — skip entirely, dead
  code for this project since `FOOTERS = 0` (real dlmalloc gates it behind
  `#if !FOOTERS` too).
- `overhead_for`, `calloc_must_clear`.

---

## 2. Three-tier design — build order

Sequenced smallest-risk to largest-risk. Each tier fully working and tested
before the next starts.

1. **dv (designated victim)** — one chunk, not a bin. Add `dv`/`dv_size` to
   `malloc_state`; `split()` routes leftover into `dv` instead of inserting
   into a bin; the small-request path in `my_malloc` tries `dv` before
   `find_suitable_block`. **Open question to resolve when this tier starts:**
   what happens to `dv` when its neighbor gets freed and `coalesce()` wants
   to merge — does `dv` get replaced, or does the merge get skipped?
2. **tree bins** — the real risk item. `malloc_tree_chunk` layout
   (`child[2]`, `parent`, `index` on top of the base chunk fields), bit-trie
   insert, leftmost-leaf best-fit walk.
   **Hard checkpoint: 2026-10-12.** If basic insert/find isn't correct by
   then, cut to a sorted-list fallback and say so explicitly in the writeup
   ("simplified, not the full trie") — do not let this tier run past the
   checkpoint.
3. **Integration** — full suite (`test-basic`, `test-edge-cases`,
   `test_threads`, `test_bugs`) + ASan/UBSan + Helgrind + a fresh
   before/after benchmark run, once both tiers are in.

---

## 3. Prerequisite bugs (carried over, must land before tier 1 starts)

Correctness debt, not design work — building `dv`/tree bins on top of a
buggy `find_suitable_block` makes any new bug unattributable to the right
layer.

- [ ] `find_suitable_block`'s `i < NUM_BINS - 1` → `i < NUM_BINS` off-by-one.
- [ ] `my_free`'s double-free check (`is_free(block)`) currently races
      outside the lock — move check-and-set inside the critical section.
- [ ] `debug.h` missing `;` after the `check_current_use` prototype.
- [ ] `get_bin()`'s cast to 32-bit before `__builtin_clzl` — **deferred**,
      not part of this refactor. If still unaddressed by 10-31, document as
      a known limit (payloads near/above 4GB), don't fix under deadline
      pressure.

---

## 4. Schedule

| Window | Work |
|---|---|
| 09-21 – 09-28 | Header refactor batches (§1) to completion; §3 prerequisite bugs |
| 09-29 – 10-05 | Tier 1: dv |
| 10-06 – 10-19 | Tier 2: tree bins (hard checkpoint **10-12**) |
| 10-20 – 10-26 | Tier 3: integration, full suite, before/after benchmark |
| 10-27 – 10-31 | Buffer only — no new work. Short writeup for resume/interview. |

---

## 5. Explicitly out of scope for this deadline

Recorded so a future session doesn't reopen these mid-sprint.

- **Arena / per-thread `malloc_state` / tcache** — glibc's real concurrency
  model. Decided against for this pass (single global lock instead).
  Revisit only as its own, separate project.
- **`binmap` bitvector for the old `internal.h`-style small/large bins** —
  moot once tiers 1–2 land; `dv` + tree bins solve the same lookup-speed
  problem differently.
- **Static-pool / no-syscall allocation mode** (the RTOS/MISRA-flavored
  variant discussed earlier) — deliberately deferred. The "why RTOS avoids
  malloc" argument gets made in words at interview time, backed by this
  project's own sbrk/madvise findings — not by more code before 10-31.
- **FreeRTOS / RTOS study** — starts only after 10-31, not before.

---

*Update this file at the end of each session: move completed items out of
§1/§3 into a dated note at the bottom rather than deleting them outright.
Keep the live sections above under ~1 page of genuinely open items; let
finished work accumulate below instead of cluttering the plan.*