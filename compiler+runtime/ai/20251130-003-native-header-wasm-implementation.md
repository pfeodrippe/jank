# Native Header Requires in WASM - Implementation Complete

Date: 2025-11-30

## Summary

Successfully implemented support for native header requires like `(:require ["flecs.h" :as flecs])` in WASM AOT compilation. Previously this threw "Native C++ headers are not supported in WASM".

## Problem Solved

When using native header requires in jank code compiled for WASM, the `register-native-header!` function would throw an error. This was unnecessarily restrictive because:
1. The `#include` is already processed during native jank AOT compilation
2. The C++ symbols are already in the generated code
3. Only the alias metadata registration is needed at runtime

## Solution

### Key Insight

The original `register_native_header` function did two things:
1. Register alias metadata in the namespace (needed in WASM)
2. JIT compile `#include <header>` (NOT needed in WASM)

For WASM, we only need step 1.

### Files Modified

1. **`include/cpp/clojure/core_native.hpp`** - Added function declarations:
   ```cpp
   object_ref register_native_header_wasm(...);
   object_ref register_native_refer_wasm(...);
   ```

2. **`src/cpp/clojure/core_native.cpp`** - Added WASM-compatible functions:
   - `register_native_header_wasm` - In native jank: calls regular function; In WASM: only registers metadata
   - `register_native_refer_wasm` - Same pattern
   - Functions are defined OUTSIDE `#ifdef JANK_TARGET_WASM` so they exist for native jank JIT
   - Internal `#ifndef JANK_TARGET_WASM` for platform-specific behavior

3. **`src/jank/clojure/core.jank`** - Updated `register-native-header!`:
   ```clojure
   #?(:wasm (do
              (cpp/clojure.core_native.register_native_header_wasm ...)
              (doseq [{:keys [local member]} refers]
                (cpp/clojure.core_native.register_native_refer_wasm ...)))
      :jank (do
              (cpp/clojure.core_native.register_native_header ...)
              ...))
   ```

4. **`bin/emscripten-bundle`** - Fixed bash array iteration bug

## Key Technical Details

### Why Both Native and WASM Need These Functions

When native jank does AOT compilation for WASM target with `:wasm` reader features:
1. It reads Clojure code with `:wasm` feature enabled
2. It generates C++ code with `cpp/clojure.core_native.register_native_header_wasm` calls
3. These calls are JIT compiled by native jank during AOT process
4. So the `_wasm` functions must exist in native jank too!

The functions use internal `#ifndef JANK_TARGET_WASM` to:
- In native jank: delegate to regular functions (which do JIT include)
- In WASM: only register metadata (no JIT available)

### The Header File is Critical

`cpp/` calls in jank generate direct C++ function calls. The JIT (Cling) needs to see the function declarations via headers. Without adding the declarations to `core_native.hpp`, the JIT couldn't find the functions.

## Testing

Test file: `build-wasm/my_flecs_static_and_wasm.jank`
```clojure
(ns my-flecs-static-and-wasm
  (:require
   ["flecs.h" :as flecs]))
```

Command:
```bash
./bin/emscripten-bundle --skip-build --run \
  -I /path/to/flecs/distr \
  --lib /path/to/flecs_wasm.o \
  build-wasm/my_flecs_static_and_wasm.jank
```

Result: Successfully compiles and runs in WASM, no more "Native C++ headers are not supported in WASM" error!

## Related Changes

Also added red error output to `emscripten-bundle` for better error visibility.
