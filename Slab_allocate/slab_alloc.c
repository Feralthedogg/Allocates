// slab_alloc.c

#include "slab_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

/**
 * align_size
 * Aligns the given size to the next multiple of 16 bytes.
 *
 * @param size The size to align.
 * @return The size aligned to 16 bytes.
 */
static size_t align_size(size_t size) {
    return (size + 15) & ~((size_t)15);
}

/**
 * simd_memset
 * Fills a memory region with a specified value using AVX instructions.
 *
 * @param ptr   Pointer to the memory region.
 * @param value The value to set.
 * @param size  Number of bytes to fill.
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

/**
 * slab_init
 * Allocates the slab memory using memory mapping (CreateFileMapping/MapViewOfFile)
 * and initializes the free list by linking all objects.
 *
 * @param slab           Pointer to a Slab structure.
 * @param total_objects  Total number of objects to allocate.
 * @param object_size    Size of each object in bytes.
 * @return 1 on success, 0 on failure.
 */
int slab_init(Slab *slab, size_t total_objects, size_t object_size) {
    if (!slab || total_objects == 0 || object_size < sizeof(void*))
        return 0;
    slab->object_size = align_size(object_size);
    slab->total_objects = total_objects;
    size_t slab_memory_size = slab->object_size * total_objects;
    
    // Create a memory mapping (Windows equivalent of mmap).
    slab->mappingHandle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                             0, (DWORD)slab_memory_size, NULL);
    if (slab->mappingHandle == NULL)
        return 0;
    slab->memory = (unsigned char*)MapViewOfFile(slab->mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, slab_memory_size);
    if (slab->memory == NULL) {
        CloseHandle(slab->mappingHandle);
        return 0;
    }
    
    // Initialize the free list by linking all objects.
    slab->free_list = slab->memory;
    for (size_t i = 0; i < total_objects; i++) {
        unsigned char* current_obj = slab->memory + i * slab->object_size;
        unsigned char* next_obj = (i < total_objects - 1) ? (slab->memory + (i + 1) * slab->object_size) : NULL;
        *((void**)current_obj) = next_obj;
    }
    
    // Initialize the critical section for thread safety during reset and destroy.
    InitializeCriticalSection(&slab->lock);
    return 1;
}

/**
 * slab_alloc
 * Pops an object from the free list using inline assembly to manipulate registers
 * (RAX, RBX). The returned pointer is offset by HEADER_SIZE.
 */
void* slab_alloc(Slab *slab) {
    if (!slab)
        return NULL;
    uintptr_t result = 0;
    __asm__ __volatile__ (
        "movq %[free_list], %%rax\n\t"   // Load free_list address into RAX.
        "testq %%rax, %%rax\n\t"          // Check if free_list is empty.
        "je 1f\n\t"
        "movq (%%rax), %%rbx\n\t"         // Load the next pointer from the current object into RBX.
        "movq %%rbx, %[free_list]\n\t"    // Update free_list to the next object.
        "movq %%rax, %[result]\n\t"       // Store the allocated object's address in result.
        "jmp 2f\n\t"
        "1:\n\t"
        "movq $0, %[result]\n\t"          // Set result to 0 if free_list is empty.
        "2:\n\t"
        : [result] "=r" (result), [free_list] "+m" (slab->free_list)
        :
        : "rax", "rbx", "memory"
    );
    return (result != 0) ? (void*)(result + HEADER_SIZE) : NULL;
}

/**
 * slab_free
 * Pushes a freed object onto the free list using inline assembly.
 * Directly manipulates registers (RAX, RCX) to update the free list.
 *
 * @param slab Pointer to the Slab structure.
 * @param ptr  Pointer to the object to free.
 */
void slab_free(Slab *slab, void* ptr) {
    if (!slab || ptr == NULL)
        return;
    uintptr_t obj = (uintptr_t)ptr - HEADER_SIZE;
    __asm__ __volatile__ (
        "movq %[free_list], %%rax\n\t"   // Load current free_list into RAX.
        "movq %%rax, (%%rcx)\n\t"         // Store current free_list pointer into the freed object's header.
        "movq %%rcx, %[free_list]\n\t"     // Update free_list to point to the freed object.
        : [free_list] "+m" (slab->free_list)
        : [rcx] "r" (obj)
        : "rax", "memory"
    );
}

/**
 * slab_reset
 * Rebuilds the free list for all objects in the slab and clears the slab memory using
 * an AVX/SIMD optimized memset.
 *
 * @param slab Pointer to the Slab structure.
 */
void slab_reset(Slab *slab) {
    if (!slab)
        return;
    EnterCriticalSection(&slab->lock);
    slab->free_list = slab->memory;
    for (size_t i = 0; i < slab->total_objects; i++) {
        unsigned char* current_obj = slab->memory + i * slab->object_size;
        unsigned char* next_obj = (i < slab->total_objects - 1) ? (slab->memory + (i + 1) * slab->object_size) : NULL;
        *((void**)current_obj) = next_obj;
    }
    simd_memset(slab->memory, 0, slab->object_size * slab->total_objects);
    LeaveCriticalSection(&slab->lock);
}

/**
 * slab_destroy
 * Destroys the slab allocator by unmapping the memory view, closing the mapping handle,
 * and cleaning up the critical section.
 *
 * @param slab Pointer to the Slab structure.
 */
void slab_destroy(Slab *slab) {
    if (!slab)
        return;
    EnterCriticalSection(&slab->lock);
    UnmapViewOfFile(slab->memory);
    CloseHandle(slab->mappingHandle);
    slab->memory = NULL;
    slab->free_list = NULL;
    LeaveCriticalSection(&slab->lock);
    DeleteCriticalSection(&slab->lock);
}
