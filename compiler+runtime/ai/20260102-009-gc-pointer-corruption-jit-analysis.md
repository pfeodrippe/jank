# GC Pointer Corruption in JIT Mode - Deep Analysis

## Problem Statement

In the iOS JIT app, a string pointer is changing its value between frames:
```
[UI 7a] got greeting= Hello from vybe.sdf.greeting!!
... (one frame later) ...
[UI 7a] got greeting= vulkan_kim/hand_cigarette.comp
```

The same pointer that previously held "Hello from vybe.sdf.greeting!!" now contains "vulkan_kim/hand_cigarette.comp" - a completely unrelated shader filename.

## Root Cause Analysis

### How jank Strings Work

1. **Small String Optimization (SSO)**: Strings <= 23 chars are stored inline in the `immutable_string` struct (3 words = 24 bytes on 64-bit)

2. **Large Strings**: Strings > 23 chars are allocated via `GC_malloc_atomic()`:
   ```cpp
   // From include/cpp/jtl/immutable_string.hpp:840
   store.large.data = static_cast<char *>(GC_malloc_atomic(size + 1));
   ```

3. **GC_malloc_atomic**: This tells Boehm GC that the memory contains NO pointers to other GC objects. The GC won't scan this memory for references, but it WILL collect it when no roots point to it.

### The Dangling Pointer Problem

When C++ code does this:
```cpp
// WRONG - creates dangling pointer!
auto greeting_obj = call_jank_function("vybe.sdf.greeting/get-greeting");
const char* greeting_ptr = to_string(greeting_obj).data();  // Raw pointer to GC memory
// ... later, after GC runs ...
printf("greeting= %s\n", greeting_ptr);  // BOOM - pointer now points to garbage
```

**What happens:**
1. jank function returns a `persistent_string_ref` (boxed string object)
2. C++ extracts `const char*` via `.data()` or `.c_str()`
3. C++ stores ONLY the raw pointer, not the `object_ref` or `persistent_string_ref`
4. The original string object has no more GC roots pointing to it
5. GC runs and collects the string's memory
6. Memory is reused for a new allocation (e.g., "vulkan_kim/hand_cigarette.comp")
7. Old pointer now points to new data = CORRUPTION

### Why This Is Worse in JIT Mode

In AOT mode, string literals are typically:
- Compiled into static data sections
- Live for the entire program lifetime
- Never collected by GC

In JIT mode:
- String constants may be created dynamically
- Each evaluation creates new GC-allocated objects
- More frequent allocations = more frequent GC = faster corruption

### Evidence from the Logs

The string "vulkan_kim/hand_cigarette.comp" is likely:
- A shader filename being loaded/processed
- Allocated in the same GC memory region
- Overwrote the old greeting string's memory after GC

## Solutions

### Solution 1: Keep object_ref Alive (Recommended)

Instead of storing raw `char*`, keep the `object_ref` or `persistent_string_ref` alive:

```cpp
// CORRECT - keeps GC root alive
class UI {
    jank::runtime::object_ref greeting_obj;  // GC root!

    void update() {
        greeting_obj = call_jank_function("vybe.sdf.greeting/get-greeting");
        // Now safe to use the string
        auto str = jank::runtime::to_string(greeting_obj);
        printf("greeting= %s\n", str.c_str());
    }
};
```

### Solution 2: Copy String Data

If you need a stable string, copy it:

```cpp
// CORRECT - owns the memory
std::string greeting_copy;

void update() {
    auto obj = call_jank_function("vybe.sdf.greeting/get-greeting");
    greeting_copy = std::string(jank::runtime::to_string(obj).data());
    // greeting_copy is now independent of GC
}
```

### Solution 3: Use Static/Global Storage for Frequently Accessed Vars

For vars that are accessed every frame, cache the var_ref:

```cpp
// Cache the var once
static jank::runtime::var_ref greeting_var;

void init() {
    greeting_var = jank::runtime::__rt_ctx->find_var("vybe.sdf.greeting", "greeting");
}

void update() {
    // Deref returns object_ref - must keep alive during use
    auto greeting_obj = greeting_var->deref();
    // Use immediately or copy
}
```

### Solution 4: Register Custom GC Roots (Advanced)

For C++ objects that hold jank references:

```cpp
class UI {
    jank::runtime::object_ref* gc_roots[10];

    UI() {
        // Register array as GC root
        GC_add_roots(gc_roots, gc_roots + 10);
    }

    ~UI() {
        GC_remove_roots(gc_roots, gc_roots + 10);
    }
};
```

## Debugging Steps

### 1. Add GC Debugging

```cpp
// Enable GC warnings
GC_set_warn_proc([](char* msg, GC_word arg) {
    printf("GC WARNING: %s\n", msg);
});

// Force GC to reproduce issue faster
GC_gcollect();
```

### 2. Track String Allocations

Add logging to `immutable_string::init_large_owned`:
```cpp
printf("ALLOC string @%p: %.40s...\n", store.large.data, store.large.data);
```

### 3. Verify Object Lifetime

```cpp
void update() {
    auto obj = get_greeting();
    printf("obj @%p, type=%d\n", obj.data, (int)obj->type);

    auto& str = expect_object<persistent_string>(obj)->data;
    printf("str.data @%p, category=%d\n", str.data(), (int)str.get_category());

    // Check if pointer survives GC
    GC_gcollect();
    printf("after GC: %s\n", str.data());  // Will show corruption if obj isn't rooted
}
```

## iOS-Specific Considerations

### Memory Pressure

iOS aggressively reclaims memory. Under memory pressure:
- GC may run more frequently
- Larger objects may be collected first
- Background/foreground transitions may trigger GC

### JIT Memory Limits

iOS has ~1850MB JIT memory limit. As memory fills:
- GC runs more often
- More allocations/deallocations
- Higher chance of pointer corruption bugs surfacing

## Prevention Checklist

- [ ] Never store raw `char*` from jank strings across function calls
- [ ] Always keep `object_ref` or `persistent_string_ref` alive while using the data
- [ ] Copy string data to `std::string` if you need stable storage
- [ ] For frequently accessed values, cache the `var_ref` not the value
- [ ] Use RAII to manage object_ref lifetimes
- [ ] Consider using arena allocator for frame-local allocations

## Related Files

- `include/cpp/jtl/immutable_string.hpp` - String implementation with GC allocation
- `src/cpp/jank/runtime/core/arena.cpp` - Arena allocator with GC root registration
- `include/cpp/jank/runtime/core/jank_heap.hpp` - Custom heap for immer with GC fallback

## Conclusion

The pointer corruption is caused by storing a raw `char*` pointer to GC-allocated string memory without keeping the owning `object_ref` alive. When GC collects the unreferenced string, the memory is reused for new allocations, causing the old pointer to now point to different data.

**Fix**: Keep the `object_ref` or `persistent_string_ref` alive for as long as you need to access the string data, or copy the string to owned memory (e.g., `std::string`).
