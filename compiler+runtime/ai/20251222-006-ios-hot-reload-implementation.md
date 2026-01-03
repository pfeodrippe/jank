# iOS Hot Reload Implementation Plan

**Date**: 2025-12-22
**Status**: Implementation Complete (Core Infrastructure)
**Approach**: Native ARM64 dylib + dlopen (Approach 7 from research)

## Architecture Overview

**Key Insight**: Cross-compile to ARM64 dylib on Mac, sign it, send to device, dlopen().
This gives **native speed** (not interpreter) and reuses existing hot_reload infrastructure.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              iOS Hot Reload Architecture (Native ARM64 + dlopen)            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Mac (nREPL Server + Cross-Compiler)       iOS Device (ARM64 Runtime)       │
│  ┌────────────────────────────────┐        ┌────────────────────────────┐  │
│  │                                │        │                            │  │
│  │  1. Receive jank code          │        │  Native jank Runtime       │  │
│  │          ↓                     │        │  ├── jank_box_integer()    │  │
│  │  2. analyze_string()           │        │  ├── jank_call_var()       │  │
│  │          ↓                     │        │  ├── jank_deref_var()      │  │
│  │  3. Generate C++ (reuse        │        │  └── hot_reload_registry   │  │
│  │     wasm_patch_processor)      │        │           ↑                │  │
│  │          ↓                     │        │           │ dlopen()       │  │
│  │  4. Cross-compile ARM64:       │        │           │                │  │
│  │     clang++ -target arm64-     │ nREPL  │  ┌────────┴─────────────┐  │  │
│  │       apple-ios17.0 -shared    │ ─────────►│  patch_42.dylib      │  │  │
│  │          ↓                     │(binary)│  │  (ARM64, signed)     │  │  │
│  │  5. codesign -s - patch.dylib  │        │  │                      │  │  │
│  │     (ad-hoc sign on Mac)       │        │  │  jank_patch_foo() {  │  │  │
│  │          ↓                     │        │  │    jank_call_var()───┼──┘  │
│  │  6. Send binary via nREPL      │        │  │  }                   │     │
│  │                                │        │  └──────────────────────┘     │
│  └────────────────────────────────┘        └────────────────────────────────┘
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Why This Approach?

| Aspect | wasm3 Interpreter | Native dylib + dlopen |
|--------|-------------------|----------------------|
| Speed | ~10-50x slower | **Native** |
| Complexity | Need wasm3 library + bridge | Reuse existing hot_reload |
| Code signing | Not needed | **Signed on Mac before send** |
| Runtime integration | Host function bridge | **Direct symbol linking** |

## Implementation Status

### ✅ Completed

#### 1. Reuse wasm_patch_processor for C++ Generation
The existing `wasm_patch_processor` generates portable C++ code that works for both WASM and native targets.
The generated C++ uses:
- `extern "C"` linkage for runtime helpers
- `__attribute__((visibility("default")))` for symbol export
- Standard `patch_symbol` metadata struct

No new processor was needed - the code is platform-agnostic.

#### 2. nREPL Handler: `ios-compile-patch`
**File**: `include/cpp/jank/nrepl_server/ops/ios_compile_patch.hpp`

The handler:
1. Parses and analyzes jank code
2. Generates C++ using `wasm_patch_processor`
3. Cross-compiles to ARM64 dylib using clang++
4. Ad-hoc signs the dylib using codesign
5. Base64 encodes the binary
6. Returns response with `dylib-binary`, `symbol-name`, `patch-id`

Supports both simulator (`arm64-apple-ios17.0-simulator`) and device (`arm64-apple-ios17.0`) targets.

#### 3. iOS Hot Reload Registry
**Files**:
- `include/cpp/jank/runtime/hot_reload.hpp`
- `src/cpp/jank/runtime/hot_reload.cpp`

Added for iOS ARM64 builds:
- `hot_reload_registry::load_patch_from_bytes()` - writes dylib to /tmp, dlopen, dlsym, register symbols
- `jank_ios_hot_reload_load_patch()` - C API for iOS app to call
- `jank_truthy()` for iOS - needed by generated patch code

#### 4. Engine Integration
**File**: `include/cpp/jank/nrepl_server/engine.hpp`

Added:
- Handler declaration: `handle_ios_compile_patch()`
- Op dispatch: `if(op == "ios-compile-patch")`
- Include: `#include <jank/nrepl_server/ops/ios_compile_patch.hpp>`

## Files Changed

### New Files
1. `include/cpp/jank/nrepl_server/ops/ios_compile_patch.hpp` - nREPL handler

### Modified Files
1. `include/cpp/jank/runtime/hot_reload.hpp` - Added iOS method declarations
2. `src/cpp/jank/runtime/hot_reload.cpp` - Added iOS implementations
3. `include/cpp/jank/nrepl_server/engine.hpp` - Added iOS handler registration

## Usage

### nREPL Protocol

**Request** (op: `ios-compile-patch`):
```clojure
{:op "ios-compile-patch"
 :code "(defn my-fn [x] (+ x 42))"
 :ns "user"
 :target "device"  ; or "simulator"
 :session "..."}
```

**Response**:
```clojure
{:dylib-binary "base64-encoded-dylib..."
 :dylib-size "12345"
 :symbol-name "jank_patch_symbols_0"
 :var-name "my-fn"
 :ns-name "user"
 :patch-id "0"
 :session "..."}
```

### iOS App Integration

The iOS app should:
1. Receive the nREPL response
2. Base64 decode `dylib-binary`
3. Call `jank_ios_hot_reload_load_patch(data, size, symbol_name)`

```cpp
// In iOS app
extern "C" int jank_ios_hot_reload_load_patch(
    uint8_t const *dylib_data,
    size_t size,
    char const *symbol_name);

void handle_patch_response(std::string const &b64_dylib,
                          std::string const &symbol_name) {
    auto dylib_bytes = base64_decode(b64_dylib);
    int result = jank_ios_hot_reload_load_patch(
        dylib_bytes.data(),
        dylib_bytes.size(),
        symbol_name.c_str());

    if (result == 0) {
        std::cout << "[hot-reload] Patch loaded!" << std::endl;
    }
}
```

## Performance Expectations

| Component | Time |
|-----------|------|
| nREPL message roundtrip | ~10-50ms |
| C++ generation | ~5ms |
| ARM64 compilation | ~100-300ms |
| Dylib transfer | ~10-50ms |
| dlopen + register | ~5-20ms |
| **Total** | **~200-500ms** |

This is acceptable for development workflow (sub-second hot reload).

## Future Improvements

### Unified Eval Path
Currently, iOS hot reload uses a separate `ios-compile-patch` operation.
A future enhancement could:
1. Add session-level platform capability indication
2. Make `eval` dispatch based on platform:
   - Desktop: JIT compile locally
   - WASM: Generate WASM patch
   - iOS: Generate ARM64 dylib

### Caching
Could cache compiled patches by content hash to avoid recompilation.

### Incremental Compilation
Use Clang modules or precompiled headers to speed up compilation.
