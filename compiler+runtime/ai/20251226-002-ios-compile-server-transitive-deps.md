# iOS Compile Server Transitive Dependencies Issue

## Date: 2025-12-26

## Problem
When the iOS compile server compiles a namespace (e.g., `vybe.sdf.ios`), it only compiles that single namespace. However, the namespace has dependencies (`vybe.sdf.math`, `vybe.sdf.state`, etc.) that are also required when the compiled code runs on iOS.

When iOS loads the compiled `vybe.sdf.ios` module, it tries to `require` its dependencies. Since those dependencies are not compiled/marked as loaded, iOS tries to load them from source via `load_jank`, which crashes.

## Crash Stack
```
clojure_core_refer -> clojure_core_set -> transient_hash_set::conj_in_place -> hash::operator() -> assertion failure
```

The crash happens because during `load_jank`, an invalid object_ref with null data is created.

## Root Cause
The `require_ns` function in `server.hpp`:
1. Evaluates the ns form on macOS (which loads all dependencies via JIT)
2. Analyzes and compiles only the requested namespace
3. Returns only that one compiled module

The compiled code still contains `require` calls for dependencies. When iOS runs this code, it tries to require modules that don't exist on iOS.

## Solution: Implemented Option 1

Modified `require_ns` in `server.hpp` to compile transitive dependencies:

### Implementation
1. **Snapshot loaded modules before ns eval**: Use `loaded_modules_in_order.rlock()` to capture size
2. **Evaluate ns form**: This triggers JIT loading of all dependencies on macOS
3. **Get newly loaded modules**: After eval, diff `loaded_modules_in_order` to find new modules
4. **Compile each dependency**:
   - Skip core modules (using `module::is_core_module()`)
   - Skip already compiled modules (tracked in `loaded_namespaces_`)
   - Find source using `module_loader.find()`
   - Read source using `loader::read_file()`
   - Compile using helper `compile_namespace_source()`
5. **Return all modules in response**: Dependencies first (in load order), then main module

### Key Changes
- Added `compiled_module_info` struct to hold compilation results
- Added `compile_namespace_source()` helper method for compiling a single namespace
- Modified `require_ns()` to track and compile transitive dependencies
- Response now includes all compiled modules in load order

### Response Format
```json
{"op":"required","id":1,"modules":[
  {"name":"vybe.sdf.math$loading__","symbol":"_ns_load_123_0","object":"..."},
  {"name":"vybe.sdf.state$loading__","symbol":"_ns_load_124_0","object":"..."},
  {"name":"vybe.sdf.ios$loading__","symbol":"_ns_load_125_0","object":"..."}
]}
```

### iOS Loader Changes (loader.cpp)
The iOS loader was updated to:
1. **Strip `$loading__` suffix** from module names to get clean module names
2. **Call `set_is_loaded()`** for each module BEFORE executing it
3. **Add to `loaded_modules_in_order`** to track load order
4. **Log each loaded module** for debugging

This ensures that when the main module's code runs and tries to `require` its dependencies, they are already marked as loaded and won't be loaded again.

### Files Modified
- `compiler+runtime/include/cpp/jank/compile_server/server.hpp`:
  - Added `compiled_module_info` struct
  - Added `compile_namespace_source()` helper method
  - Modified `require_ns()` to track and compile transitive dependencies

- `compiler+runtime/src/cpp/jank/runtime/module/loader.cpp`:
  - Modified remote compile loading to mark each module as loaded before executing it
