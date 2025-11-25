# WASM Development Context

## Session: November 25, 2025

### Current Status

**LLVM IR to WASM compilation works for simple user code, but NOT for clojure.core!**

### What Works
- **LLVM IR generation**: `--codegen llvm_ir --save-llvm-ir` generates LLVM IR files for user code
- **LLVM IR to WASM compilation**: System LLVM's `llc -march=wasm32` compiles simple IR to WASM objects
- **Linking with emscripten**: `em++` links WASM objects with `libjank.a`
- Simple jank code without `ns` forms compiles and runs in WASM
- Functions from `clojure.core-native` are available (229 vars)
- One-liner script: `./bin/jank-wasm file.jank`

### What Doesn't Work (Yet)
- **clojure.core.jank to WASM** - The native LLVM IR uses 64-bit pointers (`i64 ptrtoint`) which can't be compiled to WASM32. RTTI data has incompatible pointer arithmetic.
- Files with `(ns ...)` forms - they need `refer`, `*loaded-libs*`, `map`, `filter` which are in `clojure.core.jank`
- Higher-level sequence functions: `map`, `filter`, `reduce`, etc. - these are in `clojure.core.jank`, not native

### The Core Problem

Native jank compiles clojure.core.jank to native object files (core.o) which are pre-built in phase-1. The LLVM IR generated targets native architecture (arm64) with 64-bit pointers.

When trying to compile clojure.core's LLVM IR to WASM32:
```
error: unsupported expression in static initializer: ptrtoint (ptr @_ZTS... to i64)
error: value evaluated as -9223372036854775808 is out of range
```

This is because:
1. Native LLVM IR uses `i64` for pointer operations (64-bit)
2. WASM32 uses 32-bit pointers
3. RTTI (Run-Time Type Information) global initializers have `ptrtoint` converting pointers to `i64`
4. The 64-bit values can't fit in WASM32's 32-bit address space

### Potential Solutions

1. **Make jank generate WASM-targeted LLVM IR from the start** - Change `llvm_processor.cpp` to use `wasm32-unknown-emscripten` as target triple when a CLI flag is set
2. **Disable RTTI for WASM builds** - Build with `-fno-rtti`, but this breaks exception handling
3. **Use the C++ codegen path** - Continue using `--codegen wasm_aot` which generates C++ that emscripten can compile

### LLVM IR to WASM Build Process (for simple user code)

1. **Generate LLVM IR from jank** (use `run` command, not `compile-module`):
   ```bash
   ./build/jank --codegen llvm_ir --save-llvm-ir --save-llvm-ir-path build-wasm/output.ll run input.jank
   ```

2. **Convert IR to WASM target**:
   ```bash
   sed 's/target triple = "arm64-apple-macosx[^"]*"/target triple = "wasm32-unknown-emscripten"/' output.ll > output_wasm.ll
   sed -i '' 's/e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32/e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-n32:64-S128-ni:1:10:20/' output_wasm.ll
   ```

3. **Compile LLVM IR to WASM object**:
   ```bash
   llc -march=wasm32 -filetype=obj -o output.o output_wasm.ll
   ```

4. **Link with emscripten**:
   ```bash
   em++ -o output.js output.o build-wasm/libjank.a build-wasm/libjankzip.a build-wasm/third-party/bdwgc/libgc.a \
     -sEXPORT_ES6=1 -sMODULARIZE=1 ...
   ```

5. **Use the jank-wasm script**:
   ```bash
   ./bin/jank-wasm wasm-examples/eita3.jank
   cd build-wasm && node run_eita3.mjs
   ```

### Key Discoveries

1. **LLVM IR generation works!** Use `--codegen llvm_ir --save-llvm-ir` with `run` command
2. **System LLVM has WASM target**: `llc -march=wasm32` works with Homebrew LLVM
3. **compile-module uses pre-compiled core.o** - It doesn't recompile clojure.core, it uses the cached object file
4. **To force recompile clojure.core**: Must move `build/core-libs/clojure/core.o` aside
5. **64-bit to 32-bit pointer problem**: Native LLVM IR can't be trivially converted to WASM32 due to pointer size differences in RTTI
6. **Initialization order matters**:
   - Call `jank_load_clojure_core_native()` first
   - Then call `jank_setup_clojure_core_for_wasm()` to refer 229 vars to `clojure.core`
   - Then call user's global init function
   - Then call user's load function

### Commands Learned

```bash
# Generate LLVM IR for user code
./build/jank --codegen llvm_ir --save-llvm-ir --save-llvm-ir-path build-wasm/file.ll run file.jank

# Generate LLVM IR for clojure.core (must move core.o aside first!)
mv build/core-libs/clojure/core.o build/core-libs/clojure/core.o.bak
./build/jank --codegen llvm_ir --save-llvm-ir --save-llvm-ir-path build-wasm/core-libs/clojure/core.ll compile-module clojure.core
mv build/core-libs/clojure/core.o.bak build/core-libs/clojure/core.o

# Strip debug info from LLVM IR (reduces size but doesn't fix RTTI issue)
opt -S -strip-debug input.ll -o output_stripped.ll

# Check what functions are in the IR
grep "^define" build-wasm/file.ll

# Compile IR to WASM object
llc -march=wasm32 -filetype=obj -o build-wasm/file.o build-wasm/file_wasm.ll

# Use the one-liner script
./bin/jank-wasm wasm-examples/eita3.jank
```

## Test Files That Work

- `eita3.jank` - Simple `def` and `println` without `ns` form (LLVM IR path)
- `minimal.jank` - Simple `def` and `println` (C++ path)
- `basic.jank` - Arithmetic, `even?`, `println` (C++ path)

## Test Files That Don't Work

- `eita2.jank` - Has `(ns ...)` which needs `refer` from clojure.core.jank
- Files using `map`, `filter` - not in core-native

## C API Functions Added to WASM

Added to `c_api_wasm.cpp`:
- `jank_symbol_create`
- `jank_var_set_dynamic`
- `jank_list_create`
- `jank_vector_create`
- `jank_map_create`
- `jank_set_create`
- `jank_function_build_arity_flags`
- `jank_function_create`
- `jank_function_set_arity0` through `jank_function_set_arity10`
- `jank_set_meta`

