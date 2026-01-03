# jank WASM Hot-Reload: Exploration Summary (Shelved)

**Date:** November 28, 2025
**Status:** Shelved - More Complex Than Expected

---

## Goal

Enable REPL-like hot-reload for jank running in WebAssembly, allowing developers to edit functions in their editor and have them update in the browser instantly (~200ms).

## Architecture Attempted

```
                     Editor (Emacs/VS Code)
                              │
                              │ nREPL (port 7889)
                              ▼
                     ┌─────────────────┐
                     │  Node.js Proxy  │
                     │  hot_reload_server.cjs
                     └────────┬────────┘
                              │
            ┌─────────────────┼─────────────────┐
            │                 │                 │
            ▼                 ▼                 ▼
     nREPL to jank      WebSocket         HTTP Server
     (port 5555)     (port 7888)         (port 8080)
            │                 │                 │
            ▼                 ▼                 ▼
     ┌──────────────┐  ┌─────────────┐  ┌────────────┐
     │ jank nREPL   │  │   Browser   │  │ WASM Files │
     │ --server     │  │  WASM App   │  │ (eita.js/  │
     │              │  │             │  │  eita.wasm)│
     └──────────────┘  └─────────────┘  └────────────┘
```

**Flow:**
1. User edits `(defn ggg [v] (+ 49 v))` in editor
2. Editor sends code via nREPL to proxy server
3. Proxy forwards to jank's `wasm-compile-patch` op
4. jank parses code, generates C++ patch code
5. Server compiles C++ to WASM side module (~180ms)
6. Server sends WASM binary to browser via WebSocket
7. Browser loads patch via `dlopen()` (~1ms)
8. Function is hot-swapped!

## What Was Implemented

### Completed Components

1. **MAIN_MODULE Support** (`bin/emscripten-bundle`)
   - `HOT_RELOAD=1` mode enables dynamic linking
   - WASM bundles support `dlopen()` for loading patches

2. **Var Registry** (`runtime/hot_reload.hpp/.cpp`)
   - C API for loading WASM patches
   - Symbol registration and var binding
   - Works correctly in isolation

3. **wasm-compile-patch nREPL op** (`nrepl_server/ops/wasm_compile_patch.hpp`)
   - Parses jank code
   - Generates C++ code for WASM side modules
   - Returns patch metadata

4. **Hot Reload Server** (`hot_reload_server.cjs`)
   - HTTP server for static files
   - WebSocket server for browser communication
   - nREPL proxy for editor integration
   - WASM patch compilation via emcc

5. **Browser Client** (`eita_hot_reload.html`)
   - WebSocket client for receiving patches
   - `dlopen()` integration for loading patches

### Proof of Concept Results

Simple patches work:
```
=== jank WASM Hot-Reload Test ===
ggg(10) = 58 (original: 10+48)
Loading patch...
ggg(10) = 59 (patched: 10+49)
PASS - Hot-reload working!
```

## Why It's More Complex Than Expected

### 1. Symbol Resolution Across Runtimes

**Problem:** The jank server (native) and WASM runtime have different environments.

```
Editor sends: (defn ggg [v] (set/union #{v} #{1 2 3}))

jank server error: Unable to resolve symbol 'set/union'
```

The server-side jank doesn't have `clojure.set` required, but the browser-side WASM does. The two runtimes are isolated - they can't share state.

### 2. Namespace Synchronization

**Problem:** The server doesn't know the browser's current namespace.

```
[PATCH] Got C++ from jank for user/ggg  <-- wrong!
Should be: eita/ggg
```

Even with explicit `ns` parameter, the generated C++ uses the wrong qualified name because the code is analyzed in the server's context, not the browser's.

### 3. Complete Codegen Duplication

**Problem:** The `wasm_patch_processor` needs to generate C++ for ALL jank expression types.

Currently supported:
- Integer literals
- Basic arithmetic (`+`, `-`, `*`)
- Parameter references
- Simple function calls

NOT supported (would need implementation):
- `let` bindings
- `if`/`when`/`cond`
- `loop`/`recur`
- Destructuring
- Multi-arity functions
- Macros
- Java/C++ interop
- Protocols
- And many more...

This is essentially duplicating the entire jank compiler for WASM patches.

### 4. Emscripten dlsym Caching

**Problem:** Emscripten caches `dlsym()` results by symbol name, not module handle.

```cpp
// Both return the SAME function pointer (broken!)
void* handle1 = dlopen("patch_1.wasm", RTLD_NOW);
void* fn1 = dlsym(handle1, "jank_patch_symbols");

void* handle2 = dlopen("patch_2.wasm", RTLD_NOW);
void* fn2 = dlsym(handle2, "jank_patch_symbols");  // Returns fn1!
```

**Workaround implemented:** Unique symbol names per patch (`jank_patch_symbols_1`, `jank_patch_symbols_2`, etc.)

This works but adds complexity.

### 5. Large WASM Bundle Size

```
Regular build:    ~60 MB
HOT_RELOAD=1:    ~170 MB
```

The `MAIN_MODULE` requirement nearly triples bundle size due to dynamic linking support.

### 6. Two-Way State Problem

For real REPL functionality, we need:
- Server knows what vars exist in browser
- Server can call browser-side functions for macros
- Browser state survives across patches

This requires bidirectional communication that blurs the client/server boundary.

## Alternative Approaches Not Tried

### A. In-Browser Compilation (clang-interpreter in WASM)

Run clang-interpreter in the browser so compilation happens locally:
- Pro: No server-client state sync needed
- Pro: Would work offline
- Con: LLVM+Clang in WASM would be massive (~500+ MB)
- Con: Compilation would be slow in WASM
- Con: Complex to build and maintain

### B. Full Page Reload with State Serialization

On code change:
1. Serialize application state
2. Recompile entire WASM bundle
3. Reload page
4. Restore state

- Pro: Simpler architecture
- Pro: Uses existing compilation pipeline
- Con: Slow (~30-60 seconds per change)
- Con: State serialization is non-trivial

### C. Interpreter Mode (no compilation)

Run jank in interpreter mode without JIT:
- Pro: Instant code updates
- Pro: No compilation needed
- Con: Much slower execution
- Con: Would need separate interpreter implementation

## Files Created/Modified

### In jank Codebase
- `compiler+runtime/bin/emscripten-bundle` - HOT_RELOAD mode
- `compiler+runtime/include/cpp/jank/runtime/hot_reload.hpp`
- `compiler+runtime/src/cpp/jank/runtime/hot_reload.cpp`
- `compiler+runtime/include/cpp/jank/nrepl_server/ops/wasm_compile_patch.hpp`
- `compiler+runtime/src/cpp/jank/codegen/wasm_patch_processor.cpp`

### In Test Directory
- `wasm-clang-interpreter-test/hot-reload-test/hot_reload_server.cjs`
- `wasm-clang-interpreter-test/hot-reload-test/eita_hot_reload.html`
- Various documentation and test files

## Recommendation

This approach is **not recommended** for further development because:

1. **Complexity exceeds benefit**: The full solution would require essentially duplicating the jank compiler for WASM patch generation

2. **State synchronization is fundamental**: The two-runtime architecture (server + browser) creates insurmountable state sync issues

3. **Alternative approaches exist**: For development, using a fast rebuild + full page reload may be more practical

4. **Limited applicability**: Only useful for jank-in-browser use cases, which are not the primary focus

## What Worked Well

Despite shelving, some learnings:

1. **Emscripten dynamic linking works**: `dlopen()`/`dlsym()` function correctly for SIDE_MODULEs
2. **~180ms patch compilation is achievable**: emcc is fast for small side modules
3. **Var binding via `bind_root()` works**: Hot-swapping function implementations is viable
4. **WebSocket-based patch delivery works**: The browser integration is solid

These components could be useful if a better architecture emerges.

---

*Document created November 28, 2025*
*Status: Exploration complete, approach shelved*
