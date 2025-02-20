#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "slab_alloc.h"

int main(void) {
    // Benchmark parameters.
    const int iterations = 1000000;  // 1 million allocations.
    size_t total_objects = iterations;  // Total objects equals iterations.
    size_t object_size = 256;           // Each object is 256 bytes.

    Slab slab;
    // Initialize the slab allocator.
    if (!slab_init(&slab, total_objects, object_size)) {
        printf("Slab initialization failed.\n");
        return 1;
    }
    printf("Slab initialized: total objects = %zu, object size = %zu bytes\n",
           slab.total_objects, slab.object_size);

    // Retrieve high-resolution timer frequency.
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);

    // Allocate an array to hold allocated pointers.
    void** allocations = (void**)malloc(iterations * sizeof(void*));
    if (allocations == NULL) {
        printf("Failed to allocate pointer array.\n");
        slab_destroy(&slab);
        return 1;
    }

    LARGE_INTEGER start, end;

    // Benchmark slab_alloc.
    QueryPerformanceCounter(&start);
    for (int i = 0; i < iterations; i++) {
        allocations[i] = slab_alloc(&slab);
        if (allocations[i] == NULL) {
            printf("Allocation failed at iteration %d.\n", i);
            break;
        }
    }
    QueryPerformanceCounter(&end);
    double allocTime = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    printf("256-byte slab allocation, %d iterations: %.6f seconds (%.2f ops/sec)\n",
           iterations, allocTime, iterations / allocTime);

    // Benchmark slab_free.
    QueryPerformanceCounter(&start);
    for (int i = 0; i < iterations; i++) {
        slab_free(&slab, allocations[i]);
    }
    QueryPerformanceCounter(&end);
    double freeTime = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    printf("Slab free operations, %d iterations: %.6f seconds (%.2f ops/sec)\n",
           iterations, freeTime, iterations / freeTime);

    // Benchmark slab_reset.
    QueryPerformanceCounter(&start);
    slab_reset(&slab);
    QueryPerformanceCounter(&end);
    double resetTime = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    printf("Slab reset time: %.6f seconds\n", resetTime);

    free(allocations);
    slab_destroy(&slab);
    printf("Slab destroyed.\n");

    return 0;
}
