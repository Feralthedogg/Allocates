// slab_alloc.h

#ifndef SLAB_ALLOC_H
#define SLAB_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <windows.h>

/**
 * Slab structure
 *
 * This slab allocator allocates fixed-size objects from a contiguous memory region
 * created via memory mapping (Windows equivalent of mmap). Each object reserves its
 * first HEADER_SIZE bytes to store a pointer to the next free object.
 */
typedef struct Slab {
    unsigned char* memory;       // Base address of the memory mapped slab.
    size_t object_size;          // Size of each object (>= sizeof(void*) and 16-byte aligned).
    size_t total_objects;        // Total number of objects in the slab.
    void* free_list;             // Pointer to the first node in the free list.
    HANDLE mappingHandle;        // Handle used for memory mapping (mmap equivalent).
    CRITICAL_SECTION lock;       // Synchronization object for thread safety during reset/destroy.
} Slab;

#define HEADER_SIZE 32  // Reserved bytes at the start of each object for storing the next pointer.

/**
 * slab_init
 * Initializes the slab allocator by creating a memory mapping and linking all objects
 * into a free list.
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
 * @return Pointer to the allocated object (offset by HEADER_SIZE), or NULL if no object is available.
 */
void* slab_alloc(Slab *slab);

/**
 * slab_free
 * Returns a previously allocated object back to the free list.
 *
 * @param slab Pointer to the Slab structure.
 * @param ptr  Pointer to the object to free.
 */
void slab_free(Slab *slab, void* ptr);

/**
 * slab_reset
 * Rebuilds the free list for all objects in the slab and clears the slab memory
 * using a SIMD/AVX optimized memset.
 *
 * @param slab Pointer to the Slab structure.
 */
void slab_reset(Slab *slab);

/**
 * slab_destroy
 * Destroys the slab allocator by unmapping the memory, closing the mapping handle,
 * and cleaning up the synchronization objects.
 *
 * @param slab Pointer to the Slab structure.
 */
void slab_destroy(Slab *slab);

#ifdef __cplusplus
}
#endif

#endif // SLAB_ALLOC_H
