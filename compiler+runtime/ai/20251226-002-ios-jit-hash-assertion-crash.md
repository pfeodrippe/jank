# iOS JIT Hash Assertion Crash Investigation Plan

## Problem Summary

When iOS loads JIT-compiled modules from the remote compile server, the app crashes during namespace initialization with a hash assertion failure in `refer`.

## Crash Stack Trace

```
jtl::do_assertion_throw
std::__1::hash<jank::runtime::oref<jank::runtime::object>>::operator()
jank::runtime::obj::transient_hash_set::conj_in_place
jank::runtime::reduce
core::clojure_core_set_42775::call
core::clojure_core_refer_48233::call
[JIT CODE - ???]
jank::runtime::module::loader::load_jank
```

## Root Cause Analysis

The assertion `jank_debug_assert(o->type == T::obj_type)` in `expect_object` (rtti.hpp:58) is failing. This means:
1. `visit_object` switches on `o->type` and goes to a specific case
2. That case calls `expect_object<T>` which asserts `o->type == T::obj_type`
3. The assertion fails, meaning the type field changed or was invalid

## Reproduction Steps

1. Run `make sdf-ios-server` to start the compile server
2. Run `make ios-jit-only-sim-run` to start the iOS simulator
3. The crash occurs when loading `vybe.sdf.ios` or its dependencies
4. Specifically crashes during `refer` when creating a `set` from `:exclude` symbols

## Affected Code Paths

The crash happens in namespaces like `vybe.sdf.math` which have:
```clojure
(ns vybe.sdf.math
  (:refer-clojure :exclude [abs]))
```

The `ns` macro expands to call `refer` with the exclude list. The `refer` function calls `(set (:exclude filters))` which hashes each symbol.

## Hypotheses

### Hypothesis 1: Object Type Enum Mismatch (UNLIKELY)

The `object_type` enum values might differ between AOT (iOS) and JIT (desktop) compiled code.

**To test:** Add debug output comparing enum values at runtime.

**Why unlikely:** Both use the same headers from jank source tree.

### Hypothesis 2: Symbol Creation from JIT Code (LIKELY)

When JIT-compiled code creates symbols (e.g., the `abs` symbol in `'[abs]`), something goes wrong with the object's type field.

The codegen creates symbols using:
```cpp
::jank::runtime::make_box<::jank::runtime::obj::symbol>("{}", "{}")
```

**To test:**
1. Add debug logging in `make_box` to verify type field is set correctly
2. Log the actual type value when the assertion fails

### Hypothesis 3: Static/Global Reference Issue (POSSIBLE)

The JIT-compiled code might reference static data that has different addresses between desktop and iOS, leading to corrupted object references.

**To test:** Check if any static symbols/keywords are used in the generated code.

### Hypothesis 4: Memory Layout Difference (POSSIBLE)

The struct layouts might differ between AOT and JIT builds due to:
- Different compiler flags
- Different alignment settings
- Different include order

**To test:** Compare `sizeof` and `offsetof` values between builds.

### Hypothesis 5: Nil or Invalid Pointer (POSSIBLE)

The object being hashed might be nil or point to invalid memory. The `is_some()` check passed but the type field could be garbage.

**To test:** Add null checks and print pointer values before hashing.

## Investigation Steps

### Step 1: Add Debug Logging to Hash Function

Modify `compiler+runtime/src/cpp/jank/runtime/object.cpp`:
```cpp
size_t hash<object_ref>::operator()(object_ref const o) const noexcept
{
  if(!o.data) {
    std::cerr << "[HASH DEBUG] null object!\n";
    abort();
  }
  std::cerr << "[HASH DEBUG] type=" << static_cast<int>(o->type)
            << " ptr=" << o.data << "\n";
  return jank::hash::visit(o.data);
}
```

### Step 2: Add Debug Logging to expect_object

Modify `compiler+runtime/include/cpp/jank/runtime/rtti.hpp`:
```cpp
template <typename T>
constexpr oref<T> expect_object(object_ref const o)
{
  if constexpr(T::obj_type != object_type::nil)
  {
    if(!o.is_some()) {
      std::cerr << "[EXPECT_OBJECT] null ref for type "
                << object_type_str(T::obj_type) << "\n";
    }
    jank_debug_assert(o.is_some());
  }
  if(o->type != T::obj_type) {
    std::cerr << "[EXPECT_OBJECT] type mismatch: got "
              << static_cast<int>(o->type) << " (" << object_type_str(o->type) << ")"
              << " expected " << static_cast<int>(T::obj_type)
              << " (" << object_type_str(T::obj_type) << ")"
              << " ptr=" << o.data << "\n";
  }
  jank_debug_assert(o->type == T::obj_type);
  return reinterpret_cast<T *>(reinterpret_cast<char *>(o.data) - offsetof(T, base));
}
```

### Step 3: Check Generated Code for Symbol Creation

Look at the generated C++ for one of the crashing namespaces to see how symbols are created.

### Step 4: Compare Struct Layouts

Add code to print and compare:
```cpp
std::cerr << "sizeof(obj::symbol)=" << sizeof(obj::symbol) << "\n";
std::cerr << "offsetof(symbol,base)=" << offsetof(obj::symbol, base) << "\n";
std::cerr << "object_type::symbol=" << static_cast<int>(object_type::symbol) << "\n";
```

### Step 5: Trace the Object Creation

If the object has an invalid type, trace back where it was created:
1. Was it created by JIT code (`make_box`)?
2. Was it interned (`intern_symbol`)?
3. Was it passed from AOT code?

## Potential Fixes

### Fix A: Validate Objects at JIT/AOT Boundary

Add validation when objects cross from JIT code to AOT code (in `dynamic_call` or similar entry points).

### Fix B: Use Runtime Type Checks Instead of Assertions

Replace `jank_debug_assert` with runtime checks that provide better error messages:
```cpp
if(o->type != T::obj_type) {
  throw std::runtime_error(fmt::format(
    "Type mismatch: expected {} got {}",
    object_type_str(T::obj_type), object_type_str(o->type)));
}
```

### Fix C: Ensure Consistent Compilation Flags

Verify that both AOT (iOS) and JIT (desktop cross-compile) use identical:
- `-std=` flags
- Alignment pragmas
- Struct packing options

### Fix D: Debug Allocator for JIT Objects

Enable a debug allocator that validates object headers when they're created and accessed.

## Files to Investigate

1. `compiler+runtime/include/cpp/jank/runtime/object.hpp` - object_type enum
2. `compiler+runtime/include/cpp/jank/runtime/rtti.hpp` - expect_object
3. `compiler+runtime/src/cpp/jank/runtime/object.cpp` - hash operators
4. `compiler+runtime/src/cpp/jank/codegen/processor.cpp` - symbol generation
5. `compiler+runtime/include/cpp/jank/runtime/obj/symbol.hpp` - symbol struct

## Next Steps

1. Implement Step 1 & 2 (debug logging)
2. Rebuild jank and iOS app
3. Run test and capture debug output
4. Analyze which object has the wrong type and where it came from
5. Based on findings, implement appropriate fix

## Context: iOS JIT Architecture

iOS doesn't support traditional JIT due to security restrictions. The workaround:
1. Desktop compile server runs jank natively
2. iOS app connects to compile server over network
3. Desktop cross-compiles Clojure code to ARM64 .o files
4. iOS receives .o files and loads them via ORC JIT
5. JIT-compiled code runs alongside AOT-compiled clojure.core

The crash occurs at the boundary where JIT-compiled code interacts with AOT-compiled runtime functions.
