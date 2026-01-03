# Plan: Native Header Requires in WASM

Date: 2025-11-30

## Problem Statement

Native header requires like `(:require ["flecs.h" :as flecs])` currently throw an error in WASM:
```
Error: "Native C++ headers are not supported in WASM"
```

This is a limitation that should be fixed. For WASM AOT compilation, native header requires should work because:
1. The `#include` is already processed during native jank AOT compilation
2. The C++ symbols are already in the generated code
3. We just need the alias metadata registered at runtime for `flecs/world` syntax to work

## Root Cause Analysis

### Current Flow in Native jank

1. User writes: `(ns my-ns (:require ["flecs.h" :as flecs]))`
2. At ns load time, `load-libs` (clojure/core.jank:4512) calls `register-native-header!`
3. `register-native-header!` (clojure/core.jank:4324-4338) calls:
   - `cpp/clojure.core_native.register_native_header` which:
     - Stores alias metadata via `ns_obj->add_native_alias()` (ns.cpp:171)
     - JIT compiles `#include <header>` via `__rt_ctx->eval_cpp_string()` (core_native.cpp:331)
4. Later, analyzer uses `find_native_alias()` to resolve `flecs/world` to `flecs::world`

### Why It Fails in WASM

1. **C++ side**: `register_native_header` is wrapped in `#ifndef JANK_TARGET_WASM` (core_native.cpp:306-411)
   - This is because `eval_cpp_string()` requires JIT, which doesn't exist in WASM

2. **Clojure side**: `register-native-header!` has reader conditional:
   ```clojure
   #?(:wasm (throw "Native C++ headers are not supported in WASM")
      :jank ...)
   ```

3. **Generated AOT code**: The `require` call is emitted as runtime code:
   ```cpp
   // Line 145 of generated code
   jank::runtime::dynamic_call(my_flecs_static_and_wasm_require_63->deref(), my_flecs_static_and_wasm_const_64);
   // my_flecs_static_and_wasm_const_64 = ["flecs.h" :as flecs]
   ```
   When this runs in WASM, it calls `require` which calls `register-native-header!` which throws.

### Key Insight

For WASM AOT, we DON'T need JIT compilation of `#include`. The include is already processed during native jank AOT compilation (line 46 of generated code has `#include "flecs.h"`). We ONLY need to:
1. Register the alias metadata in the namespace
2. This enables `find_native_alias()` to work at runtime (though it's not actually needed since symbols are already resolved in AOT)

## Available Infrastructure

These components are available in WASM (no `#ifndef JANK_TARGET_WASM` around them):

- `ns::add_native_alias()` - stores alias metadata (ns.cpp:171)
- `ns::find_native_alias()` - looks up alias (ns.hpp:55)
- `ns::add_native_refer()` - stores refer metadata (ns.hpp:58-59)
- `ns::find_native_refer()` - looks up refer (ns.hpp:61)
- `native_alias` struct with: `header`, `include_directive`, `scope` fields (ns.hpp:39-44)
- `native_refer` struct with: `alias`, `member` fields (ns.hpp:46-50)

## Proposed Solution

### Step 1: Add WASM-compatible C++ functions

In `src/cpp/clojure/core_native.cpp`, add WASM-specific versions that skip JIT:

```cpp
#ifdef JANK_TARGET_WASM
  // WASM version: only registers metadata, no JIT compilation needed
  // (the #include was already processed during AOT compilation)
  object_ref register_native_header_wasm(object_ref const current_ns,
                                         object_ref const alias,
                                         object_ref const header,
                                         object_ref const scope,
                                         object_ref const include_directive)
  {
    auto const ns_obj(try_object<ns>(current_ns));
    runtime::ns::native_alias alias_data{ runtime::to_string(header),
                                          runtime::to_string(include_directive),
                                          runtime::to_string(scope) };
    ns_obj->add_native_alias(try_object<obj::symbol>(alias), std::move(alias_data)).expect_ok();
    return jank_nil;
  }

  object_ref register_native_refer_wasm(object_ref const current_ns,
                                        object_ref const alias,
                                        object_ref const local_sym,
                                        object_ref const member_sym)
  {
    auto const ns_obj(try_object<ns>(current_ns));
    auto const alias_sym(try_object<obj::symbol>(alias));
    auto const member(try_object<obj::symbol>(member_sym));
    ns_obj->add_native_refer(try_object<obj::symbol>(local_sym), alias_sym, member).expect_ok();
    return jank_nil;
  }
#endif
```

### Step 2: Register the WASM functions

In `core_native.cpp` inside `void register_all()`, add:

```cpp
#ifdef JANK_TARGET_WASM
  intern_fn("register-native-header-wasm", &core_native::register_native_header_wasm);
  intern_fn("register-native-refer-wasm", &core_native::register_native_refer_wasm);
#endif
```

### Step 3: Update clojure.core.jank

Modify `register-native-header!` (line 4324):

```clojure
(defn- register-native-header! [spec]
  (let [{:keys [alias header scope include refers]} (normalize-native-libspec spec)]
    #?(:wasm (do
               ;; WASM AOT: Just register metadata, #include was already processed at compile time
               (cpp/clojure.core_native.register_native_header_wasm
                 *ns*
                 alias
                 header
                 scope
                 include)
               (doseq [{:keys [local member]} refers]
                 (cpp/clojure.core_native.register_native_refer_wasm
                   *ns*
                   alias
                   local
                   member)))
       :jank (do
               (cpp/clojure.core_native.register_native_header
                 *ns*
                 alias
                 header
                 scope
                 include)
               (doseq [{:keys [local member]} refers]
                 (cpp/clojure.core_native.register_native_refer
                   *ns*
                   alias
                   local
                   member))))))
```

### Step 4: Handle `native-header-functions` (Optional)

The `native-header-functions` function (line 7783) also throws in WASM. This is used for autocomplete/introspection. For WASM, we could:

**Option A**: Return empty vector (simplest)
```clojure
#?(:wasm []
   :jank (cpp/clojure.core_native.native_header_functions nil alias prefix))
```

**Option B**: Throw with better message
```clojure
#?(:wasm (throw "native-header-functions requires nREPL (not available in WASM)")
   :jank ...)
```

Recommend Option A for graceful degradation.

## Files to Modify

1. **`src/cpp/clojure/core_native.cpp`**
   - Add `register_native_header_wasm` function inside `#ifdef JANK_TARGET_WASM`
   - Add `register_native_refer_wasm` function inside `#ifdef JANK_TARGET_WASM`
   - Register both functions in `register_all()` with `#ifdef JANK_TARGET_WASM`

2. **`src/jank/clojure/core.jank`**
   - Modify `register-native-header!` (line 4324) to call WASM functions in `:wasm` branch
   - Optionally modify `native-header-functions` (line 7783) to return `[]` in `:wasm`

## Testing Plan

1. Build jank with the changes
2. Create test file with native header require:
   ```clojure
   (ns test-native-header
     (:require ["flecs.h" :as flecs]))

   (defn -main []
     (println "Native header require works!")
     (let [w (flecs/world)]
       (println "Flecs world:" w)))
   ```

3. Run with emscripten-bundle:
   ```bash
   ./bin/emscripten-bundle --skip-build --run \
     -I /path/to/flecs/distr \
     --lib /path/to/flecs.o \
     test-native-header.jank
   ```

4. Verify:
   - No "Native C++ headers are not supported in WASM" error
   - The `flecs/world` syntax works
   - Module executes successfully

## Complexity Assessment

- **Difficulty**: Medium
- **Risk**: Low (changes are additive, don't break native jank)
- **Lines of code**: ~50 C++, ~20 Clojure

## Alternative Approaches Considered

### Alternative 1: Skip require call in AOT code generation
Could modify the AOT code generator to not emit `require` calls for native headers. But this would:
- Require changes to the compiler
- Break consistency between native and WASM behavior
- Make the generated code harder to understand

### Alternative 2: Make require a no-op for native headers in WASM
Could make `register-native-header!` just return nil in WASM. But this would:
- Not register the alias metadata
- Break any runtime code that calls `find_native_alias()`
- Be a silent failure that's hard to debug

The proposed solution is preferred because it maintains the same semantics as native jank while simply skipping the JIT compilation step that's not needed in WASM AOT.

## Dependencies

None - uses existing infrastructure that's already compiled for WASM.
