## 2026-01-02 - iOS JIT: binding_scope destructor + "expected symbol found nil"

### Symptom
- During iOS remote-JIT module loading, after calling an entry function (e.g. `vybe.util`), the runtime prints `Exception caught while destructing binding_scope` and then fails with `invalid object type (expected symbol found nil)`.

### Key discovery
- `binding_scope::~binding_scope()` in `compiler+runtime/src/cpp/jank/runtime/context.cpp` catches and **swallows** the real exception thrown by `pop_thread_bindings().expect_ok()`, so the print is often a secondary symptom.
- This repo already contains investigation notes pointing to a common root cause: **ABI/function signature mismatches** for `jank_load_*` entrypoints can corrupt state and later manifest as binding/pop failures and “expected symbol found nil”.

### Concrete repo issue fixed
- Found inconsistent `extern "C"` declarations in:
   - `compiler+runtime/src/cpp/main.cpp` declaring `jank_load_jank_{nrepl_server_asio,arena_native,debug_allocator_native}` as returning `jank_object_ref`.
   - `compiler+runtime/src/cpp/compile_server_main.cpp` declaring `jank_load_jank_nrepl_server_asio` as returning `void*`.
- Actual definitions (and the iOS loader) treat these `jank_load_*` functions as `void`, so the inconsistent declarations are UB and can contribute to hard-to-explain crashes.
- Patched these declarations to `extern "C" void ...();` to match the implementation and loader behavior.

### Useful pointers
- Throw site masking: `compiler+runtime/src/cpp/jank/runtime/context.cpp` (`binding_scope` destructor).
- iOS loader calls `jank_load_*` as `void (*)()` in `compiler+runtime/src/cpp/jank/runtime/module/loader.cpp`.

### Output (this instruction)
- Added richer diagnostics around iOS remote entry execution and loader phase-2 entry invocation so logs now include the failing module/entry symbol plus current `*ns*` and `*file*` at the point the exception is thrown.

## 2026-01-02 - iOS JIT: validate_meta sees corrupted meta (type=128)

### Symptom
- During `vybe.util` module load, `behavior::detail::validate_meta()` reports `meta type=128` (unknown) and throws `corrupted meta object type: 128`.
- This originally manifested as a panic in `jank::runtime::to_string()` from `validate_meta()` (when the error path tried to stringify the meta).

### Discovery
- The meta pointer was garbage/uninitialized `object_ref` (first byte `0x80` == 128), not a valid runtime object.
- Root cause is in C++ codegen lifting: when compiling a module, lifted vars/constants are *supposed* to live as module-scope globals and be initialized in the module load function via placement-new.
- Nested function structs (generated with `compilation_target::function` during module compilation) were also emitting lifted vars/constants as **struct members**, which shadow the module-scope globals.
- Those shadowing members were never initialized (only `compilation_target::eval` initializes lifted members), so any use (including var metadata maps) read garbage.

### Fix
- Introduced an `owner_target` concept in `codegen::processor` so nested processors know they are being generated under a module/wasm-aot compilation.
- For nested `compilation_target::function` under `owner_target == module/wasm_aot`, codegen no longer emits lifted var/constant members; unqualified references resolve to the module-scope globals which are correctly initialized in the module load function.

### Follow-up
- Even after eliminating member shadowing, iOS JIT could still hit corrupted meta values (e.g. type=160) during `var::with_meta()`.
- To remove the remaining dependency on lifted-constant storage for *var metadata* in module-like builds, `def` codegen now **inlines** the metadata constant expression directly in the generated `->with_meta(...)` call when `owner_target` is `module`/`wasm_aot`.
- This keeps eval builds unchanged while making module-loading more robust (metadata is only needed once per `def`, so inlining is acceptable).

### Files changed
- compiler+runtime/include/cpp/jank/codegen/processor.hpp
- compiler+runtime/src/cpp/jank/codegen/processor.cpp
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


## November 25, 2025 Update - Merged PR #598

### What Was Merged
Successfully merged C++ codegen improvements from https://github.com/jank-lang/jank/pull/598

### Key Changes from PR #598
- **Fixed void return handling in codegen** - Properly handles void-returning C++ functions
- **Improved type handling with cpp_util** - Uses `Cpp::IsVoid()` and `cpp_util::get_qualified_type_name()`
- **Added better error handling in JIT evaluation** - JIT processor throws on C++ eval failures
- **Fixed let binding codegen for arrays** - Handles C++ array types properly in let bindings
- **Added JANK_PRINT_IR environment variable** - Set to "1" to print generated C++ during codegen

### Merge Conflicts Resolved
1. **CMakeLists.txt** - Combined WASM support (`jank_target_wasm`) with debug GC (`jank_debug_gc`)
2. **bdwgc.cmake** - Combined WASM gctba skip with debug GC assertions
3. **builtin.hpp** - Kept `t.erase()` version for `from_object`
4. **processor.cpp** - Accepted PR version with improved void/type handling
5. **evaluate.cpp** - Accepted PR version with `JANK_PRINT_IR`
6. **jit/processor.cpp** - Kept our branch's detailed error handling for `load_dynamic_library`
7. **cli.cpp** - Added `wasm-aot` to codegen types alongside PR's naming

### Codegen Options After Merge
```
--codegen ENUM:{llvm-ir,cpp,wasm-aot} [default: cpp]
```
- `llvm-ir` - Generates LLVM IR (original path)
- `cpp` - Generates C++ (fixed in PR #598)
- `wasm-aot` - Generates standalone C++ for WASM AOT compilation

### Build Notes After Merge
- jank binary compiles successfully
- Core library compilation has a runtime error (unrelated to merge)
- Error: `invalid object type: unknown raw value 96`
- This appears to be a pre-existing issue in the nrepl branch


## November 26, 2025 - Browser WASM Fix

### Issue
The WASM bundle was working with Node.js but failing in the browser.

### Root Cause
The HTML wrapper generated by `emscripten-bundle` used ES6 dynamic `import()` inside a regular `<script>` tag. Browsers require `<script type="module">` for ES6 module imports to work.

The generated JS file uses `-sEXPORT_ES6=1` and `-sMODULARIZE=1`, which creates an ES6 module that must be imported with proper ES module syntax.

### Fix
Changed the `<script>` tag to `<script type="module">` in the HTML template within `bin/emscripten-bundle`.

### Key Learning
When using Emscripten with `-sEXPORT_ES6=1`, the generated JavaScript is an ES6 module. To load it in a browser:
- Must use `<script type="module">` for ES6 import syntax
- Or serve via HTTP (not `file://`) since ES modules have CORS restrictions
- The `import()` dynamic import syntax only works in module contexts

### Commands for Browser Testing
```bash
# Build WASM bundle
./bin/emscripten-bundle eita.jank

# Serve with HTTP server (required for ES modules)
python3 -m http.server -d build-wasm 8080
# Then open http://localhost:8080/eita.html
```

### Browser Function Calling Feature
Added UI to call jank functions from the browser:
- Input field for function name (e.g., `my-func`)
- Input field for numeric argument
- "Call Function" button
- Automatically tries multiple C symbol name patterns:
  - `_jank_user_<name>` - User namespace functions
  - `_jank_call_<name>` - Call wrappers
  - Direct C names
- Handles jank name munging (- → _, ? → _QMARK_, etc.)
- Falls back to `ccall` if direct export not found
- Lists available exports if function not found


## November 26, 2025 - ^:export Metadata for WASM

### Feature Added
Implemented `^:export` metadata support for exporting jank functions to WASM/JavaScript.

### How It Works

1. **In your jank code**, add `^:export` metadata to any function:
   ```clojure
   (defn ^:export ggg
     "Example exported function"
     [v]
     (+ 45 v))
   ```

2. **During WASM AOT compilation**, jank scans the namespace for vars with `:export` metadata and generates `extern "C"` wrappers:
   ```cpp
   extern "C" void* jank_export_ggg(void* arg) {
     using namespace jank::runtime;
     auto const var = __rt_ctx->find_var("eita", "ggg");
     if(var.is_nil()) { return nullptr; }
     auto const fn = var->deref();
     if(arg == nullptr) {
       return jank::runtime::dynamic_call(fn).erase();
     }
     return jank::runtime::dynamic_call(fn, oref<object>{reinterpret_cast<object*>(arg)}).erase();
   }
   ```

3. **The emscripten-bundle script** detects these exports and adds them to `-sEXPORTED_FUNCTIONS`:
   ```
   -sEXPORTED_FUNCTIONS=_main,_jank_export_ggg
   ```

4. **In JavaScript/browser**, call the exported function via `ccall` or `cwrap`:
   ```javascript
   // Using cwrap to create a JavaScript wrapper
   const ggg = Module.cwrap('jank_export_ggg', 'number', ['number']);
   const result = ggg(someJankObject);
   
   // Or using ccall for one-off calls
   const result = Module.ccall('jank_export_ggg', 'number', ['number'], [someJankObject]);
   ```

### Implementation Details

**Files Modified:**
- `compiler+runtime/src/cpp/main.cpp` - Added code to scan namespace vars for `:export` metadata and generate wrappers
- `compiler+runtime/bin/emscripten-bundle` - Added detection of `jank_export_*` functions in generated C++ to add to EXPORTED_FUNCTIONS

**Export Function Signature:**
```cpp
extern "C" void* jank_export_<munged_name>(void* arg)
```
- Takes a jank object pointer (or nullptr to call with no args)
- Returns a jank object pointer (the result)
- Uses `dynamic_call` to invoke the jank function

### Key Learning
- jank vars store metadata in `var->meta` as an optional object
- The `:export` keyword is interned as `__rt_ctx->intern_keyword("export")`  
- Namespace mappings are accessed via `ns->get_mappings()` which returns a persistent_hash_map
- Function name munging converts `-` to `_` etc. via `munge()`
- Emscripten requires functions to be explicitly listed in `-sEXPORTED_FUNCTIONS` to be callable from JS

---

## Session: November 29, 2025

### Task: nREPL Autocompletion for Nested C++ Type Members

**Problem:** Autocomplete worked for top-level symbols like `flecs/world` and `flecs/type`, but not for their nested members like `flecs/world.defer_begin`.

### Key Discoveries

1. **nREPL Autocompletion Architecture:**
   - `engine.hpp` contains the nREPL message handling including `handle_completions()` and `handle_complete()`
   - Completion queries go through `prepare_completion_query()` → `collect_symbol_names()` → `make_completion_candidates()`
   - For native C++ header aliases (like `flecs`), completion uses `native_header_index_.list_functions()`
   - The `handle_complete` op (used by CIDER) calls `native_header_index_.contains()` to check if a candidate exists

2. **Root Cause (Multiple Issues):**
   - **Issue 1:** `list_functions()` used cached top-level symbols, but `enumerate_native_header_member_symbols()` was never called for nested member access
   - **Issue 2:** `contains()` method also used the cache directly without handling nested member access
   - **Issue 3:** `describe_native_header_function()` used `GetFunctionsUsingName(scope, symbol_name)` where `symbol_name` was `world.defer_begin`, but `GetFunctionsUsingName` expects just a function name, not a qualified member access

3. **The `enumerate_native_header_member_symbols()` function already existed!**
   - Located in `native_header_completion.cpp`
   - It properly uses CppInterOp to enumerate members of a C++ type
   - It was NOT being used in the `list_functions()` path for the "complete" op

### Solution (Three Fixes)

**Fix 1:** Modified `native_header_index.cpp` - `list_functions()`:
- Added `parse_nested_prefix()` helper to detect member access patterns (e.g., `"world.defer_begin"` → `{"world", "defer_begin"}`)
- When a dot is found in the prefix, it bypasses the cache and calls `enumerate_native_header_member_symbols()` directly

**Fix 2:** Modified `native_header_index.cpp` - `contains()`:
- Added same nested member access detection
- For nested members, it calls `enumerate_native_header_member_symbols()` and checks if the name exists in the result

**Fix 3:** Modified `engine.hpp` - `describe_native_header_function()`:
- Added nested member access parsing
- For `world.defer_begin`, it now:
  1. Looks up the `world` type in the namespace scope
  2. Uses that type scope to look up `defer_begin`

### Test Infrastructure Added

Created test header `compiler+runtime/include/cpp/test/nested_types.hpp` with:
- `test::nested_types::world` struct with member functions like `defer_begin`, `defer_end`, etc.
- `test::nested_types::entity` struct with member functions

Added test case in `compiler+runtime/test/cpp/jank/nrepl/engine.cpp`:
- `TEST_CASE("complete returns nested member functions of native header types")`
- Tests that `nested-test/world.defer` returns `world.defer_begin` and `world.defer_end` completions

### Files Modified
- `compiler+runtime/src/cpp/jank/nrepl_server/native_header_index.cpp`
- `compiler+runtime/include/cpp/jank/nrepl_server/engine.hpp`
- `compiler+runtime/include/cpp/test/nested_types.hpp` (new)
- `compiler+runtime/test/cpp/jank/nrepl/engine.cpp`

### Commands Learned
```bash
# Build jank
cd compiler+runtime && ./bin/compile

# Run all tests
./bin/test

# Run specific test case
./bin/test --test-case="*nested*"
```

### Key Files for Future Reference
- `compiler+runtime/include/cpp/jank/nrepl_server/engine.hpp` - nREPL message handling
- `compiler+runtime/include/cpp/jank/nrepl_server/native_header_index.hpp` - Header symbol index
- `compiler+runtime/src/cpp/jank/nrepl_server/native_header_index.cpp` - Implementation
- `compiler+runtime/src/cpp/jank/nrepl_server/native_header_completion.cpp` - Symbol enumeration using CppInterOp
- `compiler+runtime/include/cpp/jank/nrepl_server/ops/complete.hpp` - The "complete" op handler (used by CIDER)
- `compiler+runtime/include/cpp/jank/nrepl_server/ops/completions.hpp` - The "completions" op handler

### Additional Fix: Crash on Complex Types (flecs::world)

**Problem:** When trying to autocomplete `flecs/world.` in CIDER, the nREPL server crashed with `EXC_BAD_ACCESS` in `Cpp::GetAllCppNames()`.

**Root Cause:** The `flecs::world` class is a complex C++ type with template instantiations and special declarations. When `Cpp::GetAllCppNames()` iterates over its declarations, some of them have invalid/null pointers, causing the crash in `clang::Decl::getNextDeclInContext()`.

**Fix:** Added a check using `Cpp::IsComplete(type_scope)` before calling `Cpp::GetAllCppNames()`. Incomplete types (forward declarations, templates not yet instantiated, etc.) are now skipped, preventing the crash.

**File Modified:** `compiler+runtime/src/cpp/jank/nrepl_server/native_header_completion.cpp`
