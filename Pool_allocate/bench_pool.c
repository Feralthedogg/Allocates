// bench_pool.c

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "pool_alloc.h"

int main(void) {
    // Benchmark parameters
    const int iterations = 1000000;  // 1 million allocations
    size_t pool_size = 1024 * 1024 * 10;  // 10MB initial pool

    Pool pool;
    // Initialize the memory pool.
    if (!pool_init(&pool, pool_size)) {
        printf("Memory pool initialization failed.\n");
        return 1;
    }
    printf("Memory pool initialized: initial block size = %zu bytes\n", pool.initial_block_size);

    // Retrieve high-resolution timer frequency.
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);

    // Allocate an array to hold allocated pointers for later free.
    uintptr_t *allocations = (uintptr_t*)malloc(iterations * sizeof(uintptr_t));
    if (allocations == NULL) {
        printf("Failed to allocate pointer array.\n");
        pool_destroy(&pool);
        return 1;
    }

    LARGE_INTEGER start, end;
    
    // Benchmark pool_alloc.
    QueryPerformanceCounter(&start);
    for (int i = 0; i < iterations; i++) {
        allocations[i] = pool_alloc(&pool, 256, 16);
        if (allocations[i] == 0) {
            printf("Allocation failed at iteration %d.\n", i);
            break;
        }
    }
    QueryPerformanceCounter(&end);
    double allocTime = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    printf("256-byte allocation, %d iterations: %.6f seconds (%.2f ops/sec)\n", 
           iterations, allocTime, iterations / allocTime);

    // Benchmark pool_free.
    QueryPerformanceCounter(&start);
    for (int i = 0; i < iterations; i++) {
        pool_free(&pool, allocations[i]);
    }
    QueryPerformanceCounter(&end);
    double freeTime = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    printf("Free operations, %d iterations: %.6f seconds (%.2f ops/sec)\n", 
           iterations, freeTime, iterations / freeTime);

    // Benchmark pool_reset.
    QueryPerformanceCounter(&start);
    pool_reset(&pool);
    QueryPerformanceCounter(&end);
    double resetTime = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    printf("Pool reset time: %.6f seconds\n", resetTime);

    free(allocations);
    pool_destroy(&pool);
    printf("Memory pool destroyed.\n");

    return 0;
}
