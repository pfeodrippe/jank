# iOS JIT-Only Mode Implementation

## Date: 2025-12-25

## Summary

Implemented JIT-only mode for iOS where:
- Only core jank libs (clojure.core, etc.) are AOT compiled
- App namespaces are loaded via remote compile server at runtime
- This enables REPL development on iOS with full `defn` support

## The Problem

When using remote compile server, evaluating `(defn foo [] 42)` failed because:
- macOS compile server didn't have the namespace context
- iOS had AOT-compiled app modules but macOS didn't know about them
- Symbol resolution failed: `Unable to resolve symbol 'sdfx/engine_initialized'`

## The Solution

Architecture: iOS should NOT have AOT app code for JIT mode. Instead:
1. iOS only has core libs (clojure.core, clojure.string, etc.)
2. All app namespaces are loaded via remote compile server
3. Both iOS and macOS build up matching namespace context

## Changes Made

### 1. ios-bundle script (`bin/ios-bundle`)

Added `--jit` flag:
```bash
ios-bundle --jit simulator    # Build core libs only for JIT mode
```

Key additions:
- `jit_mode=false` variable
- `--jit` argument parsing
- JIT-only workflow that generates `jank_aot_init.cpp` with core libs only
- Passes `jit` to `build-ios` for LLVM JIT support
- Adds `JANK_IOS_JIT=1` define to C++ compilation

### 2. JIT-only jank_aot_init.cpp

Generated when using `--jit` flag (no `--entry-module`):
```cpp
// Core libraries
jank_load_clojure_core_native();
jank_load_core();
jank_load_string();
jank_load_set();
jank_load_walk();
jank_load_template__();
jank_load_test();

// nREPL native module
jank_load_jank_nrepl_server_asio();

// App namespaces are NOT loaded here - loaded via remote compile server
```

### 3. Build script (`SdfViewerMobile/build_ios_jank_jit.sh`)

New script for JIT-only builds:
```bash
./SdfViewerMobile/build_ios_jank_jit.sh simulator
```

### 4. Makefile targets

New targets:
- `ios-jit-sim-core` - Builds JIT-only core libs
- `ios-jit-sim-core-libs` - Copies libs to Xcode project

## Output Directories

- AOT mode: `build-ios-simulator/` (all app modules)
- JIT mode: `build-ios-jit-simulator/` (core libs only)

## Libraries Produced

JIT-only mode produces:
- `libjank.a` - jank runtime with JIT support
- `libjankzip.a` - zip support
- `libgc.a` - Boehm GC
- `libjank_aot.a` - Core libs + jank_aot_init.o

## Usage

1. Build JIT-only:
   ```bash
   cd /Users/pfeodrippe/dev/something
   make ios-jit-sim-core-libs
   ```

2. Run compile-server on macOS:
   ```bash
   cd /Users/pfeodrippe/dev/jank/compiler+runtime
   ./build/compile-server
   ```

3. Configure iOS app to connect:
   ```cpp
   jank::compile_server::configure_remote_compile("host", 5570);
   jank::compile_server::connect_remote_compile();
   ```

4. App namespaces are loaded via `(require 'vybe.sdf.ui)` which:
   - iOS reads source file
   - Sends to macOS compile server via `require` protocol
   - macOS compiles all modules
   - Returns ARM64 object files
   - iOS loads them via ORC JIT

## Protocol Changes

Added to `protocol.hpp`:
- `require_request` - iOS sends namespace + source
- `require_response` - macOS returns compiled modules
- `compiled_module` - Individual module with object data

Added to `server.hpp`:
- `require_ns()` method for namespace compilation
- `loaded_namespaces_` tracking to avoid recompiling

Added to `client.hpp`:
- `require_ns()` method to send/receive

Added to `remote_compile.hpp`:
- `remote_require()` convenience function

## Next Steps

1. Test namespace loading via remote compile server
2. Test REPL eval with `defn` after namespaces are loaded
3. Verify symbol resolution works correctly
