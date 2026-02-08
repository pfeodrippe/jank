# WASM AOT Module Require Fix

**Date**: 2025-12-21

## Problem

When compiling jank modules with `--codegen wasm-aot`, inter-module dependencies failed with:
```
Failed to find symbol: 'jank_load_vybe_sdf_math'
```

This happened because:
1. `vybe.sdf.ios` requires `[vybe.sdf.math :as m]`
2. The `require` function called `clojure.core-native/load-module`
3. That function used `module::origin::latest` which tried to load cached `.o` files
4. Loading `.o` files requires JIT-loading the `jank_load_*` symbol
5. JIT loading fails in WASM AOT mode (no JIT compiler available)

## Root Cause

In `compiler+runtime/src/cpp/clojure/core_native.cpp`, the `load_module` function always used `module::origin::latest`:

```cpp
object_ref load_module(object_ref const path)
{
  __rt_ctx->load_module(runtime::to_string(path), module::origin::latest).expect_ok();
  return jank_nil;
}
```

## Fix

Modified `load_module` to use `module::origin::source` when in WASM AOT mode:

```cpp
object_ref load_module(object_ref const path)
{
  /* For WASM AOT compilation, force loading from source to recompile
   * dependencies rather than trying to JIT-load cached object files.
   * This allows modules to require other jank modules during AOT compilation. */
  auto const ori = (util::cli::opts.codegen == util::cli::codegen_type::wasm_aot)
    ? module::origin::source
    : module::origin::latest;
  __rt_ctx->load_module(runtime::to_string(path), ori).expect_ok();
  return jank_nil;
}
```

Also added the required include:
```cpp
#include <jank/util/cli.hpp>
```

## Files Modified

- `compiler+runtime/src/cpp/clojure/core_native.cpp` (lines 8, 319-329)

## Testing

Before fix:
```bash
$ jank --codegen wasm-aot compile-module vybe.sdf.ios
error: Failed to find symbol: 'jank_load_vybe_sdf_math'
```

After fix:
```bash
$ jank --codegen wasm-aot --save-cpp-path /tmp/test.cpp \
    -I . -I vendor/imgui ... \
    --jit-lib /opt/homebrew/lib/libvulkan.dylib ... \
    compile-module vybe.sdf.ios
[jank] Saved generated C++ to: /tmp/test.cpp
[jank] WASM AOT mode: skipping JIT compilation
```

The fix allows modules with inter-module dependencies to compile successfully in WASM AOT mode.

## Why `origin::source` Works

- `origin::source` forces the loader to load from `.jank` source files
- Source files are parsed, analyzed, and code-generated (not JIT-compiled in WASM AOT mode)
- This creates all the necessary namespace and var definitions
- Dependent modules can then reference these definitions

## Notes

- Native C++ header requires still need proper `-I` include paths and `--jit-lib` flags
- All modules in the dependency chain are compiled to C++, not JIT-loaded
- This is the expected behavior for AOT compilation
