# Claude Code Session Notes

This file documents how Claude Code was used in this project and best practices for future sessions.

## Session Overview

**Goal**: Implement WASM support for jank using real Clojure implementations instead of hardcoded C++ shortcuts.

**Outcome**: Successfully removed C++ "cheat" implementations and created a WASM-compatible subset of clojure.core that uses real Jank code compiled to C++.

## Key Accomplishments

1. Removed temporary C++ implementations of `map`, `filter`, `even?` from `core_native.cpp`
2. Created `src/jank/clojure/core_wasm.jank` with pure Jank implementations
3. Compiled and integrated `clojure.set` for WASM builds
4. Updated build system to auto-detect and include compiled modules
5. Verified native build still works (no regressions)
6. **Added reader conditional support with `:wasm` feature** for platform-specific code

## Project Context

### Architecture
- **Native builds**: JIT compilation, full `clojure/core.jank` with C++ interop
- **WASM builds**: AOT compilation only, subset of core without C++ interop
- **Shared runtime**: Both use same C++ runtime functions from `core_native.cpp`

### Critical Files
- `src/jank/clojure/core_wasm.jank` - WASM-compatible core subset
- `src/cpp/clojure/core_native.cpp` - C++ runtime functions
- `bin/emscripten-bundle` - WASM build script
- `src/cpp/jank/runtime/context.cpp` - Codegen & module loading

## How to Work on This Codebase

### Running Tests
```bash
# Native build
./bin/compile
./build/jank run test-file.jank

# WASM build
./bin/emscripten-bundle --skip-build --run wasm-examples/eita2.jank

# Verify native still works
echo '(println (map #(* 2 %) [1 2 3]))' | ./build/jank repl
```

### Making Changes

**Adding functions to WASM core:**
1. Check if function uses `cpp/` calls in `src/jank/clojure/core.jank`
2. If pure Jank, copy to `core_wasm.jank`
3. If uses `cpp/`, check if equivalent runtime function exists in `core_native.cpp`
4. Recompile: `./build/jank run --codegen wasm-aot --save-cpp --save-cpp-path build-wasm/clojure_core_generated.cpp src/jank/clojure/core_wasm.jank`
5. Test with WASM example

**Adding new namespaces for WASM:**
1. Compile: `./build/jank run --codegen wasm-aot --module-path src/jank --save-cpp --save-cpp-path build-wasm/<name>_generated.cpp src/jank/clojure/<name>.jank`
2. Update `bin/emscripten-bundle` to detect and include the module
3. Add load call in entrypoint (namespace dots become underscores: `clojure.set` → `jank_load_set()`)

**Using reader conditionals for platform-specific code:**
```clojure
;; Basic usage
(def platform-value
  #?(:wasm "WASM mode"
     :jank "Native mode"))

;; Conditional function implementations
(defn unsupported-in-wasm []
  #?(:wasm (throw "This function is not supported in WASM")
     :jank (do-native-stuff)))

;; Use :default for fallback behavior
(def feature
  #?(:wasm wasm-impl
     :default default-impl))
```

The `:wasm` feature is automatically active when compiling with `--codegen wasm-aot`.

### Common Pitfalls

1. **Function name mismatches**: `clojure.set` generates `jank_load_set()` not `jank_load_clojure_set()`
2. **Missing includes**: If compilation fails, may need to add headers in `context.cpp`
3. **Breaking native build**: Always run `./bin/compile` and native tests after changes to `core_native.cpp`
4. **Module path**: When compiling files that require other namespaces, use `--module-path src/jank`

## Claude Code Best Practices Used

### 1. Understanding Before Changing
- Read existing code (`core_native.cpp`, `core.jank`) before modifications
- Used `Grep` to find all cpp/ calls to understand scope
- Checked git status to see recent changes

### 2. Incremental Progress
- Started with simple test (eita2.jank)
- Added features incrementally (core → set → more complex examples)
- Verified each step worked before proceeding

### 3. Maintaining Compatibility
- Tested native build after each change
- Used existing patterns (function naming, module loading)
- Didn't break existing functionality

### 4. Documentation
- Created CLAUDE.md (this file) for future reference
- Created WASM_CPP_INTEROP_PLAN.md with detailed analysis
- Added comments explaining WASM-specific setup

## Tips for Future Claude Sessions

### Starting a Session
1. **Check git status** to understand current state
2. **Read CLAUDE.md** to understand project context
3. **Verify build works** before making changes
4. **Review recent commits** to see what was done

### During Development
1. **Use Read before Edit** - Never edit without reading first
2. **Test incrementally** - Don't batch many changes
3. **Check both native and WASM** - Ensure no regressions
4. **Use proper paths** - Always absolute paths for file operations

### Before Finishing
1. **Run full test suite** - `./bin/compile` for native
2. **Update documentation** - This file and any relevant READMEs
3. **Verify examples work** - Run wasm-examples to ensure functionality
4. **Clean up** - Remove temporary test files

## Known Limitations

### WASM Build Limitations
- No C++ interop (`cpp/raw`, `cpp/...` calls) - use reader conditionals
- No runtime `eval` (AOT compilation only)
- No file I/O (`slurp`, `spit`) without special setup
- ~~`:require` in `ns` form~~ **✅ NOW WORKS!** (as of Session 4)

### Workarounds
- Use reader conditionals (`#?(:wasm ... :jank ...)`) for platform-specific code
- Pre-compile all code with `--codegen wasm-aot`
- Register AOT modules as loaded with `__rt_ctx->module_loader.set_is_loaded()`
- Use JavaScript FFI for browser-specific features (future work)

## Useful Commands

```bash
# Compile WASM module
./build/jank run --codegen wasm-aot --module-path src/jank \
  --save-cpp --save-cpp-path build-wasm/output.cpp \
  input.jank

# Run WASM build
./bin/emscripten-bundle --skip-build --run wasm-examples/file.jank

# Check for cpp/ calls in a file
grep -n "cpp/" src/jank/clojure/core.jank | wc -l

# Find function in runtime
grep -r "function_name" include/cpp/jank/runtime/

# Rebuild native
./bin/compile

# Run native REPL
./build/jank repl
```

## Related Documentation

- **WASM_CPP_INTEROP_PLAN.md** - Detailed analysis of all cpp/ calls in core.jank
- **bin/emscripten-bundle** - WASM build script (well-commented)
- **wasm-examples/** - Working examples of WASM programs

## Session History

This section tracks major changes made in Claude sessions:

### Session 1 (Nov 2024) - WASM Support Implementation
- Removed cheat implementations from `core_native.cpp`
- Created `core_wasm.jank` with essential functions
- Added missing WASM headers to `context.cpp`
- Compiled and integrated `clojure.set` for WASM
- Updated `emscripten-bundle` script for auto-detection
- Verified native build compatibility
- Created comprehensive cpp/ interop analysis
- **Implemented reader conditionals with `:wasm` feature**

**Files Modified:**
- `src/cpp/clojure/core_native.cpp` - Removed map/filter/even?
- `src/cpp/jank/runtime/context.cpp` - Added WASM headers
- `bin/emscripten-bundle` - Auto-include modules, module-path support
- `src/jank/clojure/core_wasm.jank` - NEW: WASM core subset
- `src/cpp/jank/read/parse.cpp` - Added `:wasm` reader conditional support

**Files Created:**
- `CLAUDE.md` - This file
- `WASM_CPP_INTEROP_PLAN.md` - cpp/ analysis and migration plan
- `wasm-examples/test_reader_conditional.jank` - Test file for reader conditionals

**Tests Passing:**
- ✅ eita2.jank - map/filter with real clojure.core
- ✅ eita_simple.jank - sets and core functions
- ✅ Native build - no regressions
- ✅ Reader conditionals - `#?(:wasm ...)` works correctly

### Session 2 (Nov 26, 2024 AM) - Critical Codegen Bug Fixes
**Major Issues Identified and Fixed:**

1. **G__XXXXX Undefined Symbol Errors**
   - **Problem**: `cpp/value` calls (like `std::numeric_limits<jtl::i64>::min()`) generate wrapper functions with gensym names (G__12345), but the function definitions were never emitted to generated C++
   - **Root Cause**: In `src/cpp/jank/codegen/processor.cpp`, the `gen()` function for `cpp_call` expressions stored wrapper code in `expr->function_code` but never wrote it to the output
   - **Fix**: Added code to emit `function_code` to `header_buffer` with deduplication tracking
   - **Location**: `src/cpp/jank/codegen/processor.cpp:1512-1517`

2. **File Append Mode Causing Duplicate Code Generation**
   - **Problem**: Every AOT compilation appended to the output file instead of overwriting it, causing exponential duplication (33k lines → 42k lines per run)
   - **Root Cause**: `context.cpp:296` used `std::ios::app` (append mode) instead of `std::ios::trunc` (truncate/overwrite)
   - **Fix**: Changed file open mode to `std::ios::trunc` and removed stale `file_exists` check
   - **Impact**: Generated file size reduced from 42,153 lines to 8,445 lines!
   - **Location**: `src/cpp/jank/runtime/context.cpp:294-299`

3. **`refer` Function Arity Mismatch**
   - **Problem**: The `ns` macro expansion generates `(refer 'clojure.core)` with no filters, but `refer` signature was `[ns-sym & filters]` requiring varargs
   - **Root Cause**: AOT-generated C++ tried to call with arity-1 but function expected varargs with filters
   - **Fix**: Modified `refer` to detect empty filters and default to `:refer :all`
   - **Location**: `src/jank/clojure/core.jank:4373-4378`

4. **Reader Conditionals for WASM-Incompatible Functions**
   - Added `#?(:wasm ...)` guards for functions using boost or C++ features:
     - `numerator` / `denominator` (boost::multiprecision)
     - `slurp` / `spit` (file I/O)
   - **Location**: `src/jank/clojure/core.jank` (multiple locations)

**Files Modified:**
- `src/cpp/jank/codegen/processor.cpp` - Emit function_code for cpp/value wrappers (lines 1512-1517)
- `include/cpp/jank/codegen/processor.hpp` - Added `emitted_function_codes` deduplication set (line 178)
- `src/cpp/jank/runtime/context.cpp` - Fixed file mode from append to truncate (lines 294-299)
- `src/jank/clojure/core.jank` - Fixed `refer` function + reader conditionals for WASM

**Tests Passing:**
- ✅ **eita_no_require.jank** - Full WASM build with clojure.set, map, filter, even? - **WORKS PERFECTLY!**
- ✅ No more G__XXXXX undefined symbols
- ✅ No more duplicate code generation
- ✅ No more struct redefinition errors
- ✅ Native build still works with all changes

**Known Limitations:**
- `:require` in `ns` forms not fully working yet in WASM AOT (needs module loading infrastructure)
- Workaround: Use fully qualified names (e.g., `clojure.set/intersection`) as shown in `eita_no_require.jank`

### Session 3 (Nov 26, 2024 PM) - WASM Linker Errors Fixed
**Goal**: Fix all undefined symbol linker errors in WASM builds

**Issues Fixed:**

1. **Native C++ Interop Symbols**
   - **Problem**: Linker errors for `clojure::core_native::register_native_header`, `register_native_refer`, `native_header_functions`, and `compile`
   - **Solution**: Added reader conditionals to throw errors in WASM mode
   - **Location**: `src/jank/clojure/core.jank`
     - `register-native-header!` (lines 4316-4330)
     - `native-header-functions` (lines 4332-4341)
     - `compile` (lines 4520-4528)

2. **Case Macro Shift-Mask Optimization**
   - **Problem**: Linker error for `jank_shift_mask_case_integer` - optimized case dispatch not available in WASM
   - **Solution**: Disabled shift-mask optimization for WASM in macro helpers
   - **Location**: `src/jank/clojure/core.jank`
     - `prep-ints` (lines 3869-3888)
     - `prep-hashes` (lines 3934-3966)

3. **Case Usage in parse-boolean**
   - **Problem**: Even with macro fixes, `parse-boolean` function still generated shift-mask code because it was compiled before the fix
   - **Solution**: Replaced `case` with `condp` in WASM mode for `parse-boolean`
   - **Location**: `src/jank/clojure/core.jank:7774`

**Files Modified:**
- `src/jank/clojure/core.jank` - Added 5 reader conditional sections
- `WASM_FIXES_PLAN.md` - Updated to reflect Phase 1 completion

**Tests Passing:**
- ✅ **All linker errors resolved** - No more undefined symbols!
- ✅ WASM build compiles and links successfully
- ✅ `eita_no_require.jank` runs perfectly in WASM
- ✅ Native build still works with all changes
- ⚠️ `:require` in ns forms hits runtime error "Unable to find module 'clojure.set'" (AOT module loading not implemented)

**Current Status:**
- Phase 1 COMPLETED - All blocking linker errors fixed
- WASM builds work with fully qualified names
- Next: Phase 2 would add AOT module loading infrastructure for `:require` support

### Session 4 (Nov 26, 2024 PM) - `:require` Support Implemented!
**Goal**: Make `:require` in ns forms work for AOT-compiled WASM modules

**Problem**:
- When `eita.jank` used `(:require [clojure.set :as set])`, it failed with "Unable to find module 'clojure.set'"
- The issue was that AOT-compiled modules were loaded manually via `jank_load_set()` etc, but were never registered as "loaded" in the module loader
- When `:require` tried to load `clojure.set`, it didn't find it in the loaded modules list and tried to dynamically load it (which fails in WASM)

**Solution**:
- After calling each load function (e.g., `jank_load_set()`), call `__rt_ctx->module_loader.set_is_loaded("clojure.set")`
- This registers the module as loaded, so `:require` finds it already loaded and doesn't try to dynamically load it

**Files Modified:**
- `bin/emscripten-bundle` (lines 334, 340)
  - Added `__rt_ctx->module_loader.set_is_loaded("clojure.core")` after `jank_load_core()`
  - Added `__rt_ctx->module_loader.set_is_loaded("clojure.set")` after `jank_load_set()`

**Tests Passing:**
- ✅ **`:require` now works!** - `eita.jank` with `(:require [clojure.set :as set])` runs perfectly
- ✅ `set/intersection`, `set/union`, `set/difference` all work with aliased namespace
- ✅ Complete test output shows all set operations working correctly
- ✅ Native build still works

**Status**:
- **COMPLETE SUCCESS** - `:require` support fully working in WASM AOT mode!
- WASM builds now support the full ns form with `:require`
- Both fully qualified names AND aliased requires work perfectly

## Questions for Future Sessions

### Open Questions
1. How to implement `:require` in AOT compilation for cross-module references?
2. Should we add JavaScript FFI for browser APIs?
3. Should chunked sequences be added to core_wasm.jank for performance?

### Answers Found
- **Q**: How are namespace names converted to C++ function names?
  - **A**: Dots → underscores, "clojure.set" → "jank_load_set()"

- **Q**: Why do some functions work in WASM and others don't?
  - **A**: Functions using `cpp/` interop need runtime equivalents. Most sequence/math operations have them in `core_native.cpp`.

- **Q**: Can we use full core.jank in WASM?
  - **A**: No, it has ~310 cpp/ calls. Need WASM-compatible subset (core_wasm.jank) without C++ interop.

- **Q**: How to handle conditional compilation for WASM vs native builds?
  - **A**: Use reader conditionals with the `:wasm` feature. Syntax: `#?(:wasm wasm-code :jank native-code)`. The `:wasm` feature is active when using `--codegen wasm-aot`.

## Contact/Continuity

If continuing this work:
1. Read this file first to understand context
2. Check WASM_CPP_INTEROP_PLAN.md for detailed analysis
3. Test both native and WASM builds before committing
4. Update this file with new findings/changes

## Debugging Tips

### WASM Build Fails
- Check if includes are missing (add to `context.cpp`)
- Verify module load function name matches (dots → underscores)
- Ensure clojure_core_generated.cpp exists in build-wasm/

### Native Build Fails After Changes
- Check if you removed something from core_native.cpp that's still needed
- Verify function signatures match between .cpp and .hpp
- Run `./bin/compile` to see specific errors

### Generated Code Issues
- Check `build-wasm/*_generated.cpp` to see what was actually generated
- Look for `extern "C" void* jank_load_*` to find load function name
- Verify all dependencies are loaded before the module

### G__XXXXX Undefined Symbol Errors
- These are from `cpp/value` calls in Jank code that generate wrapper functions
- Check that `src/cpp/jank/codegen/processor.cpp` emits `function_code` to `header_buffer`
- Verify `emitted_function_codes` set is working for deduplication

### Duplicate Code / Redefinition Errors
- Check the file write mode in `src/cpp/jank/runtime/context.cpp` - should be `std::ios::trunc` NOT `std::ios::app`
- Delete old generated files before recompiling: `rm build-wasm/*_generated.cpp`
- Verify generated file size is reasonable (core should be ~8k lines, not 40k+)

### Arity Mismatch Errors
- AOT-generated C++ has strict arity checking
- Functions with `& args` need to handle empty args case
- Check if function has multiple arity overloads in generated code

### Runtime Errors
- Enable debug output in emscripten-bundle script
- Check browser console (for web) or terminal (for Node.js)
- Look for "undefined symbol" errors (missing function exports)

---

**Last Updated**: Nov 26, 2024
**Contributors**: Claude Code session
**Status**: WASM support functional, native build compatible
