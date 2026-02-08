# Allocator Design - User-Controlled Memory Management

## Research Summary

Researched allocator patterns from three sources:

### Zig's Allocator Interface
- **Explicit allocator parameter**: Every function that needs to allocate takes an `std.mem.Allocator` parameter
- **Composable**: Allocators are values that can be wrapped/composed (e.g., ArenaAllocator wraps a child allocator)
- **Key types**:
  - `GeneralPurposeAllocator`: Safe with debugging features
  - `ArenaAllocator`: Bulk free, wraps another allocator
  - `FixedBufferAllocator`: Stack-allocated, zero syscalls
  - `c_allocator`: High performance, wraps malloc/free

### Odin's Context System
- **Implicit context pointer**: Passed on every procedure call (Odin calling convention)
- **Two allocators**: `context.allocator` (heap-like) and `context.temp_allocator` (arena-like)
- **Copy-on-write semantics**: Prevents back-propagation of corrupted data
- **Purpose**: Intercept third-party code and modify behavior (allocation, logging)

### Flecs Block Allocator
- **Thread-specific allocators**: 60x faster than OS allocations
- **Block/chunk allocation**: Reduces heap fragmentation
- **OS API abstraction**: Portable custom allocators

## jank's Approach

Our implementation combines ideas from all three:

1. **Thread-local allocator** (like Odin's context):
   - `current_allocator` - thread-local pointer to active allocator
   - `allocator_scope` - RAII scope for setting/restoring allocator

2. **Abstract interface** (like Zig's composable design):
   ```cpp
   struct allocator {
     virtual ~allocator() = default;
     virtual void *alloc(usize size, usize alignment) = 0;
     virtual void reset() { }
     virtual stats get_stats() const { return {}; }
   };
   ```

3. **Arena implementation** (like both Zig and Flecs):
   - Bump-pointer allocation for small objects
   - Direct malloc for large allocations
   - System malloc (bypasses GC completely)
   - ~9ns per allocation when warm

## Files Created/Modified

- `include/cpp/jank/runtime/core/arena.hpp` - Interface and arena implementation
- `src/cpp/jank/runtime/core/arena.cpp` - Arena implementation
- `test/cpp/jank/runtime/core/arena.cpp` - 9 tests, all passing

## Performance Results

Arena Benchmark (100K allocations of 64 bytes):
- Cold: 78ns/alloc (includes chunk allocation)
- Warm: 10ns/alloc (reusing memory)
- Speedup: 7.7x warm vs cold

## Usage Pattern (Future Clojure-level API)

```clojure
;; User provides custom allocator
(with-allocator my-custom-allocator
  (do-work))

;; Built-in arena
(with-arena
  (do-work))
```

## Sources
- [Zig Allocators Guide](https://zig.guide/standard-library/allocators/)
- [Odin's Context System](https://www.gingerbill.org/article/2025/12/15/odins-most-misunderstood-feature-context/)
- [Flecs Block Allocator](https://github.com/SanderMertens/flecs/blob/master/include/flecs/datastructures/block_allocator.h)
