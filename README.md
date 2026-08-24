
<div align="left">

# Custom-allocator

**A from-scratch implementation of `malloc`, `free`, `realloc`, and `calloc` in C**

*Built to explore low-level memory management: segregated free lists, coalescing, and heap growth strategies.*

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Memory Layout](#memory-layout)
- [Getting Started](#getting-started)
- [Debug Tooling](#debug-tooling)
- [Project Structure](#project-structure)
- [Acknowledgments](#acknowledgments)

---

## Overview

`custom-malloc` is a drop-in allocator implementing the standard C dynamic memory API — `malloc`, `free`, `calloc`, and `realloc` — from the ground up. It's a systems-programming project focused on the classic trade-offs behind every general-purpose allocator: fragmentation, lookup speed, syscall overhead, and thread safety.

## Features

| Feature | Description |
|---|---|
| **Segregated free lists** | 64 small bins (exact-size, up to 1024 bytes) plus 7 large bins bucketed by power-of-two size class (1 KB – 64 KB+), for fast, size-appropriate lookups. |
| **Two memory sources** | Small/medium requests are served from a `sbrk`-grown heap arena; requests at or above a configurable threshold (128 KB) go straight to `mmap`/`munmap`. |
| **Splitting & coalescing** | Free blocks are split when a request doesn't need the whole chunk, and adjacent free blocks are coalesced on free to fight fragmentation. |
| **In-place growth** | `realloc` first tries to expand a block into adjacent free space (including the top chunk) before falling back to malloc–copy–free. |
| **Top-chunk management** | The heap grows in 64 KB increments and shrinks back to the OS once free space at the top passes a hysteresis threshold, avoiding "flapping" (grow/shrink thrashing). Every freed chunk is returned to the bins. |
| **Thread safety** | A single global mutex guards all allocator state, with a dedicated `pthread` stress test to catch races. |
| **Debug tooling** | Block/heap consistency checks, an ASan/UBSan build target, and a helgrind-friendly thread-check target. |

## Memory Layout

Each block is laid out contiguously as `header → payload → footer`:

```
                user pointer
                         |
                         v
        +------------------+--------------------------+----------+
        |      Header      |          Payload           |  Footer |
        |  size / flags /  |   usable memory returned   |  size   |
        |   free-list node |         to caller          |  copy   |
        +------------------+--------------------------+----------+
```

- **Header (`mblockptr`)** — the block's payload size, flags (free/allocated, `sbrk`/`mmap`), and the intrusive list node used to thread it into a bin.
- **Payload** — the address handed back by `my_malloc`/`my_calloc`; everything from here up to (but not including) the footer belongs to the caller.
- **Footer** — a trailing copy of the payload size. Scanning backward from an adjacent block's header, the allocator reads this footer to find that block's size and jump straight to its header — this is what makes `coalesce()` O(1) instead of a list walk.

## Getting Started

### Prerequisites
- GCC or Clang
- `make`
- POSIX threads (`pthread`) for the concurrency test

### Build

```bash
git clone https://github.com/KhoiHuynh2212/custom-malloc.git
cd custom-malloc
make
```

### Run tests

```bash
make test
```

## Debug Tooling

```bash
# Address/Undefined Behavior Sanitizer build
make asan

# Thread-safety check with Helgrind
make helgrind-check
```

## Project Structure

```
custom-malloc/
├── src/          # Allocator implementation
├── include/       # Public headers
├── tests/          # Unit + stress tests
└── Makefile
```

## Acknowledgments

The design draws heavily on ideas from two well-known allocators:

- **[glibc's `malloc`](https://sourceware.org/glibc/wiki/MallocInternals)** — the header/footer boundary-tag layout, bin sizing strategy, and top-chunk growth/shrink behavior.
- **[dlmalloc](http://gee.cs.oswego.edu/dl/html/malloc.html)** (Doug Lea's allocator) — the segregated free-list structure and the general approach to splitting and coalescing free blocks.

This project is an independent, from-scratch implementation written to understand these ideas by building them, not a fork or derivative of either codebase.



<div align="center">
<sub>Built by <a href="https://github.com/KhoiHuynh2212">Khoi Huynh</a></sub>
</div>