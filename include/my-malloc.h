#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <stddef.h>

void *my_malloc(size_t size);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t num, size_t size);
void my_free(void *ptr); 
size_t my_malloc_trim(void);
size_t my_malloc_footprint(void);
size_t my_malloc_align(void);
size_t my_malloc_mmap_threshold(void);
#endif // MY_MALLOC_H 

