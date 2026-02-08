# jank WebAssembly (WASM) Support Status

## Overview

jank is working towards WebAssembly support to enable running Clojure code in browsers and other WASM runtimes. This document tracks the current status, what works, and what's needed.

## Current Status: 🟢 Runtime + Reader Working!

The WASM build compiles and **runs successfully**! The runtime initializes, GC works, `clojure.core-native` functions are loaded, and **jank source code can be parsed**!

**What's working:**
```
[jank-wasm] jank WebAssembly Runtime
[jank-wasm] Module: minimal

[jank-wasm] Calling jank_init...
[jank-wasm] Inside jank_main, runtime context should be initialized
[jank-wasm] Loading clojure.core-native...
[jank-wasm] Core native loaded!

[jank-wasm] Evaluating jank source...
[jank-wasm] Source:
;; Minimal WASM test - just tests basic runtime without clojure.core
;; This should work as soon as the runtime initializes

42

[jank-wasm] Result: 42

[jank-wasm] Done!
```

## What Works ✅

- **CMake configuration** - `emcmake cmake -Djank_target_wasm=on` succeeds
- **Library compilation** - `libjank.a`, `libjankzip.a`, and `libgc.a` build for WASM
- **Linking** - Successfully links into `.js`/`.wasm` files
- **Runtime initialization** - `jank_init()` creates the runtime context
- **GC** - Boehm GC works in WASM mode
- **C API** - WASM-specific C API (`c_api_wasm.cpp`) with no LLVM deps
- **Core native functions** - `clojure.core-native` loads and registers functions
- **emscripten-bundle script** - Automates build, link, and run
- **Lexer/Parser** - Can read jank source code and produce data structures
- **jank_eval_string_c** - C API to evaluate/parse jank strings and return results

## What's Missing / Next Steps ⏳

### 1. AOT C++ Compilation Pipeline (IN PROGRESS ✅)

The `--codegen cpp` mode works! The `--save-cpp-path` flag has been added to save generated C++ to files:

```bash
# Generate C++ and save to file
$ ./build/jank --codegen cpp --save-cpp-path /tmp/output.cpp run myfile.jank

# Compile clojure.core to C++ (clear cache first!)
$ rm -rf build/classes build/core-libs ~/.cache/jank
$ ./build/jank --codegen cpp --save-cpp-path /tmp/clojure_core.cpp compile-module clojure.core
```

**Recent Progress:**
- ✅ Added `--save-cpp` and `--save-cpp-path` CLI flags
- ✅ Tested that generated C++ compiles with emscripten
- ✅ Successfully linked generated code with libjank.a for WASM
- ✅ **FIXED: loop/recur codegen bug** - Applied changes from PR #598

**What's needed:**
- [x] ~~Fix codegen bug with loop compilation~~ DONE!
- [ ] Generate proper header includes in output
- [ ] Integrate C++ generation into emscripten-bundle
- [ ] Compile generated C++ with em++ instead of JIT
- [ ] Link with libjank.a for WASM

### 2. Pre-compiled clojure.core (IN PROGRESS)

Currently only `clojure.core-native` (C++ native functions) is loaded in WASM.
The jank implementation of `clojure.core` needs to be pre-compiled.

**Solution:** AOT compile clojure.core to C++ using `--codegen cpp --save-cpp-path` and link it in.

The generated code structure is compatible with WASM - it extends `jit_function` which exists in libjank.a:
```cpp
namespace clojure::core {
  struct clojure_core_my_fn : jank::runtime::obj::jit_function {
    clojure_core_my_fn()
      : jank::runtime::obj::jit_function{ /* metadata */ }
    {}
    
    jank::runtime::object_ref call(jank::runtime::object_ref const x) final {
      // ... function body
    }
  };
}
```

### 3. Browser I/O

`println` and other I/O functions need browser-specific implementations (console.log, etc.).

## Implementation Plan

### Phase 1: Basic Runtime ✅ DONE

1. ✅ CMake WASM configuration
2. ✅ Build libjank.a for WASM
3. ✅ Create `c_api_wasm.cpp` - WASM-compatible C API
4. ✅ Add guards to `clojure/core_native.cpp` for JIT functions
5. ✅ Create GC and CLI stubs
6. ✅ Refactor `context.hpp/cpp` - Guard `analyze::processor` and `jit::processor`
7. ✅ Guard `parse.cpp` - Skip `is_special` check in WASM
8. ✅ Update `emscripten-bundle` script with exception support
9. ✅ Successfully link and run a WASM binary
10. ✅ Fix `wasm_native_stubs.cpp` - Use proper CLI `options` struct
11. ✅ Add `jank_eval_string_c` - C API for parsing/evaluating jank strings
12. ✅ Embed jank source in generated entry point
13. ✅ Test basic parsing: `42` → `"42"`

### Phase 2: Core Library (IN PROGRESS)

10. [x] Add `--save-cpp` and `--save-cpp-path` CLI flags
11. [x] Fix codegen bug with loop/recur compilation (applied PR #598 changes)
12. [ ] Pre-compile `clojure.core` to C++ using `--codegen cpp --save-cpp-path`
13. [ ] Embed compiled core in WASM binary
14. [ ] Static module registration
15. [ ] Test basic clojure.core functions

### Phase 3: User Code

14. [ ] Pre-compile user jank files to C++
15. [ ] Link user code with runtime
16. [ ] End-to-end test: jank source → WASM → browser

### Phase 4: Polish

17. [ ] Browser-specific I/O (console.log for println, etc.)
18. [ ] Integration tests with Node.js
19. [ ] Documentation and examples

## Building WASM

```bash
# Prerequisites: Install Emscripten
brew install emscripten  # or follow https://emscripten.org/docs/getting_started

# Configure and build
cd compiler+runtime
./bin/emscripten-bundle

# Build and run a jank file
./bin/emscripten-bundle --run wasm-examples/eita.jank

# Skip rebuild if already built
./bin/emscripten-bundle --skip-build --run wasm-examples/eita.jank
```

## Architecture Notes

### No JIT in WASM

WASM doesn't support generating and executing code at runtime (no `dlopen`, no RWX pages). All code must be compiled ahead-of-time on the host and linked into the WASM binary.

### Memory Management

BDWGC (Boehm GC) has a WASM mode that works with single-threaded, cooperative stack switching. This is already configured in the build.

### File System

Emscripten provides a virtual file system. For a minimal build, we may not include it. Source files should be pre-compiled, not loaded at runtime.

## Files Changed/Added

- `CMakeLists.txt` - Added WASM-specific source files and `JANK_TARGET_WASM` define
- `include/cpp/jank/runtime/context.hpp` - Guarded `an_prc` and `jit_prc` members
- `src/cpp/jank/runtime/context.cpp` - Guarded analyzer/JIT includes and usage
- `src/cpp/jank/read/parse.cpp` - Guarded `is_special` check
- `src/cpp/jank/c_api_wasm.cpp` - WASM C API without LLVM deps (new)
  - Added `jank_eval_string_c()` - Parse jank strings and return result
- `src/cpp/jank/gc_wasm_stub.cpp` - `GC_throw_bad_alloc` stub (new)
- `src/cpp/jank/wasm_native_stubs.cpp` - CLI opts stub using proper `options` struct (fixed)
- `src/cpp/jank/util/environment_wasm.cpp` - WASM environment stubs (new)
- `src/cpp/clojure/core_native.cpp` - Added `#ifndef JANK_TARGET_WASM` guards
- `include/cpp/jank/runtime/convert/builtin.hpp` - Added WASM guard for big_integer
- `bin/emscripten-bundle` - Build automation script with `-fexceptions` support
  - Embeds jank source in generated entry point using raw string literals
  - Calls `jank_eval_string_c()` to parse and display results
- `wasm-examples/minimal.jank` - Minimal test file (new)
- `wasm-examples/eita.jank` - Test file with clojure.core usage (existing)

## Architecture Notes

### No JIT in WASM

WASM doesn't support generating and executing code at runtime (no `dlopen`, no RWX pages). All code must be compiled ahead-of-time on the host and linked into the WASM binary.

### Memory Management

BDWGC (Boehm GC) has a WASM mode that works with single-threaded, cooperative stack switching. This is configured in the CMake build.

### File System

Emscripten provides a virtual file system. For the minimal build, we don't include it. Source files should be pre-compiled, not loaded at runtime.

## References

- [Emscripten Documentation](https://emscripten.org/docs/)
- [BDWGC Emscripten Support](https://github.com/nickmessing/bdwgc/blob/master/docs/platforms/README.emscripten)
