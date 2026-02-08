# Immer Custom Heap for Arena Allocator Integration

## Summary

This document analyzes how to integrate jank's `current_allocator` system with immer's persistent data structures (vectors, maps, etc.) so that when an arena/custom allocator is active, immer allocations go through it instead of directly to the GC.

## Current Architecture

### Jank's Allocator System

```
current_allocator (thread_local)
        │
        ▼
try_allocator_alloc(size, alignment)
        │
        ├── if current_allocator != nullptr → current_allocator->alloc(size, alignment)
        │
        └── if current_allocator == nullptr → return nullptr (use GC)
```

### Immer's Memory Policy System

Jank currently uses (from `type.hpp:31-36`):

```cpp
using memory_policy = immer::memory_policy<immer::heap_policy<immer::gc_heap>,
                                           immer::no_refcount_policy,
                                           immer::default_lock_policy,
                                           immer::gc_transience_policy,
                                           false>;
```

This means:
- **HeapPolicy**: `heap_policy<gc_heap>` - uses Boehm GC directly
- **RefcountPolicy**: `no_refcount_policy` - GC handles lifetimes
- **TransiencePolicy**: `gc_transience_policy` - GC-compatible transient ops

### Immer's Heap Interface (from `gc_heap.hpp`)

```cpp
class gc_heap
{
public:
    // Normal allocation (may contain references to other GC objects)
    static void* allocate(std::size_t n)
    {
        IMMER_GC_INIT_GUARD_;
        auto p = GC_malloc(n);
        if (IMMER_UNLIKELY(!p))
            IMMER_THROW(std::bad_alloc{});
        return p;
    }

    // Atomic allocation (no references - e.g., for leaf data)
    static void* allocate(std::size_t n, norefs_tag)
    {
        IMMER_GC_INIT_GUARD_;
        auto p = GC_malloc_atomic(n);
        if (IMMER_UNLIKELY(!p))
            IMMER_THROW(std::bad_alloc{});
        return p;
    }

    static void deallocate(std::size_t, void* data) { GC_free(data); }
    static void deallocate(std::size_t, void* data, norefs_tag) { GC_free(data); }
};
```

**Key observation**: Heap methods are **static**. Immer states "currently, heaps can only have global state." However, `thread_local` variables ARE accessible from static methods, which is exactly what we need!

## Proposed Solution: `jank_heap`

Create a custom heap that checks `current_allocator` first, falling back to GC:

```cpp
// In include/cpp/jank/runtime/core/jank_heap.hpp

#pragma once

#include <immer/config.hpp>
#include <immer/heap/tags.hpp>
#include <gc/gc.h>
#include <jank/runtime/core/arena.hpp>

namespace jank::runtime
{

/*!
 * Custom heap for immer that respects jank's current_allocator.
 *
 * When current_allocator is set (e.g., inside with-arena), allocations
 * go through the custom allocator. Otherwise, falls back to Boehm GC.
 *
 * This allows persistent data structures (vectors, maps) to be allocated
 * from arenas when desired, while maintaining GC compatibility by default.
 */
class jank_heap
{
public:
    /*!
     * Allocate memory that may contain references to other GC objects.
     * Uses current_allocator if set, otherwise GC_malloc.
     */
    [[gnu::hot]]
    static void* allocate(std::size_t n)
    {
        // Check thread-local allocator first (fast path when not set)
        if(current_allocator) [[unlikely]]
        {
            return current_allocator->alloc(n, 16);
        }

        // Fall back to GC (normal path)
        auto p = GC_malloc(n);
        if(IMMER_UNLIKELY(!p))
        {
            IMMER_THROW(std::bad_alloc{});
        }
        return p;
    }

    /*!
     * Allocate memory that contains NO references (atomic/leaf data).
     * GC won't scan this memory for pointers.
     */
    [[gnu::hot]]
    static void* allocate(std::size_t n, immer::norefs_tag)
    {
        if(current_allocator) [[unlikely]]
        {
            // Arena doesn't distinguish - it's all unscanned anyway
            return current_allocator->alloc(n, 16);
        }

        auto p = GC_malloc_atomic(n);
        if(IMMER_UNLIKELY(!p))
        {
            IMMER_THROW(std::bad_alloc{});
        }
        return p;
    }

    /*!
     * Deallocate memory.
     * For GC memory: calls GC_free (optional, GC will collect anyway)
     * For arena memory: no-op (arena frees all on reset/destruction)
     */
    static void deallocate(std::size_t size, void* data)
    {
        if(current_allocator) [[unlikely]]
        {
            // Arena allocators typically don't support individual frees
            // The allocator's free() method can decide what to do
            current_allocator->free(data, size, 16);
            return;
        }
        GC_free(data);
    }

    static void deallocate(std::size_t size, void* data, immer::norefs_tag)
    {
        deallocate(size, data);
    }
};

} // namespace jank::runtime
```

## Updated memory_policy in type.hpp

```cpp
// In include/cpp/jank/type.hpp

#include <jank/runtime/core/jank_heap.hpp>

namespace jank
{
  // Use jank_heap which respects current_allocator
  using memory_policy = immer::memory_policy<
      immer::heap_policy<runtime::jank_heap>,  // Changed from gc_heap
      immer::no_refcount_policy,
      immer::default_lock_policy,
      immer::gc_transience_policy,
      false>;
}
```

## Behavior Matrix

| Scenario | current_allocator | Vector allocation | Behavior |
|----------|------------------|-------------------|----------|
| Normal code | nullptr | `jank_heap::allocate()` | → GC_malloc (GC manages) |
| Inside with-arena | arena* | `jank_heap::allocate()` | → arena->alloc() (user manages) |
| Custom allocator | debug_alloc* | `jank_heap::allocate()` | → debug_alloc->alloc() |

## Critical Considerations

### 1. Memory Lifetime & GC Interaction

**Problem**: If arena-allocated immer nodes contain pointers to GC-allocated objects, and the arena is reset while those objects are still referenced elsewhere, we get dangling pointers.

**Solutions**:
- Document that arena-allocated collections should only contain arena-allocated data
- Or: Use `GC_malloc_uncollectable` for arena memory so GC won't collect referenced objects
- Or: Register arena memory with GC as roots (complex)

**Recommendation**: For v1, document the constraint. Arenas are for short-lived, self-contained computations.

### 2. Destructor Semantics

With `gc_heap`, destructors are never called (GC just reclaims memory). With `jank_heap`:
- GC path: Same as before (no destructors)
- Arena path: Same behavior (arena resets memory, no destructors)

This is consistent - immer with GC already doesn't call destructors.

### 3. Thread Safety

`current_allocator` is `thread_local`, so:
- Each thread has its own allocator context
- No synchronization needed in `jank_heap`
- Arenas are thread-confined (each thread uses its own arena)

### 4. Performance

The check `if(current_allocator)` is:
- Marked `[[unlikely]]` - branch prediction optimizes for the common case
- Just a pointer null check - extremely cheap
- Only overhead when NOT using arena (normal path)

When using arena, the allocation is actually FASTER (bump pointer vs GC).

### 5. Transience Policy Compatibility

`gc_transience_policy` should still work because:
- It uses `GC_malloc_atomic` for edit tokens
- We can intercept this too if needed (via norefs_tag overload)

## Implementation Steps

1. **Create `jank_heap.hpp`** with the implementation above
2. **Update `type.hpp`** to use `jank_heap`
3. **Test with existing tests** - should pass (nullptr allocator = GC)
4. **Add arena-aware tests** - verify vectors/maps use arena when set
5. **Document the arena constraints** for users

## Testing Strategy

```clojure
;; Test: verify immer collections use arena when set
(let [arena (jank.arena-native/create)]
  (jank.arena-native/enter! arena)
  (let [v (conj [] 1 2 3)  ; Should allocate from arena
        stats-after (jank.arena-native/stats arena)]
    (jank.arena-native/exit!)
    ;; Verify arena was used (total-used should be > 0)
    (assert (> (:total-used stats-after) 0)
            "Vector should have been allocated from arena")))
```

## Alternative Approaches Considered

### 1. Modify immer directly
- Rejected: Don't want to maintain a fork

### 2. Template all collection types with allocator parameter
- Rejected: Would require changing every vector/map usage

### 3. Per-collection allocator (like std::allocator)
- Rejected: immer design requires global/static heap state

### 4. Using immer's free_list_heap_policy with custom base
- Possible but complex; simple heap_policy is sufficient for v1

## References

- [Immer Memory Management Documentation](https://sinusoid.es/immer/memory.html)
- [Immer GitHub Repository](https://github.com/arximboldi/immer)
- `immer/heap/gc_heap.hpp` - Reference implementation
- `immer/heap/heap_policy.hpp` - Policy wrapper
- `immer/memory_policy.hpp` - Full policy template

## Implementation Results (COMPLETED)

The implementation has been completed and tested successfully!

### Files Created/Modified

1. **`include/cpp/jank/runtime/core/allocator_fwd.hpp`** - New minimal header with allocator interface (breaks circular dependency)
2. **`include/cpp/jank/runtime/core/jank_heap.hpp`** - New custom immer heap implementation
3. **`include/cpp/jank/runtime/core/arena.hpp`** - Updated to include allocator_fwd.hpp
4. **`include/cpp/jank/type.hpp`** - Changed `gc_heap` to `jank_heap` in memory_policy
5. **`test/jank/allocator/pass-cpp-counting-allocator.jank`** - Updated test proving data structures use allocator

### Test Results

```
=== Test 2: Data structures use allocator ===
After creating vector [1 2 3 4 5]:
  New allocations: 30

After creating vector [:a :b :c]:
  New allocations: 26

After creating map {:x 1 :y 2 :z 3}:
  New allocations: 17

After creating set #{:a :b :c}:
  New allocations: 16

Total new allocations for data structures: 89

=== All tests PASSED! ===
```

### Key Achievements

- **Vectors allocate through custom allocator**: ~30 allocations per vector
- **Maps allocate through custom allocator**: ~17 allocations per map
- **Sets allocate through custom allocator**: ~16 allocations per set
- **Backward compatible**: When no allocator is set, uses GC as before

## Critical Fix: GC Roots Registration

### The Problem

During testing, we discovered a crash when tests ran after allocator tests:

```
Assertion failed! o->type == T::obj_type
```

The issue: immer's structural sharing can mix nodes from different allocation sources (GC and arena). When arena memory was freed, GC might still have references to arena-allocated objects, or vice versa.

### The Solution

Register arena memory with GC using `GC_add_roots()` / `GC_remove_roots()`:

```cpp
// In arena::chunk constructor
GC_add_roots(start, end);

// In arena::chunk destructor
GC_remove_roots(start, end);
```

This tells GC that arena memory may contain pointers to GC-managed objects. GC won't collect those objects prematurely.

### Files Modified

- **`src/cpp/jank/runtime/core/arena.cpp`** - Added GC_add_roots/GC_remove_roots calls for chunk and large allocations

### For Custom Allocators

If you implement a custom allocator that creates data structures while active, you MUST:
1. Register allocated memory with `GC_add_roots(ptr, ptr + size)`
2. Unregister with `GC_remove_roots(ptr, ptr + size)` before freeing

Otherwise, immer nodes in your allocator's memory may reference GC objects that get prematurely collected.

## Conclusion

Immer's design with static heap methods and policy-based customization **does allow** custom allocators. The key insight is that while immer says "heaps can only have global state," thread-local variables ARE accessible from static methods. By creating `jank_heap` that checks `current_allocator`, we can make all immer-backed collections (vectors, maps, sets) respect jank's arena/allocator system seamlessly.

The implementation is:
- **Non-invasive**: No changes to immer source
- **Backward compatible**: When no allocator is set, behaves exactly like before
- **Performant**: Only adds a null pointer check on the normal path
- **Flexible**: Works with any allocator implementing jank's `allocator` interface
- **GC-safe**: Arena memory is registered with GC to prevent use-after-free
- **PROVEN**: Tests confirm vectors, maps, and sets use the custom allocator!
