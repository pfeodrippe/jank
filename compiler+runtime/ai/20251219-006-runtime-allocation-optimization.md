# Runtime Allocation Optimization for Fast Loops in jank

## Executive Summary

**Goal**: Enable fast loops and user-controlled memory allocation in jank runtime.

**The Problem**: A simple loop like this:
```clojure
(loop [i 0 sum 0]
  (if (< i 1000000)
    (recur (inc i) (+ sum i))
    sum))
```
Currently allocates **4-6 million boxed integers** due to boxing/unboxing on every arithmetic operation.

**The Solution**: A multi-pronged approach:
1. **Integer caching** (like JVM's -128 to 127 cache) - Quick win
2. **Thread-Local Allocation Buffers (TLABs)** - Medium effort, big impact
3. **Object pools for hot types** - Runtime-controlled allocation
4. **Escape analysis** (long-term) - Eliminate allocations entirely
5. **User-controlled arenas** - Explicit memory control for power users

---

## Current State Analysis

### How Loops Work in jank

From `codegen/processor.cpp`, loops compile to:
```cpp
{
  auto i{ /* initial value */ };    // Mutable shadow variable
  auto sum{ /* initial value */ };

  while(true) {
    if(/* condition */) {
      // recur: assign new values and continue
      i = /* new i */;
      sum = /* new sum */;
      continue;
    } else {
      return sum;
    }
  }
}
```

**Good**: Native C++ while loop, no allocation for loop structure itself.

**Bad**: Every `(inc i)` and `(+ sum i)` calls `dynamic_call()` which requires boxing.

### Current Allocation Path

From `oref.hpp`:
```cpp
template <typename T, typename... Args>
oref<T> make_box(Args &&...args) {
    T *ret{};
    if constexpr(T::pointer_free) {
        ret = new(PointerFreeGC) T{ std::forward<Args>(args)... };  // HERE
    } else {
        ret = new(GC) T{ std::forward<Args>(args)... };
    }
    return ret;
}
```

**Every boxed integer = one GC allocation.**

### What Gets Allocated Per Loop Iteration

For `(recur (inc i) (+ sum i))`:

| Operation | Boxing Operations | Allocations |
|-----------|------------------|-------------|
| `(inc i)` | Box i, allocate result | 2 |
| `(+ sum i)` | Box sum, box i (result of inc), allocate result | 2-3 |
| **Total per iteration** | | **4-5** |

For 1 million iterations: **4-5 million allocations**.

---

## Expert Research Findings

### JVM's Integer Cache

From the JVM specification:
> "If the value p being boxed is between -128 and 127, then the result is a reference to a cached Integer object."

**Impact**: Common loop values (0, 1, etc.) never allocate.

### Thread-Local Allocation Buffers (TLABs)

From [JVM Anatomy Quark #4](https://shipilev.net/jvm/anatomy-quarks/4-tlab-allocation/):
> "TLABs satisfy most allocation requests with a simple, non-atomic, increment of a pointer... Allocation inside TLAB is a simple pointer bump."

From [Baeldung - What is a TLAB](https://www.baeldung.com/java-jvm-tlab):
> "The average time with TLAB is 33 ms, and the average without goes up to 110 ms. That's an increase of **230%**."

### Object Pools for Games

From [Game Programming Patterns - Object Pool](https://gameprogrammingpatterns.com/object-pool.html):
> "An object pool gives us the best of both worlds. To the memory manager, we're just allocating one big hunk of memory up front... To the users of the pool, we can freely allocate and deallocate objects."

From [metapool](https://github.com/esterlein/metapool):
> "Up to ~1300x faster than malloc."

### Escape Analysis

From [JVM Escape Analysis](https://medium.com/@AlexanderObregon/the-purpose-and-mechanics-of-escape-analysis-in-the-jvm-f02c17860b8c):
> "To the JVM, an object might not need to exist in memory at all. If it's confident that an object doesn't leave the method, and only the fields are used, it can treat the fields as local integers."

From [JVM Advent - Escape Analysis](https://www.javaadvent.com/2020/12/seeing-escape-analysis-working.html):
> "There is **no allocation inside the loop** after optimization."

### Clojure's Approach

From [Clojure Reference - Transients](https://clojure.org/reference/transients):
> "Transient usage cut runtime on aggregation tasks by nearly **40%**."

From [Avoiding Memory Bloat](https://moldstud.com/articles/p-avoiding-memory-bloat-in-clojure-applications-practical-tips-for-efficient-programming):
> "Adding `^long` type hints to numeric arguments and return values instructs the compiler to bypass boxing."

---

## Implementation Plan

### Phase 1: Integer Cache (QUICK WIN)

**Effort**: 2-4 hours
**Expected Improvement**: 50-90% reduction in integer allocations for typical code

Like the JVM, cache small integers:

```cpp
// include/cpp/jank/runtime/integer_cache.hpp
namespace jank::runtime
{
    struct integer_cache
    {
        static constexpr i64 cache_low{ -128 };
        static constexpr i64 cache_high{ 1024 };  // Higher than JVM for loop counters
        static constexpr usize cache_size{ cache_high - cache_low + 1 };

        static obj::integer_ref cache[cache_size];

        static void initialize();  // Called at runtime startup

        [[gnu::hot, gnu::flatten]]
        static obj::integer_ref get(i64 value) {
            if(value >= cache_low && value <= cache_high) {
                return cache[value - cache_low];
            }
            return make_box<obj::integer>(value);
        }
    };
}
```

**Modify `make_box(i64)`**:
```cpp
[[gnu::flatten, gnu::hot]]
inline auto make_box(i64 const i) {
    return integer_cache::get(i);  // Returns cached or allocates
}
```

**Why this works**: Loop counters (0, 1, 2, ...) and common values (-1, 0, 1, 10, 100) hit cache.

### Phase 2: Thread-Local Allocation Buffers (MEDIUM EFFORT)

**Effort**: 1-2 days
**Expected Improvement**: 200-300% faster allocation (based on JVM benchmarks)

Implement TLABs on top of Boehm GC:

```cpp
// include/cpp/jank/runtime/tlab.hpp
namespace jank::runtime
{
    struct tlab
    {
        static constexpr usize default_size{ 64 * 1024 };  // 64KB per thread

        char* start{};
        char* current{};
        char* end{};

        tlab();
        ~tlab();

        // Fast path: bump pointer allocation
        template<typename T, typename... Args>
        [[gnu::hot, gnu::always_inline]]
        T* alloc(Args&&... args) {
            constexpr usize size{ sizeof(T) };
            constexpr usize alignment{ alignof(T) };

            // Align current pointer
            char* aligned = reinterpret_cast<char*>(
                (reinterpret_cast<uintptr_t>(current) + alignment - 1) & ~(alignment - 1)
            );

            // Fast path: fits in TLAB
            if(aligned + size <= end) [[likely]] {
                current = aligned + size;
                return new(aligned) T{ std::forward<Args>(args)... };
            }

            // Slow path: refill TLAB or allocate directly from GC
            return alloc_slow<T>(std::forward<Args>(args)...);
        }

    private:
        template<typename T, typename... Args>
        T* alloc_slow(Args&&... args);

        void refill();
    };

    // Thread-local TLAB instance
    inline thread_local tlab current_tlab;
}
```

**Critical**: TLAB memory must be GC-visible. Options:
1. Allocate TLAB chunks from `GC_malloc` (Boehm will scan them)
2. Register TLAB memory regions with `GC_add_roots`

**Modify `make_box<T>()`**:
```cpp
template <typename T, typename... Args>
oref<T> make_box(Args &&...args) {
    if constexpr(T::pointer_free && sizeof(T) <= 256) {
        // Small pointer-free objects: use TLAB
        return current_tlab.alloc<T>(std::forward<Args>(args)...);
    } else {
        // Large or pointer-containing: use GC directly
        return new(GC) T{ std::forward<Args>(args)... };
    }
}
```

### Phase 3: Object Pools for Hot Types (MEDIUM EFFORT)

**Effort**: 1 day
**Expected Improvement**: 10-50x faster allocation for pooled types

For frequently allocated types (integers, reals), maintain per-type pools:

```cpp
// include/cpp/jank/runtime/object_pool.hpp
namespace jank::runtime
{
    template<typename T>
    struct object_pool
    {
        static constexpr usize pool_size{ 4096 };  // Objects per pool

        struct pool_block {
            pool_block* next{};
            usize free_count{ pool_size };
            T objects[pool_size];
            u32 free_list[pool_size];  // Indices of free objects
        };

        pool_block* current_block{};

        [[gnu::hot]]
        T* acquire() {
            if(!current_block || current_block->free_count == 0) {
                allocate_block();
            }
            auto idx = current_block->free_list[--current_block->free_count];
            return &current_block->objects[idx];
        }

        [[gnu::hot]]
        void release(T* obj) {
            // Find which block this object belongs to
            // Add index back to free list
            // (Can be deferred to GC if using weak refs)
        }

    private:
        void allocate_block();
    };

    // Specialized pools for common types
    inline thread_local object_pool<obj::integer> integer_pool;
    inline thread_local object_pool<obj::real> real_pool;
}
```

**Alternative: Free-list in GC finalization**

Instead of explicit `release()`, use GC finalizers:
```cpp
// When GC collects an integer, add it back to pool
GC_register_finalizer(ptr, [](void* obj, void* pool) {
    static_cast<object_pool<obj::integer>*>(pool)->release(
        static_cast<obj::integer*>(obj)
    );
}, &integer_pool, nullptr, nullptr);
```

### Phase 4: Primitive Loop Optimization (COMPILER ENHANCEMENT)

**Effort**: 2-3 days
**Expected Improvement**: 100-1000x for tight arithmetic loops

Detect and optimize primitive loops at compile time:

```clojure
;; This loop:
(loop [i 0 sum 0]
  (if (< i 1000000)
    (recur (inc i) (+ sum i))
    sum))

;; Should compile to:
{
  i64 i{ 0 };       // Unboxed primitive
  i64 sum{ 0 };     // Unboxed primitive

  while(true) {
    if(i < 1000000LL) {
      ++i;           // No boxing!
      sum += i;      // No boxing!
      continue;
    } else {
      return make_box(sum);  // Box only on return
    }
  }
}
```

**Implementation in analyzer**:

1. **Loop variable type inference**:
   - If initial value is integer literal and all updates are arithmetic → unboxed i64
   - Track through `recur` forms

2. **Operation specialization**:
   - `(inc x)` where x is i64 → `++x`
   - `(+ x y)` where both i64 → `x + y`
   - `(< x y)` where both i64 → `x < y` (returns bool)

3. **Late boxing**:
   - Only box when value escapes loop (return, passed to function)

### Phase 5: User-Controlled Arenas (ADVANCED)

**Effort**: 3-5 days
**Expected Improvement**: User-controlled, potentially 50-100x for arena-scoped code

Expose arena allocation to jank code:

```clojure
;; API Design
(require '[jank.arena :as arena])

;; Create arena (pre-allocates memory)
(def my-arena (arena/create {:size (* 1024 1024)}))  ; 1MB

;; Allocate from arena
(arena/with-arena my-arena
  ;; All allocations in this scope use the arena
  (loop [result [] i 0]
    (if (< i 1000)
      (recur (conj result (* i i)) (inc i))
      result)))
;; Arena automatically reset/freed when scope exits

;; Manual control for advanced users
(arena/reset! my-arena)  ; Reuse without freeing
(arena/free! my-arena)   ; Release memory
```

**Implementation**:

```cpp
// Runtime support
namespace jank::runtime::obj
{
    struct arena : gc
    {
        static constexpr object_type obj_type{ object_type::arena };
        static constexpr bool pointer_free{ false };

        object base{ obj_type };

        char* memory{};
        usize size{};
        usize used{};

        arena(usize size);
        ~arena();

        void* alloc(usize bytes, usize alignment);
        void reset();
    };
}

// Thread-local current arena (nullptr = use normal allocation)
inline thread_local obj::arena* current_arena{ nullptr };

// Scoped arena binding
struct arena_scope {
    obj::arena* previous;
    arena_scope(obj::arena* a) : previous(current_arena) { current_arena = a; }
    ~arena_scope() { current_arena = previous; }
};
```

**Modify `make_box<T>()`** to check arena:
```cpp
template <typename T, typename... Args>
oref<T> make_box(Args &&...args) {
    if(current_arena) {
        void* mem = current_arena->alloc(sizeof(T), alignof(T));
        return new(mem) T{ std::forward<Args>(args)... };
    }
    // ... normal allocation path
}
```

### Phase 6: Escape Analysis (LONG-TERM)

**Effort**: 2-4 weeks
**Expected Improvement**: Eliminate allocation for non-escaping objects

Full escape analysis like JVM's scalar replacement:

1. **Track object lifetime** in analyzer
2. **Identify non-escaping objects**:
   - Created and used only within function
   - Not stored in collections/vars
   - Not passed to unknown functions
3. **Replace with stack allocation or registers**

This is complex but provides the ultimate optimization - zero allocation for local computations.

---

## Implementation Priority Matrix

| Phase | Effort | Impact | Risk | Priority |
|-------|--------|--------|------|----------|
| 1. Integer Cache | Low | High | Low | **P0** |
| 2. TLABs | Medium | High | Medium | **P1** |
| 3. Object Pools | Medium | Medium | Low | **P2** |
| 4. Primitive Loops | Medium | Very High | Medium | **P1** |
| 5. User Arenas | High | Medium | Low | **P3** |
| 6. Escape Analysis | Very High | Very High | High | **P4** |

---

## Quick Wins Summary

### Integer Cache (Phase 1)
```cpp
// Before (every integer allocates)
make_box<obj::integer>(42);  // GC allocation

// After (cache hit = no allocation)
integer_cache::get(42);  // Returns cached pointer
```

### Boehm GC Thread-Local (Immediate)
Just enable in CMakeLists.txt:
```cmake
target_compile_definitions(bdwgc PRIVATE
    GC_THREADS
    THREAD_LOCAL_ALLOC
    PARALLEL_MARK
)
```

### Type Hints for Primitives (User-facing)
```clojure
;; Today (boxes on every operation)
(loop [i 0] ...)

;; Future (stays unboxed)
(loop [^long i 0] ...)
```

---

## Benchmarking Plan

Before implementing, establish baselines:

```clojure
;; Benchmark 1: Tight arithmetic loop
(defn bench-loop []
  (loop [i 0 sum 0]
    (if (< i 1000000)
      (recur (inc i) (+ sum i))
      sum)))

;; Benchmark 2: Collection building
(defn bench-build []
  (loop [v [] i 0]
    (if (< i 10000)
      (recur (conj v i) (inc i))
      v)))

;; Benchmark 3: Sequence processing
(defn bench-seq []
  (reduce + (range 1000000)))
```

Measure:
1. Execution time
2. GC pause count and duration
3. Peak memory usage
4. Allocation count (via GC stats)

---

## Conclusion

Runtime allocation optimization for fast loops requires a multi-layered approach:

1. **Integer caching** eliminates most loop counter allocations (quick win)
2. **TLABs** make remaining allocations 2-3x faster (proven JVM technique)
3. **Primitive loop optimization** eliminates boxing entirely for pure arithmetic
4. **User-controlled arenas** give power users explicit memory control
5. **Escape analysis** is the ultimate goal but requires significant investment

The combination of integer caching + TLABs + primitive loop optimization should make jank competitive with JVM Clojure for numeric code, while user arenas provide an escape hatch for performance-critical applications.

---

## Sources

- [JVM Anatomy Quark #4: TLAB Allocation](https://shipilev.net/jvm/anatomy-quarks/4-tlab-allocation/)
- [JVM Anatomy Quark #18: Scalar Replacement](https://shipilev.net/jvm/anatomy-quarks/18-scalar-replacement/)
- [Escape Analysis in the JVM](https://medium.com/@AlexanderObregon/the-purpose-and-mechanics-of-escape-analysis-in-the-jvm-f02c17860b8c)
- [Seeing Escape Analysis Working](https://www.javaadvent.com/2020/12/seeing-escape-analysis-working.html)
- [What is a TLAB in Java](https://www.baeldung.com/java-jvm-tlab)
- [Why TLABs are Important](https://www.opsian.com/blog/jvm-tlabs-important-multicore/)
- [Game Programming Patterns - Object Pool](https://gameprogrammingpatterns.com/object-pool.html)
- [Pool Allocator for Games](https://medium.com/@mateusgondimlima/designing-and-implementing-a-pool-allocator-data-structure-for-memory-management-in-games-c78ed0902b69)
- [metapool - 1300x faster than malloc](https://github.com/esterlein/metapool)
- [Clojure Transients](https://clojure.org/reference/transients)
- [Clojure Memory Optimization](https://moldstud.com/articles/p-avoiding-memory-bloat-in-clojure-applications-practical-tips-for-efficient-programming)
- [Region-Based Memory Management](https://en.wikipedia.org/wiki/Region-based_memory_management)
