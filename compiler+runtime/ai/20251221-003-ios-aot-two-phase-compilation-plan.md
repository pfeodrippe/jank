# iOS AOT Two-Phase Compilation Plan

**Date**: 2025-12-21

## Problem

When compiling `vybe.sdf.ios` with `--codegen wasm-aot`, jank fails with:
```
Failed to find symbol: 'jank_load_vybe_sdf_math'
```

This happens because:
1. `vybe.sdf.ios` requires `[vybe.sdf.math :as m]`
2. During AOT compilation, jank's module loader tries to find `jank_load_vybe_sdf_math`
3. This symbol doesn't exist because we're generating code for iOS, not loading for macOS

## Root Cause Analysis

From `compiler+runtime/src/cpp/jank/runtime/module/loader.cpp` lines 988-992:
```cpp
auto const existing_load{ __rt_ctx->jit_prc.find_symbol(load_function_name) };
if(existing_load.is_ok())
{
  reinterpret_cast<object *(*)()>(existing_load.expect_ok())();
  return ok();
}
```

The module loader checks if `jank_load_<module>` exists in the JIT. If found, it calls it.
If not found, it tries to load the object file.

## Solution: Two-Phase Compilation

The jank CLI has `--obj TEXT ...` option that loads object files into the JIT.

### Phase 1: Compile Dependencies for macOS (JIT-loadable)

Compile each dependency module **without** `--codegen wasm-aot` to create macOS .o files:

```bash
# Compile vybe.sdf.math for macOS (JIT)
jank --module-path src compile-module vybe.sdf.math
# Creates: ~/.jank/binary-cache/vybe/sdf/math.o (or similar)

# Compile other dependencies
jank --module-path src compile-module vybe.sdf.state
jank --module-path src compile-module vybe.sdf.shader
jank --module-path src compile-module vybe.sdf.ui
```

### Phase 2: Compile vybe.sdf.ios with Pre-loaded Dependencies

Use `--obj` to load macOS dependencies into JIT, then generate iOS AOT code:

```bash
jank --module-path src \
  --codegen wasm-aot \
  --obj ~/.jank/binary-cache/vybe/sdf/math.o \
  --obj ~/.jank/binary-cache/vybe/sdf/state.o \
  --obj ~/.jank/binary-cache/vybe/sdf/shader.o \
  --obj ~/.jank/binary-cache/vybe/sdf/ui.o \
  --save-cpp-path SdfViewerMobile/generated/vybe_sdf_ios_generated.cpp \
  compile-module vybe.sdf.ios
```

### Phase 3: Cross-compile to iOS

Use clang to compile the generated C++ for iOS ARM64 (same as current build script).

## Implementation Steps

1. Modify `build_ios_jank_aot.sh` to:
   - First compile dependencies for macOS (creates .o files in binary cache)
   - Collect paths to .o files
   - Pass `--obj <path>` for each dependency when compiling modules with `--codegen wasm-aot`

2. Update the module compilation loop to:
   - Track which modules have been compiled for macOS
   - Pass all previously-compiled .o files to subsequent compilations

## Alternative Approach: Compile All for macOS First

Simpler approach - compile ALL modules for macOS first, then re-compile all for iOS AOT:

```bash
# Phase 1: Build all modules for macOS (populates binary cache)
for module in "${VYBE_MODULES[@]}"; do
    jank --module-path src compile-module "$module"
done

# Phase 2: Re-compile all for iOS AOT (uses macOS .o files for symbol resolution)
for module in "${VYBE_MODULES[@]}"; do
    jank --module-path src \
        --codegen wasm-aot \
        --save-cpp-path "generated/${module}_generated.cpp" \
        compile-module "$module"
done
```

The second pass should find the .o files from the first pass in the binary cache.

## Notes

- macOS .o files are only used during COMPILATION for symbol resolution
- iOS .o files are used in the FINAL binary
- The two don't need to be compatible - different architectures is fine

## Update: Issue with Current Approach

The two-phase approach doesn't work because:
1. Each jank invocation is a fresh process
2. WASM AOT mode skips JIT compilation entirely
3. When compiling vybe.sdf.ios, it tries to `:require` vybe.sdf.math
4. This triggers `load_module` which looks for `jank_load_vybe_sdf_math` symbol
5. The symbol doesn't exist because we're in WASM AOT mode (no JIT loading)

## Required jank Compiler Fix

The jank compiler needs modification for WASM AOT mode:

**Option A: Skip module loading in WASM AOT**
- When `--codegen wasm-aot`, don't try to JIT-load required modules
- Just generate forward declarations for `jank_load_<module>`
- Assume all required modules will be compiled separately

**Option B: Track compiled modules across invocations**
- After compiling a module in WASM AOT mode, record it somewhere
- When another module requires it, check the record instead of JIT loading
- Generate appropriate forward declarations

**Option C: Single-process multi-module WASM AOT**
- Add ability to compile multiple modules in one invocation
- First module compiles and registers itself
- Subsequent modules find the already-compiled modules

## Temporary Workaround

Until jank is fixed, use standalone modules (like iosmain4) that only require C++ headers, not other jank modules.

## UPDATE: Issue Fixed!

**Fixed in**: `compiler+runtime/src/cpp/clojure/core_native.cpp`

The fix was simpler than the options above: modify the `load_module` native function to use `origin::source` when in WASM AOT mode. This forces the loader to compile dependencies from source rather than trying to JIT-load cached `.o` files.

See `20251221-004-wasm-aot-module-require-fix.md` for details.
