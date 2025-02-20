// pool_alloc.h

#ifndef POOL_ALLOC_H
#define POOL_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <windows.h>  // For CRITICAL_SECTION and Interlocked functions

// BlockHeader structure (24 bytes)
// Layout:
//   [0:7]   : size         - requested allocation size
//   [8:15]  : padding      - alignment adjustment
//   [16:23] : next_free    - pointer to next free block (stored as uintptr_t)
typedef struct BlockHeader {
    size_t size;
    size_t padding;
    uintptr_t next_free;
} BlockHeader;

// PoolBlock structure represents one contiguous memory block allocated via VirtualAlloc.
// The structure is stored at the beginning of the block.
// Layout:
//   [0:7]   : base   - usable memory starts at (base)
//   [8:15]  : size   - total usable size (in bytes)
//   [16:23] : offset - current allocation offset (in bytes)
//   [24:31] : next   - pointer to next PoolBlock (stored as uintptr_t)
typedef struct PoolBlock {
    uintptr_t base;
    size_t size;
    size_t offset;
    uintptr_t next;
} PoolBlock;

// Pool structure representing the entire memory pool.
// It maintains a linked list of PoolBlock, a free list (of freed blocks), a thread lock,
// a separate spin lock for free list operations, and the initial block size for dynamic resizing.
typedef struct Pool {
    uintptr_t block_head;       // Pointer (as integer) to the first PoolBlock.
    uintptr_t free_list;        // Pointer (as integer) to the head of the free list (BlockHeader).
    CRITICAL_SECTION lock;      // Lock for overall pool operations.
    volatile LONG free_list_lock;  // Spin lock for free list operations (minimize contention).
    size_t initial_block_size;  // Initial block size for dynamic resizing.
} Pool;

/**
 * pool_init
 * Initializes the memory pool by allocating an initial block with VirtualAlloc.
 * Also initializes the thread lock and the free list spin lock.
 *
 * @param pool       Pointer to a Pool structure.
 * @param pool_size  Size of the initial usable memory block (in bytes).
 * @return           1 on success, 0 on failure.
 */
int pool_init(Pool* pool, size_t pool_size);

/**
 * pool_alloc
 * Allocates a memory block of the requested size with the specified alignment.
 * The function first checks the free list (using best-fit selection with splitting)
 * for a suitable block, then attempts sequential allocation from existing pool blocks,
 * and finally dynamically allocates a new block if needed.
 *
 * A 24-byte header is stored immediately before the returned block.
 *
 * @param pool       Pointer to the Pool structure.
 * @param alloc_size Number of bytes to allocate.
 * @param alignment  Alignment requirement (must be a power of 2).
 * @return           Allocated memory address (as uintptr_t) or 0 on failure.
 */
uintptr_t pool_alloc(Pool* pool, size_t alloc_size, size_t alignment);

/**
 * pool_free
 * Frees a previously allocated memory block by adding it to the pool's free list.
 *
 * @param pool  Pointer to the Pool structure.
 * @param ptr   Memory block to free (as returned by pool_alloc).
 */
void pool_free(Pool* pool, uintptr_t ptr);

/**
 * pool_reset
 * Resets the memory pool by setting all pool blocks' offsets to 0,
 * clearing the free list, and zeroing out the pool memory using SIMD/AVX.
 *
 * @param pool  Pointer to the Pool structure.
 */
void pool_reset(Pool* pool);

/**
 * pool_destroy
 * Destroys the memory pool by freeing all allocated blocks and deleting the thread lock.
 *
 * @param pool  Pointer to the Pool structure.
 */
void pool_destroy(Pool* pool);

#ifdef __cplusplus
}
#endif

#endif // POOL_ALLOC_H
