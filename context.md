# Project Context — my-malloc (dlmalloc-style allocator)

## What this project is
A custom `malloc`/`free`/`realloc`/`calloc` implementation (`my-malloc.c`) with a
segregated free-list allocator (small bins by exact size, large bins by
power-of-two range), sbrk-based heap growth via a "top chunk," and an mmap
path for large requests. `debug.c`/`debug.h` provide `-DDEBUG`-gated
heap-consistency checks (`check_heap`, `check_bins`, `check_top_chunk`, etc.),
modeled after dlmalloc/glibc's internal `do_check_*` functions.

Build command in use:
```
gcc -Wall -Wextra -std=c11 -Iinclude -pthread -D_GNU_SOURCE -DDEBUG \
    -fsanitize=address,undefined -g -Isrc \
    -o test/test_bugs test/test_bugs.c src/my-malloc.c src/debug.c
```

## Core files
- `internal.h` — shared types (`mblockptr`, `malloc_state`), macros
  (`ok_address`, `align_up`, `BLOCK_NEXT_HEADER`, ...), function prototypes.
- `my-malloc.c` — the allocator. Owns the single global heap state:
  `static malloc_state gm;` (file-scope `static` → **internal linkage**,
  not visible to other translation units).
- `debug.c` / `debug.h` — DEBUG-only consistency checks, compiled as a
  separate translation unit from `my-malloc.c`.

## Root cause of the original build break
`gm` is `static` in `my-malloc.c`, so `debug.c` cannot see it directly.
`ok_address(a)` originally hardcoded a reference to `gm`, which only
resolves inside `my-malloc.c`. The allocator already exposes a narrow
accessor for this purpose:
```c
#ifdef DEBUG
const malloc_state *debug_get_state(void) { return &gm; }
#endif
```
`debug.c` uses this via a static local `state` pointer + `ensure_state()`
lazy-init helper — that pattern was already correct for most functions;
only `ok_address` and a few declarations weren't wired up to it.

## Fixes applied (converging state)
1. `ok_address(a)` → `ok_address(state, a)` in `internal.h`; call sites in
   `my-malloc.c` pass `&gm` explicitly, call sites in `debug.c` pass the
   local `state` pointer.
2. `ALIGN_UP` → `align_up` in `debug.c` (`check_malloced_chunk`) — macro
   name case mismatch; **still needs to be applied**, has recurred across
   multiple uploads.
3. `check_mmapped_chunk` — was `static` in `debug.c` but declared
   non-static (extern) in `debug.h` → linkage conflict. Resolved by
   dropping `static` and keeping it public (current state), trading away
   encapsulation `check_any_chunk` still has.
4. `check_top_chunk` signature — mismatch across three sites
   (`debug.h` declaration, `debug.c` definition, `test_bugs.c` call site)
   is the recurring error. Must match in **all three places at once**.

## Open design decision: does `check_top_chunk` take a `state` param?
User wants to **keep an explicit `malloc_state*` parameter** on
`check_top_chunk`, intentionally diverging from the singleton
(`ensure_state()` + static `state`) pattern the other `check_*` functions
use, to leave room for multi-arena/mspace support later.

Verified against real dlmalloc/glibc source: this is a legitimate,
precedented style — dlmalloc/glibc's internal check functions take the
arena (`mstate av`) as an explicit parameter (e.g. glibc's
`do_check_chunk(mstate av, mchunkptr p)` calling
`do_check_free_chunk(mstate av, mchunkptr p)`), specifically because they
support multiple arenas. (uClibc instead pulls state internally via
`get_malloc_state()`, closer to this project's current singleton style —
both patterns exist in the wild.)

**Unresolved tension flagged for the user:** adding the parameter to only
`check_top_chunk` while `check_bins`, `check_heap`, and
`check_heap_bin_consistency` still use the implicit static-`state` +
`ensure_state()` singleton pattern creates an inconsistent hybrid — some
`check_*` functions are "arena-aware," others assume a global singleton,
with no naming convention to tell them apart at a glance.

Two coherent paths presented, decision pending:
- **Option 1 (full dlmalloc style):** every `check_*` function
  (`check_top_chunk`, `check_bins`, `check_heap`,
  `check_heap_bin_consistency`, `check_any_chunk`, `check_mmapped_chunk`,
  `check_malloced_chunk`) takes `const malloc_state* state` explicitly.
  Delete the static `state` var and `ensure_state()` entirely.
- **Option 2 (revert to singleton):** don't special-case
  `check_top_chunk`; keep the `ensure_state()`/static-`state` pattern
  everywhere until multi-arena support is actually being built.

Also still open regardless of which option is chosen: `test_bugs.c`'s
call site (`check_top_chunk();` vs. some parameterized call) has not yet
been seen — needed to confirm the signature change doesn't just move the
compile error there. `test_bugs.c` has not been uploaded/shared yet.

## Working style notes for this session
- User is a junior engineer learning C/systems programming; mentor persona
  is a senior engineer teaching via full explanations for new concepts,
  short conversational replies for follow-ups.
- User has iterated on the same 4–6 build errors across three uploads;
  fixes have been partial/inconsistent each round (one round reintroduced
  an already-identified bug). Re-verify the *current* uploaded file
  contents against the checklist each time rather than assuming prior
  fixes persisted.
- User explicitly values grounding claims in real source (dlmalloc/glibc)
  rather than plausible-sounding assertions — confirm via search before
  asserting "this is how X does it."