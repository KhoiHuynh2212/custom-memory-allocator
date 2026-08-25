#include "../include/my-malloc.h"
#include "internal.h"

static malloc_state gm; // allocator state

#ifdef DEBUG
const malloc_state *debug_get_state(void) { return &gm; }
#endif

static bool initialized = false;

long g_sbrk_calls = 0;
long g_scan_steps = 0;

static_assert(sizeof(mblockptr) % align == 0, "Must be mutiple of 16");

pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

void heap_init()
{
    pthread_mutex_lock(&global_lock);
    if (initialized)
    {

        pthread_mutex_unlock(&global_lock);
        return;
    }
    // initialize the heap == 128 KB
    void *start = sbrk(INITIAL_TOP_SIZE);

    if (start == (void *)-1)
    {
        pthread_mutex_unlock(&global_lock);
        return;
    }

    gm.heap_start = start;
    gm.heap_end = start + INITIAL_TOP_SIZE;

    for (int i = 0; i < NUM_BINS; i++)
    {
        list_init(&gm.bins[i]);
    }

    gm.topchunkptr = (mblockptr *)start;

    size_t raw_payload = INITIAL_TOP_SIZE - HEADER_SIZE;

    gm.topchunkptr->payload = raw_payload & ~(align - 1);
    gm.topsize = gm.topchunkptr->payload; // update the top size;
    gm.topchunkptr->flags = 0;
    set_free_chunk(gm.topchunkptr);

    list_init(&gm.topchunkptr->list);

    initialized = true; // turn the flag on
    pthread_mutex_unlock(&global_lock);
}

int get_bin(size_t payload)
{
    if (payload < SMALL_BIN_MAX)
    {
        return payload >> 4; // exact size small bin
    }

    int msb = 63 - __builtin_clzl((unsigned)payload);

    if (msb < LARGE_BIN_MIN_EXP)
        msb = LARGE_BIN_MIN_EXP;
    if (msb > LARGE_BIN_MAX_EXP)
        msb = LARGE_BIN_MAX_EXP;

    return NUM_SMALL_BINS + (msb - LARGE_BIN_MIN_EXP);
}

mblockptr *find_suitable_block(size_t request_size)
{
    int idx = get_bin(request_size);

    if (list_is_empty(&gm.bins[idx]))
    {   
        bool found = false;
        for(int i = idx; i < NUM_BINS - 1;i++) {
            if(!list_is_empty(&gm.bins[i])) {
                found = true;
                idx = i;
                break;
            } 
        }
        if(!found) {
            return NULL;
        }
    }

    list *curr = &gm.bins[idx]; // curr is head of that bins
    curr = curr->next;          // move to next block
    mblockptr *block = list_entry(curr, mblockptr, list);

    if (block->payload == request_size)
    {
        // always hit, setting other variables in malloc()
        list_unlink(&block->list);
        return block;
    }
    else
    {
        // THIS IS STILL O(N) - can be optimize if use tree O(log n)
        // TODO: PERFORM SEACHIN IN UNSORTED LARGE BINS
        mblockptr *curr_block;
        mblockptr *best = NULL;
        list_for_each_entry(curr_block, &gm.bins[idx], list)
        {
            g_scan_steps++;


            if(!ok_address(&gm,curr_block)) {
                fprintf(stderr, "heap corruption detected: bad free-list pointer %p\n",(void *)curr_block);
                abort();
            }
            if (curr_block->payload >= request_size) {
                best = curr_block;
            } else {
                break;
            }
                
        }

        if (best != NULL)
            list_unlink(&best->list);

        return best;

        // TODO: CHANGE AFTER WE CHANGE TO SORTED BINS
    }
    // if the there is no block, the caller must request from top chunk
    return NULL;
}

// extend the program break by asking OS to give big chunk of memory and assume the top chunk already exists
mblockptr *grow_top(size_t size)
{
    if (gm.topchunkptr == NULL)
    {
        return NULL;
    }
    size_t block_chunk = size + HEADER_SIZE;
    size_t allocate_size = (size < CHUNK_SIZE) ? CHUNK_SIZE : block_chunk + MINBLOCKSIZE;

    void *request = sbrk(allocate_size);
    g_sbrk_calls++;
    if (request == (void *)-1)
    {
        return NULL;
    }

    gm.topchunkptr->payload += allocate_size;
    gm.topsize = gm.topchunkptr->payload;
    gm.heap_end = (char *)request + allocate_size;
    return gm.topchunkptr;
}

mblockptr *split(mblockptr *block, size_t request_size)
{
    mblockptr *remainder = BLOCK_NEXT_HEADER(block, request_size);
    remainder->payload = block->payload - REQUEST_CHUNK(request_size);
    remainder->flags = 0;
    set_free_chunk(remainder);
    set_footer(remainder);
    list_init(&remainder->list);

    if (remainder->payload < SMALL_BIN_MAX)
        insert_small_chunk(remainder, remainder->payload);
    else
        insert_large_chunk(remainder, remainder->payload);

    block->payload = request_size;
    set_allocated_chunk(block);
    set_footer(block);
 
    return block;
}

mblockptr *coalesce(mblockptr *curr)
{
    size_t *footer = (size_t *)((char *)curr - FOOTER_SIZE);

    int prev_free = ok_address(&gm, footer);

    mblockptr *prev = prev_free ? BLOCK_PREV_HEADER(curr, *footer) : NULL;

    prev_free = (prev_free && (char *)prev >= gm.heap_start && is_free(prev));

    if (prev_free)
    {

        list_unlink(&prev->list);
        prev->payload += REQUEST_CHUNK(curr->payload);
        set_footer(prev);

        curr = prev; // set new curr at prev block
    }

    mblockptr *next = BLOCK_NEXT_HEADER(curr, curr->payload);

    // the next block is top chunk, absorb to top chunk
    if (next == gm.topchunkptr)
    {

        curr->payload += ABSORB(next->payload);

        gm.topsize = curr->payload;

        gm.topchunkptr = curr;

        return curr;
    }

    if ((char *)next < gm.heap_end && is_free(next))
    {

        curr->payload += REQUEST_CHUNK(next->payload);
        set_footer(curr);
        list_unlink(&next->list);
    }

    return curr;
}

void *my_malloc(size_t size)
{
    if (gm.topchunkptr == NULL)
    {
        heap_init();
    }

    mblockptr *curr_block;
    int s;

    if (size == 0 || size >= SIZE_MAX - (align - 1))
    {
        return NULL;
    }

    size_t request_size = align_up(size);

    if (request_size > SIZE_MAX - align_tag)
        return NULL;

    if (request_size >= MMAP_THRESHOLD)
    {

        size_t total_need = align_tag + request_size;
        size_t total_page_up = ((total_need + LINUX_PAGE - 1) & ~(LINUX_PAGE - 1));

        void *ptr = mmap(NULL, total_page_up,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (ptr == MAP_FAILED)
        {

            return NULL;
        }

        curr_block = (mblockptr *)ptr;
        curr_block->flags = 0;
        set_allocated_chunk(curr_block);
        set_mmap_chunk(curr_block);
        curr_block->payload = total_page_up - HEADER_SIZE - FOOTER_SIZE;
        set_footer(curr_block);
    }
    else
    {

        s = pthread_mutex_lock(&global_lock);

        if (s != 0)
        {
            fprintf(stderr, "pthread_mutex_lock failed\n");
        }

        curr_block = find_suitable_block(request_size);

        // if return null, the bucket is empty
        if (curr_block == NULL)
        {

            // carve from the top chunk
            if (request_size >= gm.topsize)
            {
                // grow if the top chunk is too small
                if (grow_top(request_size) == NULL)
                {
                    pthread_mutex_unlock(&global_lock);
                    return NULL;
                }
            }

            mblockptr *p = gm.topchunkptr; // start at old top
            size_t needed = request_size + align_tag;
            if(needed > gm.topsize) {
                // grow if the top chunk is smaller than needed
                if (grow_top(request_size) == NULL)
                {
                    pthread_mutex_unlock(&global_lock);
                    return NULL;
                }
            }
            p->payload = request_size;
            set_allocated_chunk(p);
            set_footer(p);

            gm.topsize -= needed;
            gm.topchunkptr = BLOCK_NEXT_HEADER(p, request_size); // bump request byte
            gm.topchunkptr->payload = gm.topsize;
            gm.topchunkptr->flags = 0;
            list_init(&gm.topchunkptr->list); 
            set_free_chunk(gm.topchunkptr);

            curr_block = p;
        }
        else
        {
            // for large bins only, small bins are fixed size allocated
            if (curr_block->payload >= request_size + MINBLOCKSIZE)
            {
                curr_block = split(curr_block, request_size);
            }

            set_allocated_chunk(curr_block); // mark as allocated (clear free bit)
            set_chunk(curr_block);      // mark as sbrk'd (clear mmap bit)
        }

        s = pthread_mutex_unlock(&global_lock);

        if (s != 0)
        {
            fprintf(stderr, "pthread_mutex_unlock failed\n");
        }
    }

    return curr_block + 1;
}

void *my_calloc(size_t num, size_t size)
{

    if (num != 0 && size > __SIZE_MAX__ / num)
    {
        return NULL;
    }       

    void *ptr = my_malloc(num * size);
    if (ptr == NULL)
    {
        return NULL;
    }

    memset(ptr, 0, num * size);

    return ptr;
}

mblockptr *try_expand(mblockptr *curr, size_t new_payload)
{
    mblockptr *next = BLOCK_NEXT_HEADER(curr, curr->payload);

    if (next == gm.topchunkptr)
    {   
        if (new_payload <= curr->payload)
             return curr;
             
        size_t needed = new_payload - curr->payload;   

        if (needed >= gm.topsize)                       
        {
            if (grow_top(new_payload) == NULL)
                return NULL;
        }

        curr->payload = new_payload;
        gm.topsize -= needed;
        set_footer(curr);

        mblockptr *new_top = BLOCK_NEXT_HEADER(curr, curr->payload);
        gm.topchunkptr = new_top;
        new_top->payload = gm.topsize;
        new_top->flags = 0;
        list_init(&new_top->list);
        set_free_chunk(new_top);

        return curr;
    }

    int next_free = ((char *)next < gm.heap_end && is_free(next));

    size_t *prev_footer = (size_t *)((char *)curr - FOOTER_SIZE);
    int prev_in_range    = ((char *)prev_footer >= gm.heap_start);
    mblockptr *prev       = prev_in_range ? BLOCK_PREV_HEADER(curr, *prev_footer) : NULL;
    int prev_free         = (prev_in_range && (char *)prev >= gm.heap_start && is_free(prev));

    
    size_t best_case = curr->payload
                      + (next_free ? REQUEST_CHUNK(next->payload) : 0)
                      + (prev_free ? REQUEST_CHUNK(prev->payload) : 0);

    if (best_case < new_payload)
        return NULL;   

    
    if (next_free)
    {
        list_unlink(&next->list);
        curr->payload += REQUEST_CHUNK(next->payload);
        set_footer(curr);

        if (curr->payload >= new_payload)
            return curr;  

    list_unlink(&prev->list);
    prev->payload += REQUEST_CHUNK(curr->payload);
    prev->flags = 0;
    set_footer(prev);

    if (curr->payload > 0)
        memmove(prev + 1, curr + 1, curr->payload);

    return prev;
}

void *my_realloc(void *ptr, size_t size)
{
    void *new_ptr;

    if (ptr == NULL)
    {
        return my_malloc(size);
    }
    if (size == 0)
    {
        my_free(ptr);
        return NULL;
    }

    size_t request_size = align_up(size);
    mblockptr *current_block = (mblockptr *)ptr - 1;

    if (!is_mmap(current_block))
    {   
        // SBRK BRANCH
        int s = pthread_mutex_lock(&global_lock);
        if (s != 0)
            fprintf(stderr, "pthread_mutex_lock failed\n");

        // resize to smaller size, cut off and split the block
        if (current_block->payload >= request_size)
        {
            if (current_block->payload >= request_size + MINBLOCKSIZE)
                split(current_block, request_size);

            pthread_mutex_unlock(&global_lock);
            return ptr;
        }

        // if current block is not fit but the request size is smaller than MMAP_THRESHOLD
        if (request_size < MMAP_THRESHOLD)
        {
            mblockptr *surv = try_expand(current_block, request_size);

            if (surv != NULL)
            {
                if (surv->payload >= request_size + MINBLOCKSIZE)
                    split(surv, request_size); // split survivor block

                pthread_mutex_unlock(&global_lock);
                return surv + 1;
                // try_expand may move the data to previous address, to ensure we return correct address of the data, use block + 1
            }
        }
        pthread_mutex_unlock(&global_lock);
    }
    else
    {
        // MMAP BRANCH
        if (request_size <= current_block->payload)
        {
            return ptr;
        }

        void *new_loc;
        size_t total_need = align_tag + request_size;
        size_t total_page_up = ((total_need + LINUX_PAGE - 1) & ~(LINUX_PAGE - 1));
        new_loc = mremap(current_block, current_block->payload + align_tag, total_page_up, MREMAP_MAYMOVE);
        if (new_loc != MAP_FAILED)
        {
            mblockptr *nb = (mblockptr *)new_loc;
            nb->payload = total_page_up - HEADER_SIZE - FOOTER_SIZE;
            set_footer(nb);
            return nb + 1;
        } else {
           /* mremap failed: current_block (old payload) remains untouched —
                intentionally fall through to the common malloc-copy-free path at the end instead of returning NULL */
            perror("mremap");
        }
    }

    new_ptr = my_malloc(size);
    if (new_ptr == NULL)
        return NULL;

    size_t copySize =
        (current_block->payload < request_size)
            ? current_block->payload
            : request_size;

    memcpy(new_ptr, ptr, copySize);
    my_free(ptr);

    return new_ptr;
}

size_t trim_chunk(mblockptr* block) {

    if(!is_free(block) || is_mmap(block)) {
        return 0;
    }

    char* payload_start = (char*) (block + 1); 
    char* payload_end = payload_start + block->payload;

    size_t page = (size_t)LINUX_PAGE;

    uintptr_t start = ((uintptr_t) payload_start + page - 1) & ~(page - 1); // ROUND UP
    uintptr_t end = (uintptr_t) payload_end & ~(page - 1); // ROUND DOWN

    if(start >= end) {
        return 0;
    } 

    if(madvise((void*) start, end - start, MADV_DONTNEED) != 0) {
        return 0;
    }

    return end - start;
} 

size_t my_malloc_trim(void){

    const size_t ps = LINUX_PAGE; 

    int psindex = get_bin(ps);

    // const size_t psm1 = ps - 1; 

    size_t total_trimmed = 0;

    int s = pthread_mutex_lock(&global_lock);
    if (s != 0)
        fprintf(stderr, "pthread_mutex_lock failed\n");

    mblockptr* curr;
    for(int i = 0; i < NUM_BINS; ++i) {
        if(i >= psindex) {
            list_for_each_entry(curr, &gm.bins[i], list) {
                total_trimmed += trim_chunk(curr);
            }
        }
    } 

    s = pthread_mutex_unlock(&global_lock);
    if (s != 0)
        fprintf(stderr, "pthread_mutex_unlock failed\n");
    return total_trimmed;    
}

void insert_small_chunk(mblockptr *chunk, size_t size)
{
    int idx = get_bin(size);
    list *head = &gm.bins[idx]; // now at the sentinel head
    list_push_front(head, &chunk->list);
}

void insert_large_chunk(mblockptr *chunk, size_t size)
{

    int idx = get_bin(size);

    list *head = &gm.bins[idx];

    if(list_is_empty(head)) {
        list_add_after(head, &chunk->list);
        return;
    }

    list *curr = head;

    while (curr->next != head)
    {
        mblockptr *next_block = list_entry(curr->next, mblockptr, list);
        
        // insert before to keep sorted list
        if (next_block->payload < chunk->payload)
        {
            break;
        }
        curr = curr->next;
    } 

    list_add_after(curr, &chunk->list);

}
static_assert(TOP_PAD_SIZE < TRIM_THRESHOLD, "shrink pad must be smaller than trigger threshold");

void my_free(void *ptr)
{
    if (ptr == NULL)
        return;

    // get the block header
    mblockptr *block = (mblockptr *)ptr - 1;

    int s;
    if (is_free(block))
    {
        fprintf(stderr, "double free detected at %p\n", ptr);
        abort();
    }

    if (is_mmap(block))
    {

        munmap(block, align_tag + block->payload);
    }
    else
    {
        s = pthread_mutex_lock(&global_lock);
        if (s != 0)
            fprintf(stderr, "pthread_mutex_lock failed\n");
        set_free_chunk(block);
        set_footer(block);
        list_init(&block->list);
        mblockptr *survivor = coalesce(block);

        if (survivor != gm.topchunkptr)
        {

            size_t final_size = survivor->payload;

            if (final_size < SMALL_BIN_MAX)
            {
                insert_small_chunk(survivor, final_size);
            }
            else
            {
                insert_large_chunk(survivor, final_size);
            }
        }

        /** If the top chunk is bigger than a shrink threshold,
            we shrink and return memory for OS, but we must to make sure
            that we don't shrink too much to even below the inital top chunk size
        **/

        if (gm.topsize >= TRIM_THRESHOLD) // so shrink at double initial top size = 128 KB
        {

            uintptr_t next_topchunkptr = (uintptr_t)gm.topchunkptr + HEADER_SIZE + TOP_PAD_SIZE; // keep only 16KB

            // Calculate actual bytes to give back
            size_t actual_shrink_amt = (uintptr_t)gm.heap_end - next_topchunkptr;

            if (actual_shrink_amt > 0)
            {

                if (sbrk(-(intptr_t)actual_shrink_amt) != (void *)-1)
                {
                    // Calculate the new payload size based on the actual new break
                    gm.topchunkptr->payload = TOP_PAD_SIZE;
                    gm.topsize = gm.topchunkptr->payload;
                    gm.heap_end = (char *)next_topchunkptr;
                }
            }
        }
        s = pthread_mutex_unlock(&global_lock);
        if (s != 0)
            fprintf(stderr, "pthread_mutex_unlock failed\n");
    }
}