# my-malloc — Context (reset 2026-09-22)

Supersedes the 2026-09-21 reset. That version's §2 (three-tier dv/tree-bins
design) and §4 schedule are now stale — **`DESIGN.md` is the source of truth**
for design and milestones as of 2026-09-22. This doc carries forward only
what `DESIGN.md` doesn't cover: session-level status, decisions, and the
prerequisite bug list.

---

## 0. Deadline & scope contract

- **Hard stop: 2026-10-31.** Buffer window 10-27 to 10-31 — no new work starts there.
- Goal: per `DESIGN.md` — a coherent, workload-dependent hybrid allocator
  (exact small bins / sorted medium bins / large tree / bounded unsorted
  staging / mmap huge), built and benchmarked well enough to defend in an
  interview, ahead of an embedded/avionics track (RTOS/FreeRTOS study starts
  after this deadline, not before). Not aiming to clone dlmalloc or glibc.
- **Locking: single global mutex over one `malloc_state`.** No arena, no
  per-thread state. Decided 2026-09-21 — matches `DESIGN.md` §21 (global
  lock is the explicit stop point; arenas are future work). Revisit only if
  a future project explicitly needs concurrent arenas.
- **Dropped 2026-09-22: the designated-victim (dv) tier.** The three-tier
  plan (small exact / dv / tree) from the 09-21 reset is replaced by
  `DESIGN.md`'s four-tier plan (small exact / medium sorted DLL / large tree
  / mmap huge) plus unsorted/recent-free staging in front of medium+large.
  dv's open question (what happens to it on a coalescing free) is moot —
  there is no dv chunk anymore.

---

## 1. Header refactor — status (`malloc_chunk.h`)

Field-at-a-time, each batch compiled clean before the next (same discipline
as the original refactor plan). This work still applies under `DESIGN.md`
§4 (chunk layout) — the field-level design (`prev_size`, packed flags,
`PREV_INUSE`) is unchanged from what was already in progress.

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
  `debug.c`/`DESIGN.md` §18 debug invariants later).

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
   design's fixed 48. This is the actual overhead win the refactor exists
   for, and matches `DESIGN.md` §4/§5 (drop the unconditional footer, keep
   `prev_size` doing double duty via `PREV_INUSE`).
2. `set_inuse` / `set_inuse_and_prev_inuse` — replaces `set_allocated_chunk`.
   Must also flip the **next** chunk's `PREV_INUSE_BIT`; the old design's
   flag was independent per-chunk, this one isn't.

**Not started:**
- `insert_small_chunk` / `insert_large_chunk` — dlmalloc's originals write
  `p->fd`/`p->bk` directly; this project's chunk uses an embedded `list list`
  instead, so these need translating through `list_add_after`/`list_unlink`,
  not copied verbatim. Under `DESIGN.md` §6–§9 these become: exact-bin insert
  (small), sorted-DLL insert (medium), tree insert (large) — three separate
  routines, not one generic pair.
- `set_size_and_prev_inuse_of_inuse_chunk`.
- `mark_inuse_foot` / the `FOOTERS`-gated machinery — skip entirely, dead
  code for this project since `FOOTERS = 0` (real dlmalloc gates it behind
  `#if !FOOTERS` too).
- `overhead_for`, `calloc_must_clear`.

---

## 2. Design — see `DESIGN.md`

Full design (chunk layout, boundary tags, bin tiers, unsorted staging, top
chunk, ownership model, allocation/free/realloc paths, debug invariants,
benchmark plan, PDN concurrency study, milestones 1–7, definition of done)
now lives in `DESIGN.md` — not duplicated here. This file tracks only
session status against it.

Current position: still inside **Milestone 1** (close the search bug) and
the tail of the pre-existing header refactor (§1 above), which overlaps
Milestone 2 (chunk metadata redesign). Not yet started: Milestone 3
(split/coalesce/realloc invariants) onward.

---

## 3. Prerequisite bugs (carried over, must land before Milestone 2 work goes deep)

Correctness debt, not design work — building the new bin structures on top
of a buggy `find_suitable_block` makes any new bug unattributable to the
right layer. `find_suitable_block`'s bug below **is** `DESIGN.md` Milestone 1.

- [ ] `find_suitable_block`'s `i < NUM_BINS - 1` → `i < NUM_BINS` off-by-one.
      (= Milestone 1: "fix find_suitable_block, ensure larger bins are
      searched correctly, add regression tests.")
- [ ] `my_free`'s double-free check (`is_free(block)`) currently races
      outside the lock — move check-and-set inside the critical section.
- [ ] `debug.h` missing `;` after the `check_current_use` prototype.
- [ ] `get_bin()`'s cast to 32-bit before `__builtin_clzl` — **deferred**,
      not part of this refactor. If still unaddressed by 10-31, document as
      a known limit (payloads near/above 4GB), don't fix under deadline
      pressure.

---

## 4. Schedule

Superseded by `DESIGN.md` §22 (Milestones 1–7). Rough mapping to calendar,
kept here since `DESIGN.md` doesn't date its milestones:

| Window | Milestone |
|---|---|
| 09-21 – 09-28 | M1 (search bug + regression tests) + finish header refactor (§1 above, overlaps M2) |
| 09-29 – 10-08 | M2: chunk metadata redesign (prev_size, packed flags, free-list-in-payload) |
| 10-09 – 10-16 | M3: split/coalesce/realloc invariants, debug checker passing |
| 10-17 – 10-23 | M4: free-space structures (exact small / sorted medium / large tree / bounded unsorted) |
| 10-24 – 10-26 | M5: ownership/memory-provider isolation |
| 10-24 – 10-26 | M6: benchmark + tune (may overlap M5) |
| 10-27 – 10-31 | M7: PDN scalability study (1/2/4/8 threads) + buffer + writeup. No new design work starts here. |

This is a working estimate, not a commitment in `DESIGN.md` — adjust as
milestones land faster/slower than expected, but keep the 10-31 hard stop
and the "no new work in the buffer window" rule from §0.

---

## 5. Explicitly out of scope for this deadline

Recorded so a future session doesn't reopen these mid-sprint. Matches
`DESIGN.md` §2 ("Explicitly out of scope before 2026-10-31") — kept here too
since some of these predate `DESIGN.md` and carry extra local context.

- **Arena / per-thread `malloc_state` / tcache / NUMA-aware allocation /
  lock-free structures** — glibc's real concurrency model. Decided against
  for this pass (single global lock instead, per `DESIGN.md` §21). Revisit
  only as its own, separate project.
- **`binmap` bitvector for the old `internal.h`-style small/large bins** —
  moot; `DESIGN.md` §17 (bin discovery) still wants a bitmap-or-equivalent
  index for the new bin tiers, but that's Milestone 4+ work, not a revival
  of the old design.
- **Static-pool / no-syscall allocation mode** (the RTOS/MISRA-flavored
  variant discussed earlier) — deliberately deferred. The "why RTOS avoids
  malloc" argument gets made in words at interview time, backed by this
  project's own sbrk/mmap findings (`DESIGN.md` §13 ownership model) — not
  by more code before 10-31.
- **FreeRTOS / RTOS study** — starts only after 10-31, not before.
- **Drop-in libc compatibility, ABI compatibility, allocator tuning APIs,
  production-grade hardening** — per `DESIGN.md` §2.

---

*Update this file at the end of each session: move completed items out of
§1/§3 into a dated note at the bottom rather than deleting them outright.
Keep the live sections above under ~1 page of genuinely open items; let
finished work accumulate below instead of cluttering the plan. Design
questions belong in `DESIGN.md`, not here — if a decision changes the
design, update `DESIGN.md` first and just log the fact of the change here.*
</content>
</invoke>