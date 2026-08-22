Custom-malloc

A custom implementation of malloc, free, realloc, and calloc in C reference by dlmalloc and glibc - built to explore memory management in low level

Features

Segregated free lists: 64 small bins (exact-size, up to 1024 bytes) plus 7 large bins bucketed by power-of-two size class (1 KB – 64 KB+), for fast, size-appropriate lookups.
Two memory sources: small/medium requests are from a sbrk-grown heap arena; requests at or above a configurable threshold (128 KB) go straight to mmap/munmap.
Splitting & coalescing: free blocks are split when a request doesn't need the whole chunk, and adjacent free blocks are coalesced on free to fight fragmentation.
In-place growth: realloc first tries to expand a block into adjacent free space (including the top chunk) before falling back to malloc-copy-free.
Top-chunk management: the heap grows in 64 KB increments and shrinks back to the OS once free space at the top passes a hysteresis threshold, avoiding "flapping" (grow/shrink thrashing).Every free chunk go back to the bins
Thread safety: a single global mutex guards all allocator state, with a dedicated pthread stress test to catch races.
Debug tooling: block/heap consistency checks, ASan/UBSan build target, and helgrind-friendly thread-check target.


Memory layout
Each block is laid out contiguously as header → payload → footer:
       	      user pointer
           	        |
                    v
+-------------------+--------------------------------+-----------------+
|   Header          |              Payload              |  Footer  |
| payload           |        usable memory return       |  size    |
| flags, list       |           to caller               |  copy    |
+------------------+---------------------------------+-----------------+

Header (mblockptr): the block's payload size, flags (free/allocated, sbrk/mmap), and the intrusive list node used to thread it into a bin.
Payload: the address handed back by my_malloc/my_calloc — everything from here up to (but not including) the footer belongs to the caller.
Footer: a trailing copy of payload's size. Scanning backward from an adjacent block's header, the allocator reads this footer to find this block's size and jump straight to its header — that's what makes coalesce() O(1) instead of a list walk.



