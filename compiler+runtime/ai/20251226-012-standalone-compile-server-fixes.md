# Standalone Compile Server Fixes

## Date: 2025-12-26

## Summary

Fixed two critical issues with the standalone compile-server binary:

1. **fn macro not expanding** - The `refer_clojure_core()` fix
2. **Native header compilation failing** - The include paths initialization timing fix

## Issue 1: fn Macro Not Expanding

### Symptom
```clojure
(def y (fn [] 42))
```
Would crash with assertion failure in codegen because `fn` wasn't being recognized as a macro.

### Root Cause
When the compile server created a namespace with `intern_ns()`, clojure.core wasn't being referred to it. Without `clojure.core` referred, `fn` couldn't be found as a macro during `macroexpand1`, so the form was treated as a function call to undefined `fn`.

### Fix
Added `refer_clojure_core()` helper function in `server.hpp` that iterates all public vars from `clojure.core` and refers them to the target namespace:

```cpp
// In compile_code():
auto const ns_sym = runtime::make_box<runtime::obj::symbol>(jtl::immutable_string(ns));
auto const eval_ns = runtime::__rt_ctx->intern_ns(ns_sym);

// CRITICAL: Refer clojure.core vars to the namespace
refer_clojure_core(eval_ns);
```

### Files Modified
- `include/cpp/jank/compile_server/server.hpp` - Added `refer_clojure_core()` function

## Issue 2: Native Header Compilation Failing

### Symptom
```
[jank-jit] Attempting to compile: #include <vulkan/sdf_engine.hpp>
[jank-jit] Compilation FAILED for: #include <vulkan/sdf_engine.hpp>
```

Native headers like `vulkan/sdf_engine.hpp` couldn't be found even when `-I` include paths were passed to the compile server.

### Root Cause
The include paths were being parsed and stored, but they weren't being added to `util::cli::opts.include_dirs` until AFTER `jank_init()` was called. The JIT processor is initialized during `jank_init()` and reads `include_dirs` at that time. By the time we added the paths, the JIT had already been initialized without them.

### Fix
Added include paths to `util::cli::opts.include_dirs` BEFORE calling `jank_init()`:

```cpp
// In main(), before jank_init():
// CRITICAL: Add include paths to cli opts BEFORE jank_init()
for(auto const &inc : g_include_paths)
{
  jank::util::cli::opts.include_dirs.push_back(inc);
}

return jank_init(argc, argv, true, compile_server_main);
```

Also updated `compile_server_main()` to add paths to both places (for cross-compilation):

```cpp
for(auto const &inc : g_include_paths)
{
  config.include_paths.push_back(inc);           // For cross-compilation
  util::cli::opts.include_dirs.push_back(inc);   // For local JIT (redundant but safe)
}
```

### Files Modified
- `src/cpp/compile_server_main.cpp` - Added include paths before jank_init()

## Testing

### Test 1: fn macro (macOS REPL)
```bash
clj-nrepl-eval -p 5557 "(def y (fn [] 42))"
# => #'user/y

clj-nrepl-eval -p 5557 "(y)"
# => 42
```

### Test 2: Native header compilation (standalone server + iOS app)
```
[jank-jit] Attempting to compile: #include <vulkan/sdf_engine.hpp>
[jank-jit] Compilation SUCCEEDED for: #include <vulkan/sdf_engine.hpp>
```

## Remaining Issue

The iOS app now fails with a different error:
```
Unable to find module 'vybe.sdf.math'
```

This is because the compile server doesn't have the jank source paths configured for the vybe modules. This is a separate issue that needs the source directory to be added to the module search paths.

## Usage

```bash
./build/compile-server --target sim --port 5570 \
  -I /path/to/ios-resources/include \
  -I /opt/homebrew/include \
  -I /path/to/project \
  -I /path/to/project/vendor
```
