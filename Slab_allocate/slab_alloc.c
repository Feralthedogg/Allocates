#include "slab_alloc.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>
#include <stdio.h>

// Ensure C linkage in C++ builds.
#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Helper Function: align_size
// -----------------------------------------------------------------------------

/**
 * align_size
 * Aligns the given size to the next multiple of 16 bytes.
 *
 * @param size The size to align.
 * @return The aligned size.
 */
static size_t align_size(size_t size) {
    return (size + 15) & ~((size_t)15);
}

// -----------------------------------------------------------------------------
// Constants and Macros
// -----------------------------------------------------------------------------

// HEADER_SIZE: Reserved space at the beginning of each object for storing
// the next free pointer. Must be a multiple of 16 bytes.
#define HEADER_SIZE 32

// -----------------------------------------------------------------------------
// SIMD/AVX memset Implementation
// -----------------------------------------------------------------------------

/**
 * simd_memset
 * Sets a memory region to a specified value using AVX instructions.
 *
 * @param ptr   Pointer to the memory region.
 * @param value The value to set.
 * @param size  Number of bytes to set.
 */
static void simd_memset(void* ptr, int value, size_t size) {
    unsigned char* p = (unsigned char*)ptr;
    size_t i = 0;
    __m256i vec = _mm256_set1_epi8((char)value);
    size_t misalignment = ((uintptr_t)p) % 32;
    if (misalignment) {
         size_t align_bytes = 32 - misalignment;
         if (align_bytes > size)
             align_bytes = size;
         memset(p, value, align_bytes);
         p += align_bytes;
         size -= align_bytes;
    }
    for (i = 0; i < size / 32; i++) {
         _mm256_store_si256((__m256i*)(p + i * 32), vec);
    }
    size_t remaining = size % 32;
    if (remaining) {
         memset(p + (size - remaining), value, remaining);
    }
}

// -----------------------------------------------------------------------------
// Slab Initialization
// -----------------------------------------------------------------------------

/**
 * slab_init
 * Allocates a contiguous slab of memory for a fixed number of objects and
 * initializes the free list by linking all objects together.
 *
 * @param slab           Pointer to a Slab structure.
 * @param total_objects  Total number of objects to allocate.
 * @param object_size    Size of each object in bytes.
 * @return 1 on success, 0 on failure.
 */
__attribute__((noinline))
int slab_init(Slab *slab, size_t total_objects, size_t object_size) {
    if (!slab || total_objects == 0 || object_size < sizeof(uintptr_t))
        return 0;
    // Ensure object_size is 16-byte aligned.
    slab->object_size = align_size(object_size);
    slab->total_objects = total_objects;
    size_t slab_memory_size = slab->object_size * total_objects;
    // Allocate slab memory using VirtualAlloc.
    slab->memory = (uintptr_t)VirtualAlloc(NULL, slab_memory_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (slab->memory == 0)
        return 0;
    // Initialize the free list by linking all objects.
    slab->free_list = slab->memory;
    for (size_t i = 0; i < total_objects - 1; i++) {
        uintptr_t current_obj = slab->memory + i * slab->object_size;
        uintptr_t next_obj = slab->memory + (i + 1) * slab->object_size;
        *((uintptr_t*)current_obj) = next_obj;
    }
    uintptr_t last_obj = slab->memory + (total_objects - 1) * slab->object_size;
    *((uintptr_t*)last_obj) = 0;
    // Initialize the critical section for thread safety.
    InitializeCriticalSection(&slab->lock);
    return 1;
}

// -----------------------------------------------------------------------------
// Slab Allocation (Free List Pop) using Inline Assembly
// -----------------------------------------------------------------------------

/**
 * slab_alloc
 * Pops the first free object from the slab's free list using inline assembly.
 *
 * @param slab Pointer to the Slab structure.
 * @return Pointer to the allocated object (user pointer), or NULL if none available.
 */
__attribute__((noinline))
void* slab_alloc(Slab *slab) {
    if (!slab)
        return NULL;
    uintptr_t result = 0;
    EnterCriticalSection(&slab->lock);
    __asm__ __volatile__ (
        "movq %[free_list], %%rax\n\t"  // Load slab->free_list into rax.
        "testq %%rax, %%rax\n\t"         // Check if free_list is empty.
        "je 1f\n\t"                     // Jump if empty.
        "movq (%%rax), %%rbx\n\t"         // Load next free object's address.
        "movq %%rbx, %[free_list]\n\t"    // Update slab->free_list.
        "movq %%rax, %[result]\n\t"       // Set result to popped object.
        "jmp 2f\n\t"
        "1:\n\t"
        "movq $0, %[result]\n\t"          // Set result to 0 on failure.
        "2:\n\t"
        : [result] "=r" (result), [free_list] "+m" (slab->free_list)
        :
        : "rax", "rbx", "memory"
    );
    LeaveCriticalSection(&slab->lock);
    return (result != 0) ? (void*)(result + HEADER_SIZE) : NULL;
}

// -----------------------------------------------------------------------------
// Slab Free (Free List Push) using Inline Assembly
// -----------------------------------------------------------------------------

/**
 * slab_free
 * Pushes the freed object onto the slab's free list using inline assembly.
 *
 * @param slab Pointer to the Slab structure.
 * @param ptr  Pointer to the object to free.
 */
__attribute__((noinline))
void slab_free(Slab *slab, void* ptr) {
    if (!slab || ptr == NULL)
        return;
    EnterCriticalSection(&slab->lock);
    uintptr_t obj = (uintptr_t)ptr - HEADER_SIZE;
    __asm__ __volatile__ (
        "movq %[free_list], %%rax\n\t"   // Load current free list.
        "movq %%rax, (%%rcx)\n\t"         // Store current free list pointer into freed object's header.
        "movq %%rcx, %[free_list]\n\t"     // Update free list to point to freed object.
        : [free_list] "+m" (slab->free_list)
        : [rcx] "r" (obj)
        : "rax", "memory"
    );
    LeaveCriticalSection(&slab->lock);
}

// -----------------------------------------------------------------------------
// Slab Reset using SIMD/AVX memset
// -----------------------------------------------------------------------------

/**
 * slab_reset
 * Rebuilds the free list for all objects in the slab and clears the slab memory
 * using a SIMD/AVX optimized memset.
 *
 * @param slab Pointer to the Slab structure.
 */
__attribute__((noinline))
void slab_reset(Slab *slab) {
    if (!slab)
        return;
    EnterCriticalSection(&slab->lock);
    slab->free_list = slab->memory;
    for (size_t i = 0; i < slab->total_objects - 1; i++) {
        uintptr_t current_obj = slab->memory + i * slab->object_size;
        uintptr_t next_obj = slab->memory + (i + 1) * slab->object_size;
        *((uintptr_t*)current_obj) = next_obj;
    }
    uintptr_t last_obj = slab->memory + (slab->total_objects - 1) * slab->object_size;
    *((uintptr_t*)last_obj) = 0;
    simd_memset((void*)slab->memory, 0, slab->object_size * slab->total_objects);
    LeaveCriticalSection(&slab->lock);
}

// -----------------------------------------------------------------------------
// Slab Destroy
// -----------------------------------------------------------------------------

/**
 * slab_destroy
 * Destroys the slab allocator by releasing the allocated memory and cleaning up
 * synchronization primitives.
 *
 * @param slab Pointer to the Slab structure.
 */
__attribute__((noinline))
void slab_destroy(Slab *slab) {
    if (!slab)
        return;
    EnterCriticalSection(&slab->lock);
    VirtualFree((LPVOID)slab->memory, 0, MEM_RELEASE);
    slab->memory = 0;
    slab->free_list = 0;
    LeaveCriticalSection(&slab->lock);
    DeleteCriticalSection(&slab->lock);
}

#ifdef __cplusplus
}
#endif
