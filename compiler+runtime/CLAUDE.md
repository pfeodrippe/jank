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
- No C++ interop (`cpp/raw`, `cpp/...` calls)
- No runtime `eval` (AOT compilation only)
- No file I/O (`slurp`, `spit`) without special setup
- `:require` in `ns` form not fully working yet (use fully qualified names)

### Workarounds
- Use `core_wasm.jank` for WASM builds (subset of core)
- Pre-compile all code with `--codegen wasm-aot`
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

**Files Modified:**
- `src/cpp/clojure/core_native.cpp` - Removed map/filter/even?
- `src/cpp/jank/runtime/context.cpp` - Added WASM headers
- `bin/emscripten-bundle` - Auto-include modules, module-path support
- `src/jank/clojure/core_wasm.jank` - NEW: WASM core subset

**Files Created:**
- `CLAUDE.md` - This file
- `WASM_CPP_INTEROP_PLAN.md` - cpp/ analysis and migration plan

**Tests Passing:**
- ✅ eita2.jank - map/filter with real clojure.core
- ✅ eita_simple.jank - sets and core functions
- ✅ Native build - no regressions

## Questions for Future Sessions

### Open Questions
1. How to implement `:require` in AOT compilation for cross-module references?
2. Should we add JavaScript FFI for browser APIs?
3. What's the best way to handle conditional compilation (#?(:wasm ...))?
4. Should chunked sequences be added to core_wasm.jank for performance?

### Answers Found
- **Q**: How are namespace names converted to C++ function names?
  - **A**: Dots → underscores, "clojure.set" → "jank_load_set()"

- **Q**: Why do some functions work in WASM and others don't?
  - **A**: Functions using `cpp/` interop need runtime equivalents. Most sequence/math operations have them in `core_native.cpp`.

- **Q**: Can we use full core.jank in WASM?
  - **A**: No, it has ~310 cpp/ calls. Need WASM-compatible subset (core_wasm.jank) without C++ interop.

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

### Runtime Errors
- Enable debug output in emscripten-bundle script
- Check browser console (for web) or terminal (for Node.js)
- Look for "undefined symbol" errors (missing function exports)

---

**Last Updated**: Nov 26, 2024
**Contributors**: Claude Code session
**Status**: WASM support functional, native build compatible
