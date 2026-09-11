#ifndef MALLOC_CONFIG_H
#define MALLOC_CONFIG_H

#include <sys/types.h>

#ifndef MALLOC_ALIGNMENT
#define MALLOC_ALIGNMENT ((size_t) (2 * sizeof(void *))) // reserve for 2 pointers - can work on 32 bit or 64 bit machine
#endif  /* MALLOC_ALIGNMENT */


#ifndef HEAP_CONTIGUOUS
#define HEAP_CONTIGUOUS 1
#endif

#ifndef DEFAULT_GRANULARITY 
#if (HEAP_CONTIGUOUS) 
#define DEFAULT_GRANULARITY (0) 
#else   /* HEAP_CONTIGUOUS */
#define DEFAULT_GRANULARITY ((size_t) 64U * (size_t) 1024U)
#endif  /* HEAP_CONTIGUOUS */
#endif  /* DEFAULT_GRANULARITY */


#ifndef TRIM_THRESHOLD
#define TRIM_THRESHOLD ((size_t) 2U * (size_t) 64U * (size_t) 1024U)
#endif


#endif