// pool_alloc.c

#include "pool_alloc.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#include <stdio.h>

// -----------------------------------------------------------------------------
// Constants and Macros
// -----------------------------------------------------------------------------

// HEADER_SIZE: Padded size of the BlockHeader (must be a multiple of 16 bytes).
#define HEADER_SIZE 32

// MIN_SPLIT_THRESHOLD: Minimum extra space required to split a free block.
#define MIN_SPLIT_THRESHOLD 16

// -----------------------------------------------------------------------------
// Spin Lock Helper Functions for Free List
// -----------------------------------------------------------------------------

/**
 * acquire_free_list_lock
 * Acquires the spin lock used for protecting the free list.
 * Uses a busy-wait loop with a timeout to prevent deadlock.
 */
static void acquire_free_list_lock(volatile LONG* lock) {
    int timeout = 1000000;  // Arbitrary large timeout value.
    int count = 0;
    while (InterlockedExchange(lock, 1) != 0) {
        if (++count % 10000 == 0) {
            // Debug output can be enabled if needed.
            // printf("[DEBUG] Waiting for free list lock...\n");
        }
        if (--timeout == 0) {
            // Debug output can be enabled if needed.
            // printf("[DEBUG] Deadlock detected in acquire_free_list_lock()\n");
            exit(1);
        }
        Sleep(0); // Yield CPU to prevent hogging cycles.
    }
    // printf("[DEBUG] Acquired free list lock.\n");
}

/**
 * release_free_list_lock
 * Releases the spin lock used for protecting the free list.
 */
static void release_free_list_lock(volatile LONG* lock) {
    InterlockedExchange(lock, 0);
    // printf("[DEBUG] Released free list lock.\n");
}

// -----------------------------------------------------------------------------
// SIMD/AVX memset Implementation
// -----------------------------------------------------------------------------

/**
 * simd_memset
 * Initializes a memory region with a specified value using AVX instructions.
 *
 * @param ptr   Pointer to the memory region.
 * @param value The value to set.
 * @param size  Number of bytes to set.
 */
static void simd_memset(void* ptr, int value, size_t size) {
    // Debug prints disabled.
    unsigned char* p = (unsigned char*)ptr;
    size_t i = 0;
    __m256i vec = _mm256_set1_epi8((char)value);

    // Align the pointer to a 32-byte boundary.
    size_t misalignment = ((uintptr_t)p) % 32;
    if (misalignment) {
         size_t align_bytes = 32 - misalignment;
         if (align_bytes > size)
             align_bytes = size;
         memset(p, value, align_bytes);
         p += align_bytes;
         size -= align_bytes;
    }
    // Use AVX instructions to fill 32 bytes at a time.
    for (i = 0; i < size / 32; i++) {
         _mm256_store_si256((__m256i*)(p + i * 32), vec);
    }
    // Set any remaining bytes.
    size_t remaining = size % 32;
    if (remaining) {
         memset(p + (size - remaining), value, remaining);
    }
    // Debug prints disabled.
}

// -----------------------------------------------------------------------------
// Free List Block Removal (First-Fit)
// -----------------------------------------------------------------------------

/**
 * remove_free_block
 * Traverses the free list and returns the first block that satisfies
 * the allocation size and alignment requirements. If the block is sufficiently
 * large, it splits the block and returns the allocated portion.
 *
 * @param pool       Pointer to the Pool structure.
 * @param alloc_size Number of bytes requested.
 * @param alignment  Alignment requirement.
 * @return Pointer to the allocated block (user pointer), or 0 if none found.
 */
static uintptr_t remove_free_block(Pool* pool, size_t alloc_size, size_t alignment) {
    BlockHeader* prev = NULL;
    BlockHeader* current = (BlockHeader*)pool->free_list;
    while (current) {
        uintptr_t candidate_user_ptr = ((uintptr_t)current) + HEADER_SIZE;
        if ((candidate_user_ptr % alignment) == 0 && current->size >= alloc_size) {
            // Suitable free block found.
            if (prev == NULL) {
                pool->free_list = current->next_free;
            } else {
                prev->next_free = current->next_free;
            }
            // If the free block is large enough, perform splitting.
            if (current->size >= alloc_size + HEADER_SIZE + MIN_SPLIT_THRESHOLD) {
                size_t original_size = current->size;
                current->size = alloc_size;  // Set allocated size.
                // Calculate address for the leftover block.
                uintptr_t leftover_addr = ((uintptr_t)current) + HEADER_SIZE + alloc_size;
                size_t leftover_size = original_size - alloc_size - HEADER_SIZE;
                BlockHeader* leftover_header = (BlockHeader*)leftover_addr;
                leftover_header->size = leftover_size;
                leftover_header->padding = 0;
                leftover_header->next_free = pool->free_list;
                pool->free_list = leftover_addr;
            }
            return ((uintptr_t)current) + HEADER_SIZE;
        }
        prev = current;
        current = (BlockHeader*)current->next_free;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Allocation from a PoolBlock Using Inline Assembly
// -----------------------------------------------------------------------------

/**
 * alloc_from_block
 * Attempts to allocate memory from a given PoolBlock sequentially.
 * The function uses inline assembly for efficient arithmetic and alignment.
 *
 * @param block_ptr  Pointer (as an integer) to the PoolBlock.
 * @param alloc_size Number of bytes requested.
 * @param alignment  Alignment requirement.
 * @return User pointer to the allocated memory, or 0 if insufficient space.
 */
static uintptr_t alloc_from_block(uintptr_t block_ptr, size_t alloc_size, size_t alignment) {
    uintptr_t result = 0;
    __asm__ __volatile__ (
        "movq 16(%[block_ptr]), %%rax\n\t"      // Load current offset into rax.
        "movq %%rax, %%r12\n\t"                 // Save original offset in r12.
        "movq 0(%[block_ptr]), %%rdx\n\t"       // Load block base address into rdx.
        "addq %%rdx, %%rax\n\t"                 // Compute raw address: base + offset.
        "movq %%rax, %%rcx\n\t"                 // Copy raw address into rcx.
        "addq $32, %%rax\n\t"                   // Add HEADER_SIZE (32) to raw address.
        "movq %[alignment], %%rbx\n\t"          // Load alignment into rbx.
        "movq %%rbx, %%r8\n\t"                  // Copy alignment into r8.
        "dec %%r8\n\t"                          // Compute (alignment - 1).
        "addq %%r8, %%rax\n\t"                  // Add (alignment - 1) to rax.
        "not %%r8\n\t"                          // Compute bitwise NOT of r8.
        "andq %%r8, %%rax\n\t"                  // Align the address.
        "movq %%rax, %%r9\n\t"                  // Save aligned address in r9.
        "movq %%rcx, %%r10\n\t"                 // Copy raw address into r10.
        "addq $32, %%r10\n\t"                   // r10 = raw address + HEADER_SIZE.
        "movq %%r9, %%r11\n\t"                  // r11 = aligned address.
        "subq %%r10, %%r11\n\t"                 // r11 = padding.
        "movq $32, %%r8\n\t"                    // r8 = HEADER_SIZE.
        "addq %%r11, %%r8\n\t"                  // r8 = HEADER_SIZE + padding.
        "addq %[alloc_size], %%r8\n\t"          // r8 = total required size.
        "addq %%r8, %%r12\n\t"                  // Update new offset in r12.
        "movq 8(%[block_ptr]), %%rdi\n\t"       // Load block total size into rdi.
        "cmpq %%rdi, %%r12\n\t"                 // Compare new offset with block size.
        "ja 1f\n\t"                             // Jump if insufficient space.
        "movq %%r12, 16(%[block_ptr])\n\t"      // Store new offset.
        "movq %%r9, %%r10\n\t"                  // Prepare to store header.
        "subq $32, %%r10\n\t"                   // Compute header address.
        "movq %[alloc_size], (%%r10)\n\t"       // Store allocated size in header.
        "movq %%r11, 8(%%r10)\n\t"              // Store padding in header.
        "movq $0, 16(%%r10)\n\t"                // Initialize next_free to 0.
        "movq %%r9, %[result]\n\t"              // Set result to aligned address.
        "jmp 2f\n\t"
        "1:\n\t"                                // Label for allocation failure.
        "movq $0, %[result]\n\t"                // Return 0 on failure.
        "2:\n\t"
        : [result] "=r" (result)
        : [block_ptr] "r" (block_ptr), [alloc_size] "r" (alloc_size), [alignment] "r" (alignment)
        : "rax", "rcx", "rdx", "rbx", "r8", "r9", "r10", "r11", "r12", "rdi", "memory"
    );
    return result;
}

// -----------------------------------------------------------------------------
// Coalescing Free Blocks in the Free List
// -----------------------------------------------------------------------------

/**
 * coalesce_free_list
 * Merges adjacent free blocks in the free list to reduce fragmentation.
 * The function builds an array of free block addresses, sorts them, and then
 * merges blocks that are physically contiguous.
 */
static void coalesce_free_list(Pool* pool) {
    int count = 0;
    BlockHeader* cur = (BlockHeader*)pool->free_list;
    while (cur) {
        count++;
        cur = (BlockHeader*)cur->next_free;
    }
    if (count == 0)
        return;
    uintptr_t* arr = (uintptr_t*)malloc(count * sizeof(uintptr_t));
    if (!arr)
        return;
    int i = 0;
    cur = (BlockHeader*)pool->free_list;
    while (cur) {
        arr[i++] = (uintptr_t)cur;
        cur = (BlockHeader*)cur->next_free;
    }
    int cmp(const void* a, const void* b) {
        uintptr_t ua = *(const uintptr_t*)a;
        uintptr_t ub = *(const uintptr_t*)b;
        return (ua < ub) ? -1 : (ua > ub) ? 1 : 0;
    }
    qsort(arr, count, sizeof(uintptr_t), cmp);
    for (i = 0; i < count - 1; i++) {
        BlockHeader* a = (BlockHeader*)arr[i];
        BlockHeader* b = (BlockHeader*)arr[i + 1];
        uintptr_t a_end = ((uintptr_t)a) + HEADER_SIZE + a->size;
        if (a_end == (uintptr_t)b) {
            a->size = a->size + HEADER_SIZE + b->size;
            arr[i + 1] = 0;  // Mark block b as merged.
        }
    }
    uintptr_t new_free_list = 0;
    BlockHeader* prev = NULL;
    for (i = 0; i < count; i++) {
        if (arr[i] != 0) {
            BlockHeader* cur_block = (BlockHeader*)arr[i];
            if (new_free_list == 0) {
                new_free_list = (uintptr_t)cur_block;
                prev = cur_block;
            } else {
                prev->next_free = (uintptr_t)cur_block;
                prev = cur_block;
            }
        }
    }
    if (prev)
        prev->next_free = 0;
    pool->free_list = new_free_list;
    free(arr);
}

// -----------------------------------------------------------------------------
// Pool Initialization, Allocation, Free, Reset, and Destroy Functions
// -----------------------------------------------------------------------------

/**
 * pool_init
 * Initializes the memory pool by allocating an initial PoolBlock and setting up
 * internal structures. Ensures that the usable memory area is 16-byte aligned.
 *
 * @param pool      Pointer to a Pool structure.
 * @param pool_size Total size (in bytes) for the initial PoolBlock.
 * @return 1 on success, 0 on failure.
 */
int pool_init(Pool* pool, size_t pool_size) {
    if (!pool || pool_size == 0)
        return 0;
    InitializeCriticalSection(&pool->lock);
    pool->free_list_lock = 0;
    size_t block_total_size = pool_size + sizeof(PoolBlock);
    PoolBlock* block = (PoolBlock*)VirtualAlloc(NULL, block_total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == 0) {
        DeleteCriticalSection(&pool->lock);
        return 0;
    }
    // Ensure the usable base is aligned to 16 bytes.
    uintptr_t base = (uintptr_t)block + sizeof(PoolBlock);
    uintptr_t aligned_base = (base + 15) & ~((uintptr_t)15);
    block->base = aligned_base;
    block->size = pool_size;
    block->offset = 0;
    block->next = 0;
    pool->block_head = (uintptr_t)block;
    pool->free_list = 0;
    pool->initial_block_size = pool_size;
    return 1;
}

/**
 * pool_alloc
 * Allocates a memory block of the specified size and alignment.
 * It first checks the free list (using first-fit), then attempts sequential allocation
 * from existing PoolBlocks, and if necessary performs dynamic expansion.
 *
 * @param pool       Pointer to the Pool structure.
 * @param alloc_size Number of bytes requested.
 * @param alignment  Alignment requirement (must be a power of 2).
 * @return Pointer to the allocated memory (user pointer), or 0 if allocation fails.
 */
uintptr_t pool_alloc(Pool* pool, size_t alloc_size, size_t alignment) {
    if (!pool || alloc_size == 0 || (alignment & (alignment - 1)) != 0)
        return 0;
    uintptr_t result = 0;
    EnterCriticalSection(&pool->lock);

    // Attempt to find a suitable free block (first-fit).
    acquire_free_list_lock(&pool->free_list_lock);
    result = remove_free_block(pool, alloc_size, alignment);
    release_free_list_lock(&pool->free_list_lock);
    if (result != 0) {
        LeaveCriticalSection(&pool->lock);
        return result;
    }

    // Allocate from existing PoolBlocks.
    uintptr_t block_ptr = pool->block_head;
    PoolBlock* block;
    while (block_ptr != 0) {
        block = (PoolBlock*)block_ptr;
        result = alloc_from_block(block_ptr, alloc_size, alignment);
        if (result != 0) {
            LeaveCriticalSection(&pool->lock);
            return result;
        }
        block_ptr = block->next;
    }

    // Dynamic Expansion: allocate a new PoolBlock if necessary.
    size_t new_block_size = pool->initial_block_size;
    if (new_block_size < alloc_size + HEADER_SIZE)
        new_block_size = alloc_size + HEADER_SIZE;
    size_t total_new_block_size = new_block_size + sizeof(PoolBlock);
    PoolBlock* new_block = (PoolBlock*)VirtualAlloc(NULL, total_new_block_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (new_block == 0) {
        LeaveCriticalSection(&pool->lock);
        return 0;
    }
    new_block->base = ((uintptr_t)new_block + sizeof(PoolBlock) + 15) & ~((uintptr_t)15);
    new_block->size = new_block_size;
    new_block->offset = 0;
    new_block->next = 0;
    if (pool->block_head == 0) {
        pool->block_head = (uintptr_t)new_block;
    } else {
        PoolBlock* last = (PoolBlock*)pool->block_head;
        while (last->next != 0) {
            last = (PoolBlock*)last->next;
        }
        last->next = (uintptr_t)new_block;
    }
    result = alloc_from_block((uintptr_t)new_block, alloc_size, alignment);
    LeaveCriticalSection(&pool->lock);
    return result;
}

/**
 * pool_free
 * Frees a previously allocated block by inserting it into the free list
 * and then coalescing adjacent free blocks.
 *
 * @param pool Pointer to the Pool structure.
 * @param ptr  User pointer to the block to free.
 */
void pool_free(Pool* pool, uintptr_t ptr) {
    if (!pool || ptr == 0)
        return;
    EnterCriticalSection(&pool->lock);
    uintptr_t header_addr = ptr - HEADER_SIZE;
    BlockHeader* header = (BlockHeader*)header_addr;
    acquire_free_list_lock(&pool->free_list_lock);
    header->next_free = pool->free_list;
    pool->free_list = header_addr;
    release_free_list_lock(&pool->free_list_lock);
    coalesce_free_list(pool);
    LeaveCriticalSection(&pool->lock);
}

/**
 * pool_reset
 * Resets the memory pool by clearing the free list and resetting the offset
 * of every PoolBlock. Also, the usable memory is cleared using SIMD/AVX.
 *
 * @param pool Pointer to the Pool structure.
 */
void pool_reset(Pool* pool) {
    if (!pool)
        return;
    EnterCriticalSection(&pool->lock);
    acquire_free_list_lock(&pool->free_list_lock);
    pool->free_list = 0;
    release_free_list_lock(&pool->free_list_lock);
    uintptr_t block_ptr = pool->block_head;
    while (block_ptr != 0) {
        PoolBlock* block = (PoolBlock*)block_ptr;
        block->offset = 0;
        simd_memset((void*)block->base, 0, block->size);
        block_ptr = block->next;
    }
    LeaveCriticalSection(&pool->lock);
}

/**
 * pool_destroy
 * Destroys the memory pool by releasing all allocated PoolBlocks and cleaning up
 * the synchronization primitives.
 *
 * @param pool Pointer to the Pool structure.
 */
void pool_destroy(Pool* pool) {
    if (!pool)
        return;
    EnterCriticalSection(&pool->lock);
    uintptr_t block_ptr = pool->block_head;
    while (block_ptr != 0) {
        PoolBlock* block = (PoolBlock*)block_ptr;
        uintptr_t next = block->next;
        VirtualFree((LPVOID)block, 0, MEM_RELEASE);
        block_ptr = next;
    }
    pool->block_head = 0;
    acquire_free_list_lock(&pool->free_list_lock);
    pool->free_list = 0;
    release_free_list_lock(&pool->free_list_lock);
    LeaveCriticalSection(&pool->lock);
    DeleteCriticalSection(&pool->lock);
}
