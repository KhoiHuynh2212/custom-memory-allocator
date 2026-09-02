#ifndef MALLOC_CHUNK
#define MALLOC_CHUNK

#include <sys/types.h>

#include "malloc_config.h"

typedef struct Block_Header
{   
    size_t prev_sz; 
    size_t size;
    list list;      // double links 
} mblockptr; // block header structure 


#endif MALLOC_CHUNK 