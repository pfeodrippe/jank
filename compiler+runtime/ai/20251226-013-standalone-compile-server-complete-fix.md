# Standalone Compile Server Complete Fix

## Date: 2025-12-26

## Summary

Made the standalone compile-server fully functional for iOS JIT development. The server now:
- Compiles all native headers successfully
- Finds and loads all required libraries for JIT symbol resolution
- Loads the nREPL server native module
- Successfully compiles vybe.sdf.ios and all dependencies

## Key Changes

### 1. Added nREPL Server Native Module Loader

**File:** `src/cpp/compile_server_main.cpp`

The compile server now loads the `jank.nrepl-server.asio` native module at startup:

```cpp
// nREPL server asio native module (no header - declare extern)
extern "C" void *jank_load_jank_nrepl_server_asio();

// In compile_server_main():
jank_load_clojure_core_native();
jank_load_jank_compiler_native();
jank_load_jank_perf_native();
jank_load_jank_nrepl_server_asio();  // NEW
```

### 2. Required CLI Options

The compile server requires multiple options to work correctly with the iOS app:

```bash
./build/compile-server --target sim --port 5570 \
  --module-path /path/to/app/jank-resources/src/jank:/path/to/jank/nrepl-server/src/jank \
  --jit-lib /opt/homebrew/lib/libvulkan.dylib \
  --jit-lib /opt/homebrew/lib/libSDL3.dylib \
  --jit-lib /opt/homebrew/lib/libshaderc_shared.dylib \
  --jit-lib /path/to/app/vulkan/libsdf_deps.dylib \
  -I /path/to/app/jank-resources/include \
  -I /opt/homebrew/include \
  -I /opt/homebrew/include/SDL3 \
  -I /path/to/app \
  -I /path/to/app/vendor \
  -I /path/to/app/vendor/imgui \
  -I /path/to/app/vendor/imgui/backends \
  -I /path/to/app/vendor/flecs/distr \
  -I /path/to/app/vendor/miniaudio
```

### Key Options Explained:

1. **--module-path**: Colon-separated paths to jank source files
   - App's jank sources (e.g., vybe.sdf.math, vybe.sdf.ios)
   - jank's nrepl-server source (for jank.nrepl-server.server)

2. **--jit-lib**: Dynamic libraries loaded into JIT for symbol resolution
   - `libvulkan.dylib` - Vulkan symbols
   - `libSDL3.dylib` - SDL3 symbols
   - `libshaderc_shared.dylib` - Shader compilation symbols
   - `libsdf_deps.dylib` - ImGui (core + backends), flecs, app symbols

3. **-I**: Include paths for native header compilation
   - App headers (vulkan/sdf_engine.hpp, etc.)
   - Homebrew includes (SDL3, Vulkan)
   - Vendor includes (imgui, flecs, miniaudio)

## Issues Fixed

### Issue 1: Native headers not found
**Symptom:** `[jank-jit] Compilation FAILED for: #include <vulkan/sdf_engine.hpp>`

**Cause:** Include paths were being added AFTER `jank_init()`, but the JIT processor reads them during initialization.

**Fix:** Set `util::cli::opts.include_dirs` BEFORE calling `jank_init()`.

### Issue 2: Native symbols not found (shaderc, ImGui backends)
**Symptom:** `JIT session error: Symbols not found: [ _shaderc_compiler_initialize, _ImGui_ImplVulkan_NewFrame, ... ]`

**Cause:** Required libraries weren't loaded into JIT.

**Fix:** Added `--jit-lib` option and set `util::cli::opts.jit_libs` before `jank_init()`. Use `libsdf_deps.dylib` which contains all imgui (including backends).

### Issue 3: Module not found (vybe.sdf.math)
**Symptom:** `Unable to find module 'vybe.sdf.math'`

**Cause:** No `--module-path` option to specify jank source locations.

**Fix:** Added `--module-path` option and set `util::cli::opts.module_path` before `jank_init()`.

### Issue 4: Module not found (jank.nrepl-server.asio)
**Symptom:** `Unable to find module 'jank.nrepl-server.asio'`

**Cause:** `asio` is a native-only module (no .jank file), and it wasn't being loaded.

**Fix:** Added call to `jank_load_jank_nrepl_server_asio()` in compile server startup.

## Testing

### Test 1: iOS app initialization
```
[jank] Loading vybe.sdf.ios module...
[compile-server] ns form evaluated successfully
[compile-server] Modules to compile for iOS: 10
```

### Test 2: nREPL connectivity
```bash
nc -zv 127.0.0.1 5558
# Connection to 127.0.0.1 port 5558 [tcp/*] succeeded!
```

### Test 3: fn macro
```bash
clj-nrepl-eval -p 5558 "(def y (fn [] 42))"
# => #'user/y

clj-nrepl-eval -p 5558 "(y)"
# => 42
```

### Test 4: defn macro
```bash
clj-nrepl-eval -p 5558 "(defn greet [name] (str \"Hello, \" name \"!\"))"
# => #'user/greet

clj-nrepl-eval -p 5558 "(greet \"jank\")"
# => "Hello, jank!"
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    macOS Host                            │
│  ┌─────────────────────────────────────────────────────┐ │
│  │            Standalone compile-server                │ │
│  │  - Loads clojure.core, compiler, perf natives       │ │
│  │  - Loads nrepl-server.asio native                   │ │
│  │  - JIT compiles native headers locally              │ │
│  │  - Cross-compiles jank to iOS ARM64                 │ │
│  │  - Listens on port 5570                             │ │
│  └─────────────────────────────────────────────────────┘ │
│                           │                               │
│                           │ TCP (port 5570)               │
│                           ▼                               │
│  ┌─────────────────────────────────────────────────────┐ │
│  │              iOS Simulator                           │ │
│  │  ┌────────────────────────────────────────────────┐ │ │
│  │  │         SdfViewerMobile-JIT-Only               │ │ │
│  │  │  - AOT clojure.core, nrepl-server              │ │ │
│  │  │  - Remote compile client                       │ │ │
│  │  │  - Loads object files from compile server      │ │ │
│  │  │  - nREPL server on port 5558                   │ │ │
│  │  └────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────┘ │
│                           │                               │
│                           │ TCP (port 5558)               │
│                           ▼                               │
│  ┌─────────────────────────────────────────────────────┐ │
│  │           Developer's REPL client                    │ │
│  │  (clj-nrepl-eval, Calva, etc.)                       │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## Files Modified

- `src/cpp/compile_server_main.cpp` - Added nrepl native loader
