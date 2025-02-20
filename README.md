## 📌 Overview
This project is designed for studying and optimizing **memory allocation techniques**.
It includes implementations of **Slab Allocator** and **Pool Allocator**, both of which provide efficient memory management compared to traditional `malloc/free`.

These allocators are optimized for **performance** and **low fragmentation**, making them ideal for high-performance systems, embedded environments, and kernel-level memory management.

This project is **still in development**, and **more features will be added in the future**.

---

## 📂 Features
### ✅ **Slab Allocator**
- Uses **preallocated fixed-size memory chunks** for objects of the same size.
- **Lock-free allocation** using inline assembly for fast memory operations.
- **SIMD/AVX optimized memset** for fast zeroing and initialization.
- Supports **fast recycling** of freed objects via a free list.

### ✅ **Pool Allocator**
- **Efficient memory block allocation** with sequential allocation and free list management.
- Implements **First-Fit allocation** strategy with splitting and coalescing.
- Supports **dynamic expansion** with new memory blocks.
- **AVX-based memset optimization** for efficient memory initialization.

---

## ⚙️ How to Build
### 🔹 **Windows (MinGW-w64)**
```sh
gcc -mavx -c slab_alloc.c -o slab_alloc.o
ar rcs libslab_alloc.a slab_alloc.o
gcc bench_slab.c -L. -lslab_alloc -mavx -o bench_slab.exe
```
```sh
gcc -mavx -c pool_alloc.c -o pool_alloc.o
ar rcs libpool_alloc.a pool_alloc.o
gcc bench_pool.c -L. -lpool_alloc -mavx -o bench_pool.exe
```

### 🔹 **Run Benchmarks**
```sh
./bench_slab
./bench_pool
```

---

## 📈 Performance
| Allocator        | Allocations/sec | Free Operations/sec | Reset Time |
|-----------------|----------------|---------------------|------------|
| **Slab Allocator** | ~112M ops/sec  | ~101M ops/sec       | ~24ms      |
| **Pool Allocator** | ~11M ops/sec   | ~500K ops/sec       | ~27ms      |

---

## 📜 License
MIT

---