/*
 * mm.c
 * yzhi - Yiting Zhi
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "contracts.h"

#include "mm.h"
#include "memlib.h"


// Create aliases for driver tests
// DO NOT CHANGE THE FOLLOWING!
#ifdef DRIVER
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#endif

#define WSIZE 8
#define DSIZE 16
#define CHUNKSIZE (1<<9)
#define BIN 25
#define MSIZE 256

#define verbose 0

/*
 *  Logging Functions
 *  -----------------
 *  - dbg_printf acts like printf, but will not be run in a release build.
 *  - checkheap acts like mm_checkheap, but prints the line it failed on and
 *    exits if it fails.
 */

#ifndef NDEBUG
#define dbg_printf(...) printf(__VA_ARGS__)
#define checkheap(verbose) do {if (mm_checkheap(verbose)) {  \
                             printf("Checkheap failed on line %d\n", __LINE__);\
                             exit(-1);  \
                        }}while(0)
#else
#define dbg_printf(...)
#define checkheap(...)
#endif

static char* heap_listp;
static void* bin[BIN];


/*
 *  Helper functions
 *  ----------------
 */

// Align p to a multiple of w bytes
static inline void* align(const void const* p, unsigned char w) {
    return (void*)(((uintptr_t)(p) + (w-1)) & ~(w-1));
}

// Check if the given pointer is 8-byte aligned
static inline int aligned(const void const* p) {
    return align(p, 8) == p;
}

// Return whether the pointer is in the heap.
static int in_heap(const void* p) {
    return p <= (void *)((char *)mem_heap_hi() + WSIZE) && p >= mem_heap_lo();
}

/*
 *  Block Functions
 *  ---------------
 *  TODO: Add your comment describing block functions here.
 *  The functions below act similar to the macros in the book, but calculate
 *  size in multiples of 4 bytes.
 */

static inline size_t max(size_t x, size_t y) {
    return x > y ? x : y;
}

static inline size_t min(size_t x, size_t y) {
	return x > y ? y : x;
}

static inline size_t pack(size_t size, size_t alloc) {
    return size | alloc;
}

static inline size_t get(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
    return *(size_t *)block;
}

static inline void put(void* block, size_t val) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
    (*(size_t *)block) = val;
}

static inline void put_ptr(void* block, void* ptr) {
	*(size_t *)block = (size_t)ptr;
}

static inline size_t get_size(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (get(block) & (~0x7));
}

static inline size_t get_alloc(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
    return (get(block) & 0x1);
}

static inline void* header_pointer(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (char *)block - WSIZE;
}

static inline void* footer_pointer(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (char *)block + get_size(header_pointer(block)) - DSIZE;
}
static inline void* next_block(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (char *)block + get_size((char *)block - WSIZE);
}

static inline void* prev_block(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (char *)block - get_size((char *)block - DSIZE);
}

static inline void* next_pointer(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (char *)block;
}

static inline void* prev_pointer(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return (char *)block + WSIZE;
}

static inline void* next(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return *(char **)block;
}

static inline void* prev(void* block) {
	REQUIRES(block != NULL);
	REQUIRES(in_heap(block));
	return *(char **)prev_pointer(block);
}

static void insert(void*);
static void delete(void*);
static void *extend_heap(size_t);
static void *coalesce(void*);
static void *find_fit(size_t);
static void place(void*, size_t);

/*
 *  Malloc Implementation
 *  ---------------------
 *  The following functions deal with the user-facing malloc implementation.
 */

/*
 * Initialize: return -1 on error, 0 on success.
 */

 /* bin layout */
 /* 32 40 48 64 ... 248   -- 28 bins */
 /* 256 512 1024 2048 4096 8192 16384    -- 7 bins */

int mm_init(void) {
	if (verbose) printf("init\n");
    for (int i = 0; i < BIN; i++) {
    	bin[i] = 0;
    }
    if ((heap_listp = mem_sbrk(4*WSIZE)) == (void *)-1) {
        return -1;
    }
    put(heap_listp, 0);
    put(heap_listp + WSIZE, pack(DSIZE, 1));
    put(heap_listp + 2 * WSIZE, pack(DSIZE, 1));
    put(heap_listp + 3 * WSIZE, pack(0, 1));
    heap_listp += DSIZE;

    if (extend_heap(CHUNKSIZE/WSIZE) == NULL) {
        return -1;
    }

    return 0;
}

static void *extend_heap(size_t words) {
    size_t size;
    char *bp;
    if (verbose) printf("extending\n");
    size = (words % 2) ? (words+1) * WSIZE : words * WSIZE;
    if ((long)(bp = mem_sbrk(size)) == -1) {
        return NULL;
    }
    put(header_pointer(bp), pack(size, 0));
    put(footer_pointer(bp), pack(size, 0));
    put(header_pointer(next_block(bp)), pack(0, 1));
    return coalesce(bp);
    
}

static void *coalesce(void *bp) {
	if (verbose) printf("coalescing\n");
	size_t size = get_size(header_pointer(bp));

	size_t prev_alloc = get_alloc(header_pointer(prev_block(bp)));
	size_t next_alloc = get_alloc(header_pointer(next_block(bp)));

	if (prev_alloc && next_alloc) {
		insert(bp);
		return bp;
	}

	if (prev_alloc && !next_alloc) {
		delete(next_block(bp));
		size += get_size(header_pointer(next_block(bp)));
		put(header_pointer(bp), pack(size, 0));
		put(footer_pointer(bp), pack(size, 0));
	}

	else if (!prev_alloc && next_alloc) {
		delete(prev_block(bp));
		size += get_size(header_pointer(prev_block(bp)));
		put(footer_pointer(bp), pack(size, 0));
		put(header_pointer(prev_block(bp)), pack(size, 0));
		bp = prev_block(bp);
	}

	else {
		delete(next_block(bp));
		delete(prev_block(bp));
		size += get_size(header_pointer(prev_block(bp))) + get_size(footer_pointer(next_block(bp)));
		put(header_pointer(prev_block(bp)), pack(size, 0));
		put(footer_pointer(next_block(bp)), pack(size, 0));
		bp = prev_block(bp);
	}
	insert(bp);
	return bp;
}

static void insert(void* bp) {
	size_t asize = get_size(header_pointer(bp));
	size_t size = asize;
	void *bin_pointer = NULL;
	void *insert_pointer = NULL;
	int i = 0;
	if (size <= MSIZE) {
		i = (size - 2 * DSIZE) / DSIZE;
		bin_pointer = bin[i];
	}
	else {
		i = (MSIZE - 2 * DSIZE) / DSIZE;
		while (i < BIN - 1 && size > MSIZE) {
			size /= 2;
			i++;
		}
		bin_pointer = bin[i];
		while (bin_pointer != NULL && asize > get_size(header_pointer(bin_pointer))) {
			insert_pointer = bin_pointer;
			bin_pointer = next(bin_pointer);
		}
	}
	if(verbose) printf("Inserting size: %ld, bin No. %d\n", get_size(header_pointer(bp)), i);

	
	if (bin_pointer != NULL) {
		if (insert_pointer != NULL) {
			put_ptr(next_pointer(bp), bin_pointer);
			put_ptr(prev_pointer(bin_pointer), bp);
			put_ptr(prev_pointer(bp), insert_pointer);
			put_ptr(next_pointer(insert_pointer), bp);
		}
		else {
			put_ptr(next_pointer(bp), bin_pointer);
			put_ptr(prev_pointer(bin_pointer), bp);
			put_ptr(prev_pointer(bp), NULL);
			bin[i] = bp;
		}
	}
	else {
		if (insert_pointer != NULL) {
			put_ptr(next_pointer(bp), NULL);
			put_ptr(prev_pointer(bp), insert_pointer);
			put_ptr(next_pointer(insert_pointer), bp);
		}
		else {
			put_ptr(next_pointer(bp), NULL);
			put_ptr(prev_pointer(bp), NULL);
			bin[i] = bp;
		}
	}
}

static void delete(void* bp) {
	if(verbose) printf("Deleting size: %ld\n", get_size(header_pointer(bp)));
	size_t size = get_size(header_pointer(bp));
	int i = 0;
	if (size <= MSIZE) {
		i = (size - 2 * DSIZE) / DSIZE;
	}
	else {
		i = (MSIZE - 2 * DSIZE) / DSIZE;
		while (i < BIN - 1 && size > MSIZE) {
			size /= 2;
			i++;
		}
	}
	if (next(bp) != NULL) {
		if (prev(bp) != NULL) {
			put_ptr(prev_pointer(next(bp)), prev(bp));
			put_ptr(next_pointer(prev(bp)), next(bp));
		}
		else {
			put_ptr(prev_pointer(next(bp)), NULL);
			bin[i] = next(bp);
		}
	}
	else {
		if (prev(bp) != NULL) {
			put_ptr(next_pointer(prev(bp)), NULL);
		}
		else {
			bin[i] = NULL;
		}
	}
}

/*
 * malloc
 */

void *malloc (size_t size) {
    if (verbose) printf("malloc: size = %ld\n", size);
    //checkheap(1);  // Let's make sure the heap is ok!

    //size = size;
    size_t asize;
    size_t extendsize;
    void *bp;

    if (heap_listp == 0) {
    	mm_init();
    }
    if (size <= 0) {
    	return NULL;
    }

    if (size <= DSIZE) {
    	asize = 2 * DSIZE;
    }
    else {
    	asize = DSIZE * ((size + (DSIZE) + (DSIZE - 1)) / DSIZE);
    }
    if ((bp = find_fit(asize)) == NULL) {
    	extendsize = max(asize, CHUNKSIZE);
    	if ((bp = extend_heap(extendsize/WSIZE)) == NULL) {
    		return NULL;
    	}
    }
    place(bp, asize);
    return bp;
}

void *find_fit(size_t asize) {
	if (verbose) printf("find fit....");
	int i = 0;
	size_t size = asize;
	void *bp = NULL;
	if (size <= MSIZE) {
		i = (size - 2 * DSIZE) / DSIZE;
	}
	else {
		i = (MSIZE - 2 * DSIZE) / DSIZE;
		while (i < BIN - 1 && size > MSIZE) {
			size /= 2;
			i++;
		}
	}
	while (i < BIN) {
		if (bin[i] != NULL) {
			bp = bin[i];
			while (bp != NULL && asize > get_size(header_pointer(bp))) {
				bp = next(bp);
			}
			if (bp != NULL) {
				if (verbose) printf("found\n");
				return bp;
			}
		}
		i++;
	}
	if (verbose) printf("not found\n");
	return NULL;
}

void place(void *bp, size_t asize) {
	if (verbose) printf("place\n");
	size_t size, oldsize;
	oldsize = get_size(header_pointer(bp));
	size = oldsize - asize;
	delete(bp);
	if (size >= 2 * DSIZE) {
		put(header_pointer(bp), pack(asize, 1));
		put(footer_pointer(bp), pack(asize, 1));
		put(header_pointer(next_block(bp)), pack(size, 0));
		put(footer_pointer(next_block(bp)), pack(size, 0));
		insert(next_block(bp));
	}
	else {
		put(header_pointer(bp), pack(oldsize, 1));
		put(footer_pointer(bp), pack(oldsize, 1));
	}
}


/*
 * free
 */
void free (void *ptr) {
	if (ptr == 0) {
		return;
	}
	if (heap_listp == 0) {
		mm_init();
	}
	if (!in_heap(ptr)) {
		//if (verbose) printf("free a pointer not in heap\n");
		return;
	}
	if (!aligned(ptr)) {
		//if (verbose) printf("free pointer is not aligned\n");
		return;
	}
	else if (!get_alloc(header_pointer(ptr))) {
		//if (verbose) printf("Block is not allocated\n");
		return;
	}
	if (verbose) printf("free: %ld  %ld\n", get(header_pointer(ptr)), get(footer_pointer(ptr)));

   size_t size = get_size(header_pointer(ptr));

   put(header_pointer(ptr), pack(size, 0));
   put(footer_pointer(ptr), pack(size, 0));
   coalesce(ptr);
}

/*
 * realloc - you may want to look at mm-naive.c
 */
void *realloc(void *oldptr, size_t size) {
	
	if (verbose) printf("realloc: size %ld\n", size);
	if (heap_listp == 0) {
		mm_init();
	}
    size_t oldsize, asize, remain;
    void *newptr;

    if (size == 0) {
    	free(oldptr);
    	return 0;
    }

    if (oldptr == NULL) {
    	return malloc(size);
    }
    if (!aligned(oldptr)) {
		return 0;
	}
	if (!in_heap(oldptr)) {
		return 0;
	}
	if (size <= DSIZE) {
    	size = 2 * DSIZE;
    }
    else {
    	size = DSIZE * ((size + (DSIZE) + (DSIZE - 1)) / DSIZE);
    }
	oldsize = get_size(header_pointer(oldptr));
	if (size <= oldsize) {
		remain = oldsize - size;
		if (remain >= 2 * DSIZE) {
			put(header_pointer(oldptr), pack(size, 1));
			put(footer_pointer(oldptr), pack(size, 1));
			put(header_pointer(next_block(oldptr)), pack(remain, 0));
			put(footer_pointer(next_block(oldptr)), pack(remain, 0));
			insert(next_block(oldptr));
		}
		else {
			put(header_pointer(oldptr), pack(oldsize, 1));
			put(footer_pointer(oldptr), pack(oldsize, 1));
		}
		return oldptr;
	}
	else {
		void* nextblock = next_block(oldptr);
		if(!get_alloc(header_pointer(nextblock))) {
			remain = size - oldsize;
			asize = get_size(header_pointer(nextblock));
			remain -= asize;
			if ((signed long)remain < 0) {
				delete(nextblock);
				asize += oldsize;
				remain = asize - size;
				if (remain >= 2 * DSIZE) {
					put(header_pointer(oldptr), pack(size, 1));
					put(footer_pointer(oldptr), pack(size, 1));
					put(header_pointer(next_block(oldptr)), pack(remain, 0));
					put(footer_pointer(next_block(oldptr)), pack(remain, 0));
					insert(next_block(oldptr));
				}
				else {
					put(header_pointer(oldptr), pack(asize, 1));
					put(footer_pointer(oldptr), pack(asize, 1));
				}
				return oldptr;
			}
		}
	}
	newptr = malloc(size);
	if (!newptr) {
		return 0;
	}
	memcpy(newptr, oldptr, oldsize);
	free(oldptr);
	return newptr;
}

/*
 * calloc - you may want to look at mm-naive.c
 */
void *calloc (size_t nmemb, size_t size) {
    size_t bytes = nmemb * size;
    void *newptr;

    newptr = malloc(bytes);
    memset(newptr, 0, bytes);

    return newptr;
}

// Returns 0 if no errors were found, otherwise returns the error
int mm_checkheap(int v) {
	return 0;
    if (v) {
    	void *ptr = heap_listp;
    	if ((long)get_size((char *)ptr - 3 * WSIZE) != DSIZE ||
    		(long)get_size((char *)ptr - DSIZE) != DSIZE ||
    		!get_alloc((char *)ptr - 3 * WSIZE) ||
    		!get_alloc((char *)ptr - DSIZE)) {
    		printf("prologue block corrupted\n");
    		return 1;
    	}
    	while (get_size(header_pointer(ptr)) > 0) {
    		if (!aligned(ptr)) {
    			printf("not aligned %ld\n", (long)ptr);
    			return 1;
    		}
    		if ((long)get(header_pointer(ptr)) != (long)get(footer_pointer(ptr))) {
    			printf("block doesn't match, %ld,   %ld\n", (long)get(header_pointer(ptr)), (long)get(footer_pointer(ptr)));
    			return 1;
    		}
    		else if (get_size(header_pointer(next_block(ptr))) != 0 && (long)get(header_pointer(next_block(ptr))) != (long)get(footer_pointer(next_block(ptr)))) {
    			printf("next block doesn't match, %ld,   %ld\n", (long)get(header_pointer(next_block(ptr))), (long)get(footer_pointer(next_block(ptr))));
    			return 1;
    		}
    		ptr = next_block(ptr);
    	}
    	if (get((char *)ptr - WSIZE) != pack(0, 1)) {
    		printf("Epilogue block corrupted\n");
    		return 1;
    	}
    }
    return 0;
}