# AOT Source Functions for Debugging

## Date: 2025-11-30

## Summary
Added four new functions to `jank.compiler` namespace for viewing AOT-compiled C++ source code:

### Native AOT (module target)
- `native-aot-source-raw` - Returns raw unformatted native AOT C++ source
- `native-aot-source-formatted` - Returns formatted native AOT C++ source

### WASM AOT (wasm_aot target)
- `native-wasm-aot-source-raw` - Returns raw unformatted WASM AOT C++ source
- `native-wasm-aot-source-formatted` - Returns formatted WASM AOT C++ source

## Key Differences

| Target | compilation_target | Use Case |
|--------|-------------------|----------|
| `native-cpp-source-*` | `eval` | JIT/REPL (wrapped for immediate evaluation) |
| `native-aot-source-*` | `module` | Native AOT (includes `jank_ns_intern_c()`) |
| `native-wasm-aot-source-*` | `wasm_aot` | WASM AOT (no `jank_ns_intern_c()`, standalone) |

The main difference between `module` and `wasm_aot` is that `module` includes
`jank_ns_intern_c()` call in the load function, while `wasm_aot` omits it for
standalone WASM modules.

## Implementation Details

### Files Modified
1. `src/cpp/jank/compiler_native.cpp`:
   - Added `render_cpp_aot_declaration()` helper using `compilation_target::module`
   - Added `render_cpp_wasm_aot_declaration()` helper using `compilation_target::wasm_aot`
   - Added all four functions and registered them

2. `src/jank/jank/compiler.jank`:
   - Added bindings for all four new functions

### Compilation Targets (from `codegen/llvm_processor.hpp`)
```cpp
enum class compilation_target : u8
{
  module,      // Native AOT module compilation (includes ns_intern)
  function,    // Function-level compilation
  eval,        // JIT/REPL evaluation (wrapped expression)
  wasm_aot,    // Standalone WASM modules (no ns_intern)
  wasm_patch   // WASM hot-reload patches
};
```

## Usage
```clojure
(require '[jank.compiler :as c])

;; WASM AOT source (what emscripten-bundle uses)
(println (c/native-wasm-aot-source-formatted '(+ 1 2)))

;; Native AOT source (for native compilation)
(println (c/native-aot-source-formatted '(+ 1 2)))

;; JIT source (for REPL evaluation)
(println (c/native-cpp-source-formatted '(+ 1 2)))
```
