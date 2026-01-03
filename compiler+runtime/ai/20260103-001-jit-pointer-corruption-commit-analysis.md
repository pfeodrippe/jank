# JIT Pointer Corruption Bug - Commit Analysis & Fix Plan

## Executive Summary

Investigation of commit **c7c74a0eb3c1d060578fb5acb8ce2a0575b7dc66** (merge of origin/main into nrepl-4) revealed **4 critical issues** that combined to cause string pointer corruption in JIT mode.

**Symptom**: String values change unexpectedly between frames:
```
[UI 7a] got greeting= Hello from vybe.sdf.greeting!!
... (one frame later) ...
[UI 7a] got greeting= vulkan_kim/hand_cigarette.comp
```

---

## Critical Issues Found

### Issue #1: `oref` Default Member Initializer Calls Uninitialized Function (CRITICAL)

**File**: `include/cpp/jank/runtime/oref.hpp`

**Before (nrepl-4 branch)**:
```cpp
namespace detail {
  extern obj::nil *jank_nil_ptr;  // Static pointer initialized at startup
}

constexpr oref()
  : data{ std::bit_cast<object *>(detail::jank_nil_ptr) }
{
}
```

**After (origin/main merged)**:
```cpp
extern "C" void *jank_const_nil();  // Function declaration

oref() = default;

// DEFAULT MEMBER INITIALIZER - CALLS FUNCTION AT CLASS INIT TIME
value_type *data{ std::bit_cast<object *>(jank_const_nil()) };
```

**Problem**:
- Default member initializers execute during **static initialization**
- `jank_const_nil()` calls `jank_nil()` which may not be initialized yet
- Results in **garbage pointer** in oref's data member
- Every oref created before runtime initialization has corrupted data

**Impact**: HIGH - Affects all object references created during static initialization

---

### Issue #2: `var` No Longer Inherits from `gc` (CRITICAL)

**File**: `include/cpp/jank/runtime/var.hpp`

**Before**:
```cpp
struct var : gc  // GC tracks this object
{
  folly::Synchronized<object_ref> root;
};
```

**After**:
```cpp
struct var  // NO GC INHERITANCE - object may be collected!
{
  folly::Synchronized<object_ref> root;
};
```

**Problem**:
- Var objects are no longer tracked by the garbage collector
- When vars are collected, thread bindings still hold references
- Deref operations access freed memory (use-after-free)
- String values in var's `root` become corrupted

**Impact**: HIGH - Causes use-after-free when vars are garbage collected

---

### Issue #3: Namespace Prefix Changes in Code Generation (MEDIUM)

**File**: `src/cpp/jank/codegen/processor.cpp`

**Before**:
```cpp
// Fully qualified - always resolves correctly
return "::jank::runtime::obj::persistent_string_ref";
util::format_to(buffer, "::jank::runtime::jank_nil");
```

**After**:
```cpp
// Relative - may resolve incorrectly in certain contexts
return "jank::runtime::obj::persistent_string_ref";
util::format_to(buffer, "jank::runtime::jank_nil()");
```

**Problem**:
- JIT-compiled code may resolve symbols to wrong namespaces
- Different behavior in eval context vs module context
- String constants may bind to wrong objects

**Impact**: MEDIUM - Can cause symbol resolution issues in JIT code

---

### Issue #4: Empty String Singleton Optimization (LOW-MEDIUM)

**File**: `src/cpp/jank/codegen/processor.cpp`

**Added optimization**:
```cpp
else if constexpr(std::same_as<T, runtime::obj::persistent_string>)
{
  if(typed_o->data.empty())
  {
    // NEW: Uses singleton instead of allocating
    util::format_to(buffer, "jank::runtime::obj::persistent_string::empty()");
  }
  else
  {
    util::format_to(buffer,
                    "jank::runtime::make_box<jank::runtime::obj::persistent_string>({})",
                    typed_o->to_code_string());
  }
}
```

**Problem**:
- Singleton empty strings are shared across compilation contexts
- In JIT mode, multiple evaluations share the same empty string object
- Can cause aliasing issues if empty strings are expected to be independent

**Impact**: LOW-MEDIUM - May cause unexpected sharing behavior

---

### Issue #5: Infinite Recursion in `oref<nil>::erase()` (FIXED in b66b7467b)

**File**: `include/cpp/jank/runtime/oref.hpp`

**Problematic code** (introduced, then fixed):
```cpp
oref<object> erase() const noexcept {
  return { std::bit_cast<object *>(jank_const_nil()) };  // INFINITE RECURSION
}
```

`jank_const_nil()` calls `jank_nil().data` which could call `erase()` again.

**Status**: Fixed in commit b66b7467b with:
```cpp
return { reinterpret_cast<object *>(data) };
```

---

## Root Cause Analysis

The pointer corruption happens through this sequence:

1. **Static Initialization Phase**:
   - oref objects are created with corrupted `data` pointers (Issue #1)
   - `jank_const_nil()` returns garbage before runtime is initialized

2. **JIT Compilation**:
   - Generated code uses relative namespace prefixes (Issue #3)
   - String constants may resolve to wrong objects
   - Empty strings use singleton (Issue #4)

3. **Runtime Execution**:
   - Var objects are created but not tracked by GC (Issue #2)
   - GC collects var objects while thread bindings still reference them
   - `deref()` accesses freed memory
   - String pointer now points to whatever was allocated in that memory

4. **Memory Reuse**:
   - GC allocates new string ("vulkan_kim/hand_cigarette.comp") in freed memory
   - Old pointer now points to new string
   - UI displays corrupted value

---

## Recommended Fixes

### Fix #1: Restore Static `jank_nil_ptr` for oref Initialization (PRIORITY: CRITICAL)

**Option A**: Restore the old initialization pattern
```cpp
// oref.hpp
namespace detail {
  extern obj::nil *jank_nil_ptr;  // Initialized at startup in nil.cpp
}

// oref<object> specialization
value_type *data{ std::bit_cast<object *>(detail::jank_nil_ptr) };

// nil.cpp
namespace detail {
  obj::nil jank_nil_obj{};
  obj::nil *jank_nil_ptr{ &jank_nil_obj };
}
```

**Option B**: Use two-phase initialization
```cpp
// oref.hpp
value_type *data{ nullptr };  // Start null

// Ensure all orefs are initialized before use
static void ensure_nil_initialized() {
  static obj::nil nil_instance{};
  // Initialize all static orefs here
}
```

### Fix #2: Restore GC Inheritance for `var` (PRIORITY: CRITICAL)

```cpp
// var.hpp
struct var : gc  // MUST inherit from gc for proper tracking
{
  // ...existing members...
};

struct var_thread_binding : gc  // Same for related types
{
  // ...
};

struct var_unbound_root : gc
{
  // ...
};
```

**Alternative**: If GC inheritance is not desired, register vars manually:
```cpp
var::var() {
  GC_add_roots(this, this + 1);  // Register with GC
}

var::~var() {
  GC_remove_roots(this, this + 1);  // Unregister
}
```

### Fix #3: Restore Fully Qualified Namespace Prefixes (PRIORITY: MEDIUM)

In `src/cpp/jank/codegen/processor.cpp`, change all namespace references back:

```cpp
// Change this:
return "jank::runtime::obj::persistent_string_ref";

// To this:
return "::jank::runtime::obj::persistent_string_ref";
```

Global search and replace:
- `"jank::runtime::` → `"::jank::runtime::`

### Fix #4: Add GC Root Registration for JIT Constants (PRIORITY: MEDIUM)

When JIT-compiling string constants, ensure they're registered as GC roots:

```cpp
// In jit/processor.cpp or codegen
void register_jit_constant(object_ref const o) {
  // Add to a set of GC roots that persists for the JIT module lifetime
  jit_gc_roots.push_back(o);
}
```

---

## Testing Plan

### Test 1: Verify Static Initialization Order
```cpp
// Add debug logging in jank_const_nil()
void* jank_const_nil() {
  static bool initialized = false;
  if (!initialized) {
    printf("WARNING: jank_const_nil called before initialization!\n");
  }
  return jank_nil().data;
}
```

### Test 2: Verify GC Tracking of Vars
```cpp
// Add to var constructor
var::var() {
  printf("var created at %p\n", this);
  // Verify GC is tracking this
}
```

### Test 3: Stress Test String Lifetime
```clojure
;; In REPL, repeatedly evaluate:
(dotimes [_ 1000]
  (let [s (str "test-" (rand-int 10000))]
    (System/gc)  ;; Force GC
    (println s)))
```

### Test 4: Frame-to-Frame Value Stability
```clojure
;; Create var, access it across multiple frames
(def test-greeting "Hello from test!")

;; In UI loop:
(dotimes [frame 100]
  (assert (= @test-greeting "Hello from test!")
          (str "Corruption at frame " frame ": " @test-greeting)))
```

---

## Files to Modify

| File | Change | Priority |
|------|--------|----------|
| `include/cpp/jank/runtime/oref.hpp` | Restore static nil pointer | CRITICAL |
| `include/cpp/jank/runtime/var.hpp` | Restore GC inheritance | CRITICAL |
| `src/cpp/jank/runtime/obj/nil.cpp` | Add static nil definition | CRITICAL |
| `src/cpp/jank/codegen/processor.cpp` | Fix namespace prefixes | MEDIUM |
| `src/cpp/jank/c_api.cpp` | Review jank_const_nil | LOW |

---

## Commits to Review

| Commit | Description | Impact |
|--------|-------------|--------|
| c7c74a0eb | Merge origin/main into nrepl-4 | Introduced all issues |
| 768f8310c | C++ gen (WIP) | GC changes |
| b66b7467b | Fix ios code | Fixed erase() recursion |
| 9a30b6a69 | Fix ios issues | Added binding_scope.pushed flag |

---

## Conclusion

The JIT pointer corruption is caused by **multiple cascading issues** introduced in the merge:

1. **Primary cause**: `oref` default member initializer calling `jank_const_nil()` before initialization
2. **Secondary cause**: `var` objects no longer tracked by GC, leading to use-after-free
3. **Contributing factors**: Namespace resolution changes, singleton optimizations

**Recommended immediate action**:
1. Restore static `jank_nil_ptr` initialization pattern
2. Restore `gc` inheritance for `var` and related types
3. Fix namespace prefixes in codegen

These changes should restore the stable behavior from before the merge.
