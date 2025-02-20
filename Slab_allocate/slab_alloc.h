#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <windows.h>  // For CRITICAL_SECTION and Interlocked functions

/**
 * Slab structure
 *
 * This slab allocator provides fixed-size object allocation from a pre-allocated
 * contiguous memory region. Each object reserves its first sizeof(uintptr_t) bytes
 * to store the address of the next free object.
 */
typedef struct Slab {
    uintptr_t memory;       // Base address of the allocated slab memory.
    size_t object_size;     // Size of each object (>= sizeof(uintptr_t) and 16-byte aligned).
    size_t total_objects;   // Total number of objects in the slab.
    uintptr_t free_list;    // Address of the first free object.
    CRITICAL_SECTION lock;  // Synchronization primitive for thread safety.
} Slab;

/**
 * slab_init
 * Initializes the slab allocator by allocating a contiguous memory region (the slab)
 * and setting up the free list for fixed-size object allocation.
 *
 * @param slab           Pointer to a Slab structure.
 * @param total_objects  Total number of objects to allocate.
 * @param object_size    Size of each object in bytes.
 * @return 1 on success, 0 on failure.
 */
int slab_init(Slab *slab, size_t total_objects, size_t object_size);

/**
 * slab_alloc
 * Allocates an object from the slab.
 *
 * @param slab Pointer to the Slab structure.
 * @return Pointer to the allocated object, or NULL if no free objects remain.
 */
void* slab_alloc(Slab *slab);

/**
 * slab_free
 * Frees a previously allocated object by pushing it onto the free list.
 *
 * @param slab Pointer to the Slab structure.
 * @param ptr  Pointer to the object to free.
 */
void slab_free(Slab *slab, void* ptr);

/**
 * slab_reset
 * Resets the slab allocator by rebuilding the free list for all objects and
 * clearing the slab memory using a SIMD/AVX optimized memset.
 *
 * @param slab Pointer to the Slab structure.
 */
void slab_reset(Slab *slab);

/**
 * slab_destroy
 * Destroys the slab allocator by releasing the allocated memory and cleaning up
 * synchronization primitives.
 *
 * @param slab Pointer to the Slab structure.
 */
void slab_destroy(Slab *slab);

#ifdef __cplusplus
}
#endif

#endif // SLAB_ALLOC_H
