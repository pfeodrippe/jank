# Zig-Style Debug Allocator for jank

## Why This Matters for jank

While jank uses Boehm GC (which handles most memory management automatically), there are important use cases for a debug allocator:

1. **Native Interop Debugging**: When calling C/C++ libraries, memory bugs can occur at the boundary
2. **Arena Allocator Validation**: Verify arena-allocated objects aren't escaped
3. **Performance Profiling**: Track allocation patterns without full profiler overhead
4. **FFI Development**: Help users developing native extensions find memory issues
5. **Teaching Tool**: Help users understand allocation patterns in their code

## Research Summary: Zig's GeneralPurposeAllocator (GPA)

### Sources
- [Zig GPA Source Code](http://ratfactor.com/zig/stdlib-browseable/heap/general_purpose_allocator.zig.html)
- [Zig Allocators Guide](https://zig.guide/standard-library/allocators/)
- [Memory Safety Features in Zig](https://gencmurat.com/en/posts/memory-safety-features-in-zig/)
- [Learning Zig - Heap Memory](https://www.openmymind.net/learning_zig/heap_memory/)
- [GPA Original Repository](https://github.com/andrewrk/zig-general-purpose-allocator)

### Zig's Allocator Philosophy

Zig's key insight: **"No hidden memory allocations"** - every allocation is explicit and visible. This contrasts with C++ where operators can allocate behind the scenes.

The allocator design philosophy emphasizes:
- **Explicit parameter passing**: APIs accept `std.mem.Allocator` and return ownership to caller
- **Composable allocators**: Allocators can wrap others (e.g., Arena wrapping Debug)
- **Pay only for what you use**: Each allocator serves a specific need

### Available Allocators in Zig (Comparison)

| Allocator | Safety | Speed | Use Case |
|-----------|--------|-------|----------|
| **DebugAllocator (GPA)** | High (leak/double-free detection) | Moderate | Development, testing |
| **ArenaAllocator** | High (scoped) | Fast (no individual frees) | Temporary allocations, parsers |
| **FixedBufferAllocator** | High (bounded) | Fastest (pre-allocated) | Embedded, kernel, known limits |
| **c_allocator** | Lower | Fast | Production (wraps malloc) |
| **page_allocator** | Basic | Slow (syscall per alloc) | Simplicity when perf doesn't matter |

### What GPA Does

Zig's GeneralPurposeAllocator (now called DebugAllocator in recent versions) is designed for **safety over performance**. It detects:

1. **Double-free**: Freeing the same memory twice
2. **Use-after-free**: Accessing memory after it's been freed
3. **Memory leaks**: Memory that was never freed
4. **Invalid cross-thread usage**: When thread-safety is disabled
5. **Buffer overruns**: Via guard patterns (optional)

### How GPA Works

#### Bucket-Based Architecture
- Divides small allocations into 12 size classes (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048 bytes)
- Each size class has a "current bucket" - a linked list of page-sized allocations
- Large allocations (>2048 bytes) use a backing allocator with metadata stored in a hash map

#### Safety Mechanisms

1. **Used Bits Tracking**: 1 bit per slot indicating if the slot is in use
   - Allocations iterate to find free slots
   - Frees assert the bit is 1, then set to 0 (detects double-free)

2. **Stack Trace Capture**: Records allocation and deallocation stack traces
   - 4-8 stack frames captured per allocation
   - Shows exact source of allocation, first free, and problematic second free

3. **Memory Protection**:
   - Freed pages are kept mapped but with no permissions (read/write/exec)
   - Accessing freed memory causes page faults (detects use-after-free)
   - Allocator state stored on protected pages (prevents rogue writes)

4. **Slot Non-Reuse**: In safe modes, freed slots are not reused immediately
   - Helps detect pointer math errors that would affect other allocations

#### Configuration Options
- `stack_trace_frames`: Control trace depth
- `enable_memory_limit`: Track and enforce memory caps
- `safety`: Toggle safety checks entirely
- `thread_safe`: Enable/disable mutex protection
- `never_unmap`: Convert segfaults to logged errors (for debugging)
- `retain_metadata`: Keep allocation metadata after free for better detection
- `verbose_log`: Log every allocation and free

---

## jank's Current Allocator Landscape

After the recent work, jank now has:

| Allocator | Status | Use Case |
|-----------|--------|----------|
| **Boehm GC** | Default | Normal allocations, automatic memory management |
| **Integer Cache** | Active | Cached small integers (-128 to 127) |
| **Real Cache** | Active | Cached common real numbers |
| **Arena Allocator** | Active | Scoped, bulk-freeable allocations |
| **Debug Allocator** | Planned | Memory bug detection (this document) |

### What Debug Allocator Adds

The debug allocator complements the existing allocators:

1. **Not a replacement for GC**: Works alongside Boehm GC
2. **Optional and scoped**: Only active when explicitly enabled
3. **Development-focused**: Primarily for debugging, not production
4. **Pattern detection**: Catches bugs that GC can't (like arena escape)

---

## jank Implementation Plan: Debug Allocator

### Goals

Provide an optional debug allocator that users can enable for:
1. **Development/debugging**: Find memory bugs in their code
2. **Testing**: Verify no memory issues in test suites
3. **Arena validation**: Detect arena-allocated objects that escape their scope
4. **FFI debugging**: Find memory issues in native interop code

### Phase 1: Core Debug Allocator (Minimal Viable Implementation)

#### 1.1 Create `debug_allocator` class

```cpp
// include/cpp/jank/runtime/core/debug_allocator.hpp
namespace jank::runtime
{
  struct allocation_info
  {
    void *ptr{};
    usize size{};
    native_vector<void*> alloc_stack_trace;
    native_vector<void*> free_stack_trace;
    bool is_freed{false};
  };

  struct debug_allocator
  {
    /* Thread-local current allocator (nullptr = use normal allocation) */
    static thread_local debug_allocator *current;

    /* Allocate memory with tracking */
    void *alloc(usize size, usize alignment = 16);

    /* Free memory with double-free detection */
    void free(void *ptr);

    /* Check for leaks and report them */
    bool detect_leaks() const;

    /* Reset allocator state */
    void reset();

    /* Get stats */
    struct stats_t
    {
      usize total_allocated{};
      usize total_freed{};
      usize current_live{};
      usize allocation_count{};
      usize free_count{};
    };
    stats_t stats() const;

  private:
    /* Map from pointer to allocation info */
    std::unordered_map<void*, allocation_info> allocations;
    mutable std::mutex mutex; // Optional thread safety
    stats_t stats_;
  };
}
```

#### 1.2 Integrate with `make_box`

Similar to arena integration, add debug allocator check:

```cpp
template <typename T, typename... Args>
jtl::ref<T> make_box(Args &&...args)
{
  T *ret{};

  if constexpr(requires { T::pointer_free; })
  {
    if constexpr(T::pointer_free)
    {
      // pointer_free types use caching, skip debug allocator
      ret = new(PointerFreeGC) T{ std::forward<Args>(args)... };
    }
    else
    {
      // Check debug allocator first (in debug builds)
      #if JANK_DEBUG_ALLOCATOR
      if(auto *debug_mem = try_debug_alloc(sizeof(T), alignof(T)))
      {
        ret = new(debug_mem) T{ std::forward<Args>(args)... };
      }
      else
      #endif
      // Then arena
      if(auto *arena_mem = try_arena_alloc(sizeof(T), alignof(T)))
      {
        ret = new(arena_mem) T{ std::forward<Args>(args)... };
      }
      else
      {
        ret = new(GC) T{ std::forward<Args>(args)... };
      }
    }
  }
  // ... rest unchanged
}
```

#### 1.3 Stack Trace Capture

```cpp
// Use libbacktrace or boost::stacktrace
native_vector<void*> capture_stack_trace(usize max_frames = 8)
{
  native_vector<void*> frames;
  frames.resize(max_frames);

  // Platform-specific capture
  #if defined(__APPLE__) || defined(__linux__)
  int count = backtrace(frames.data(), max_frames);
  frames.resize(count);
  #endif

  return frames;
}

// Pretty-print stack trace
void print_stack_trace(native_vector<void*> const& frames)
{
  char **symbols = backtrace_symbols(frames.data(), frames.size());
  for(usize i = 0; i < frames.size(); ++i)
  {
    util::println("  [{}] {}", i, symbols[i]);
  }
  std::free(symbols);
}
```

### Phase 2: jank.debug-allocator-native Module

Expose to jank code:

```clojure
; Create a debug allocator
(def dbg (jank.debug-allocator-native/create))

; Enter debug allocator scope
(jank.debug-allocator-native/enter! dbg)

; ... allocations happen here ...

; Exit debug allocator scope
(jank.debug-allocator-native/exit!)

; Check for leaks (returns vector of leak info)
(def leaks (jank.debug-allocator-native/detect-leaks dbg))

; Check if there are any leaks
(jank.debug-allocator-native/has-leaks? dbg)

; Get allocation stats
(jank.debug-allocator-native/stats dbg)
; => {:total-allocated 12345
;     :total-freed 10000
;     :current-live 2345
;     :allocation-count 100
;     :free-count 80}

; Reset (clear all tracking)
(jank.debug-allocator-native/reset! dbg)
```

### Phase 3: Enhanced Detection

#### 3.1 Use-After-Free Detection

Options for implementation:

1. **Poison bytes**: Fill freed memory with a known pattern (0xDEADBEEF)
   - On access, if pattern is detected, we know it was freed
   - Cheaper than page protection but less reliable

2. **Guard pages**: Like Zig, keep pages mapped with no permissions
   - More reliable but higher overhead
   - Requires larger minimum allocation granularity

3. **Delayed free list**: Don't actually free immediately
   - Keep track of "freed" allocations
   - Check on subsequent allocations if any freed memory is accessed

For jank, **poison bytes** is likely the best first approach due to GC integration.

#### 3.2 Boundary Check Detection

```cpp
struct guarded_allocation
{
  static constexpr u32 guard_pattern = 0xDEADBEEF;
  static constexpr usize guard_size = 8;

  u32 front_guard[2]; // 8 bytes before
  // ... actual data ...
  u32 back_guard[2];  // 8 bytes after
};

bool check_guards(void *ptr)
{
  auto *alloc = get_allocation_info(ptr);
  // Check front and back guards for corruption
}
```

### Phase 4: Configuration

```cpp
struct debug_allocator_config
{
  /* Number of stack frames to capture */
  usize stack_trace_frames{8};

  /* Enable thread safety (mutex) */
  bool thread_safe{true};

  /* Fill freed memory with poison pattern */
  bool poison_freed_memory{true};

  /* Keep metadata after free for double-free detection */
  bool retain_freed_metadata{true};

  /* Log every allocation/free */
  bool verbose_log{false};

  /* Max allocations to track (0 = unlimited) */
  usize max_tracked_allocations{0};
};

debug_allocator create_debug_allocator(debug_allocator_config config = {});
```

### Phase 5: Integration with GC

The tricky part: jank uses Boehm GC, which has its own allocation/deallocation semantics.

Options:

1. **Debug allocator as wrapper**: Track GC allocations but let GC manage actual memory
   - Track when `make_box` is called
   - Use GC finalizers to detect when objects are collected
   - Can't detect use-after-free reliably (GC might not have collected yet)

2. **Separate memory pool**: Debug allocator uses its own memory (like arena)
   - Full control over memory lifecycle
   - Objects won't be GC'd - must be manually freed or scope-exited
   - Good for debugging specific sections of code

3. **Hybrid**: Debug mode that wraps GC with extra tracking
   - Register finalizers on all allocations to track collection
   - Use weak references to detect if objects are still alive

**Recommended**: Option 2 (separate pool) for Phase 1, Option 3 for later phases.

---

## Implementation Order

1. **Phase 1.1**: Basic `debug_allocator` class with alloc/free/leak detection
2. **Phase 1.2**: Integration with `make_box` (compile-time flag)
3. **Phase 1.3**: Stack trace capture and printing
4. **Phase 2**: jank.debug-allocator-native module
5. **Phase 3.1**: Poison bytes for use-after-free hints
6. **Phase 3.2**: Guard bytes for buffer overrun detection
7. **Phase 4**: Configuration options
8. **Phase 5**: Better GC integration

## Files to Create/Modify

### New Files
- `include/cpp/jank/runtime/core/debug_allocator.hpp`
- `src/cpp/jank/runtime/core/debug_allocator.cpp`
- `include/cpp/jank/debug_allocator_native.hpp`
- `src/cpp/jank/debug_allocator_native.cpp`
- `test/jank/allocator/pass-debug-allocator.jank`
- `test/cpp/jank/runtime/debug_allocator.cpp`

### Modify
- `include/cpp/jank/runtime/oref.hpp` - Add debug allocator check to make_box
- `CMakeLists.txt` - Add new source files
- `src/cpp/jank/c_api.cpp` - Add `jank_load_jank_debug_allocator_native()`

## Estimated Complexity

- Phase 1: Medium (core allocator + make_box integration)
- Phase 2: Low (straightforward native module)
- Phase 3: Medium (memory protection tricks)
- Phase 4: Low (configuration struct)
- Phase 5: High (GC integration is tricky)

## Testing Strategy

1. **Unit tests**: Test allocator directly in C++
   - Verify double-free detection
   - Verify leak detection
   - Verify stats accuracy

2. **Integration tests**: Test via jank code
   - Verify native module works
   - Verify enter/exit scoping

3. **Intentional bug tests**: Create code with known bugs
   - Double-free test (should report error)
   - Leak test (should detect unreleased allocations)

---

## Usage Patterns from Zig (Applied to jank)

### Pattern 1: Debug in Development, Production Allocator in Release

From Zig best practices:
```clojure
; In development/test
(jank.debug-allocator-native/with-debug-alloc
  (my-allocating-code))

; In production - just use normal GC (no overhead)
(my-allocating-code)
```

### Pattern 2: Composable Allocators

Like Zig's ArenaAllocator wrapping DebugAllocator:
```clojure
; Create debug allocator for tracking
(def dbg (jank.debug-allocator-native/create))

; Create arena that uses debug allocator as backing
(def arena (jank.arena-native/create-with-backing dbg))

; All arena allocations are now tracked for debugging
(jank.arena-native/enter-arena! arena)
; ... allocations ...
(jank.arena-native/exit-arena!)

; Check what the arena allocated
(jank.debug-allocator-native/stats dbg)
```

### Pattern 3: Test Suite Integration

From Zig's `std.testing.allocator` pattern:
```clojure
; In test setup
(def test-alloc (jank.debug-allocator-native/create))
(jank.debug-allocator-native/enter! test-alloc)

; Run tests...

; In test teardown - fail if leaks detected
(when (jank.debug-allocator-native/has-leaks? test-alloc)
  (throw (ex-info "Memory leak detected"
                  {:leaks (jank.debug-allocator-native/detect-leaks test-alloc)})))
```

### Pattern 4: Scoped Debugging (Narrowing Down Issues)

```clojure
; Suspect this function has a memory issue
(jank.debug-allocator-native/with-tracking
  {:verbose true}
  (suspicious-function arg1 arg2))
; Prints all allocations/frees that happened
```

---

## Key Insights from Zig Research

1. **Safety is configurable**: Turn off features you don't need for better performance
2. **Bucket architecture is efficient**: Size classes prevent fragmentation
3. **Stack traces are essential**: Without them, finding the bug source is nearly impossible
4. **Metadata retention matters**: Keep freed allocation info to detect double-free
5. **Page protection is powerful but expensive**: Use poison bytes as cheaper alternative
6. **Thread-safety is optional**: Single-threaded code shouldn't pay the mutex cost

---

## References

- [Zig GPA Source Code](http://ratfactor.com/zig/stdlib-browseable/heap/general_purpose_allocator.zig.html)
- [Zig Allocators Guide](https://zig.guide/standard-library/allocators/)
- [Memory Safety Features in Zig](https://gencmurat.com/en/posts/memory-safety-features-in-zig/)
- [GPA is Dead, Long Live Debug Allocator](https://ziggit.dev/t/gpa-is-dead-long-live-the-debug-allocator/8449)
- [Learning Zig - Heap Memory](https://www.openmymind.net/learning_zig/heap_memory/)
- [GPA Original Repository README](https://github.com/andrewrk/zig-general-purpose-allocator/blob/master/README.md)
- [Introduction to Zig - Memory Chapter](https://pedropark99.github.io/zig-book/Chapters/01-memory.html)
- [Manual Memory Management in Zig](https://dev.to/hexshift/manual-memory-management-in-zig-allocators-demystified-46ne)
