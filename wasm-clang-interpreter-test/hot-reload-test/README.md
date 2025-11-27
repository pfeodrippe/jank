# jank WASM Hot-Reload Proof of Concept

**Status:** Working - REPL-like speed achieved!

## Summary

This demonstrates that jank can achieve Clojure REPL-like hot-reload speed in WebAssembly:

| Metric | Time |
|--------|------|
| Compile single function to WASM | ~180ms |
| Load side module (dlopen) | ~1ms |
| **Total hot-reload** | **~180ms** |

For comparison, Clojure REPL eval takes ~50-200ms depending on code complexity.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        WASM Runtime (Browser/Node)              │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    Main Module (jank)                     │  │
│  │                                                           │  │
│  │  ┌─────────────────┐    ┌──────────────────────────────┐ │  │
│  │  │   Var Registry  │    │     Runtime Functions        │ │  │
│  │  │                 │    │                              │ │  │
│  │  │  ggg_impl ──────┼────┼──> [function pointer]        │ │  │
│  │  │  hhh_impl ──────┼────┼──> [function pointer]        │ │  │
│  │  │  ...            │    │                              │ │  │
│  │  └─────────────────┘    └──────────────────────────────┘ │  │
│  │           ▲                                               │  │
│  │           │ dlopen + dlsym                                │  │
│  │           │                                               │  │
│  │  ┌────────┴────────┐                                      │  │
│  │  │  Side Modules   │  (dynamically loaded patches)        │  │
│  │  │  - ggg_v1.wasm  │  116 bytes                           │  │
│  │  │  - ggg_v2.wasm  │  116 bytes                           │  │
│  │  └─────────────────┘                                      │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              ▲
                              │ WebSocket
                              │
┌─────────────────────────────┴───────────────────────────────────┐
│                      Native Server (jank nREPL)                  │
│                                                                  │
│  User edits: (defn ggg [v] (+ v 49))                            │
│       ↓                                                          │
│  jank compiler generates C++                                     │
│       ↓                                                          │
│  emcc compiles to WASM (~180ms)                                 │
│       ↓                                                          │
│  Send patch.wasm over WebSocket                                  │
└──────────────────────────────────────────────────────────────────┘
```

## How It Works

### 1. Main Module (main.cpp)

The main jank WASM bundle has:
- A **var registry**: function pointers that can be hot-swapped
- `dlopen()` support via `-sMAIN_MODULE=2`
- Functions to load side modules and register implementations

```cpp
// Var registry
typedef int (*ggg_fn)(int);
ggg_fn ggg_impl = nullptr;

// Load new implementation
int load_module(const char* path) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    void* sym = dlsym(handle, "jank_ggg");
    ggg_impl = (ggg_fn)sym;
    return 0;
}

// Call through registry (indirect)
int call_ggg(int v) {
    return ggg_impl(v);
}
```

### 2. Side Modules (ggg_v1.cpp, ggg_v2.cpp)

Each function patch is a tiny WASM side module:

```cpp
extern "C" {
__attribute__((visibility("default")))
int jank_ggg(int v) {
    return v + 49;  // The patched implementation
}
}
```

Compiled with: `emcc -sSIDE_MODULE=1 -O2 -fPIC`

### 3. Hot-Reload Flow

1. User edits jank code in editor
2. Editor sends code to jank nREPL server (native)
3. jank compiles to C++, then emcc compiles to WASM (~180ms)
4. Server sends patch.wasm over WebSocket to browser
5. Browser calls `dlopen()` to load the side module (~1ms)
6. `dlsym()` gets the new function pointer
7. Var registry is updated to point to new implementation
8. Next call to function uses new code!

## Build Commands

```bash
# Build main module (one-time, ~18s first build)
emcc main.cpp -o main.js \
    -sMAIN_MODULE=2 \
    -sMODULARIZE -sEXPORT_NAME=createModule \
    -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,stringToNewUTF8 \
    -sEXPORTED_FUNCTIONS=_main,_load_module,_call_ggg,_malloc,_free \
    -sALLOW_TABLE_GROWTH=1 \
    -O2

# Build side module (per-function, ~180ms)
emcc ggg_v2.cpp -o ggg_v2.wasm -sSIDE_MODULE=1 -O2 -fPIC
```

## Files

- `main.cpp` - Main module with var registry and dlopen support
- `main.js` / `main.wasm` - Compiled main module (24KB WASM)
- `ggg_v1.cpp` - Version 1: `(+ v 48)`
- `ggg_v2.cpp` - Version 2: `(+ v 49)`
- `ggg_v1.wasm` / `ggg_v2.wasm` - Compiled side modules (116 bytes each)
- `test_hot_reload.cjs` - Node.js test script
- `hot_reload_demo.sh` - End-to-end demonstration

## Run the Test

```bash
./hot_reload_demo.sh
```

Expected output:
```
=== jank WASM Hot-Reload Demo ===

Step 1: User edits (defn ggg [v] (+ v 48)) to (defn ggg [v] (+ v 49))

Step 2: Compiling patched function to WASM...
   Compile time: 200ms
   WASM size: 116 bytes

Step 3: Load in WASM runtime (simulated by running test)...
call_ggg(10) = 58 (expected: 58 = 10+48)
PASS
call_ggg(10) = 59 (expected: 59 = 10+49)
PASS

CONCLUSION: REPL-like speed achieved!
```

## Integration with jank

### ✅ Step 1: MAIN_MODULE Support (COMPLETE)

**Implementation:** Added `HOT_RELOAD=1` mode to `bin/emscripten-bundle`

**Usage:**
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
HOT_RELOAD=1 ./bin/emscripten-bundle your_code.jank
```

**What it does:**
- Enables `-sMAIN_MODULE=2` for dynamic linking
- Adds `-fPIC` to all compiled objects
- Enables FS, dlopen, and other required runtime methods

### 📝 Steps 2-4: Implementation Examples

Complete working examples provided:
- `example_var_registry.cpp` - Step 2: Var registry implementation
- `example_websocket_client.js` - Step 3: Browser WebSocket client
- `example_nrepl_server.cpp` - Steps 3 & 4: nREPL server with patch compilation

See `INTEGRATION.md` for detailed implementation guide and `IMPLEMENTATION_SUMMARY.md` for complete status.

### Key Insight

We don't need clang Interpreter running in WASM! The server does the compilation and sends pre-compiled WASM patches (~180ms turnaround).

## Requirements

- Emscripten SDK 3.1.45+
- Node.js 18+ (for testing)
- jank with `HOT_RELOAD=1` build

## Documentation

- `README.md` - This file (proof of concept overview)
- `INTEGRATION.md` - Complete 4-step integration plan
- `STEP1_COMPLETE.md` - Step 1 implementation details
- `IMPLEMENTATION_SUMMARY.md` - Overall status and architecture

---

*Last Updated: Nov 27, 2025*
*Step 1 Complete - Ready for Steps 2-4 integration!*
