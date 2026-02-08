# iOS Device Hybrid AOT+JIT Mode

## Overview

iOS device JIT compilation of `clojure.core` is slow (stack-intensive, may crash due to 1MB default stack limit). This document describes the hybrid AOT+JIT mode that pre-compiles core libraries while keeping user code JIT-compiled for fast iteration.

## Problem

Pure JIT mode on iOS device:
1. Compiles `clojure.core` from source at runtime (~thousands of lines)
2. Requires large stack (8MB) - added via pthread wrapper
3. Still slow startup on device

## Solution: Hybrid AOT+JIT

Pre-compile core libraries (clojure.core, clojure.string, etc.) to native code, then only JIT-compile user application code.

### Architecture

```
┌─────────────────────────────────────────────────┐
│              iOS App Startup                    │
├─────────────────────────────────────────────────┤
│ 1. GC_init() + runtime context creation         │
│ 2. Check for AOT core libs (weak symbols)       │
│    ├── If found: load_aot_core_libs()           │
│    └── If not: JIT compile clojure.core         │
│ 3. Register nREPL native module                 │
│ 4. JIT compile vybe.sdf.ios (user app)          │
│ 5. Call -main                                   │
└─────────────────────────────────────────────────┘
```

### Implementation

**sdf_viewer_ios.mm** uses weak symbols to detect AOT core libs:

```cpp
extern "C" __attribute__((weak)) void* jank_load_core();
extern "C" __attribute__((weak)) void* jank_load_string();
// ... etc

static bool has_aot_core_libs() {
    return jank_load_core != nullptr;
}
```

If the AOT .o files are linked into the app, these symbols will resolve. Otherwise they're null and pure JIT mode is used.

### Build Process

1. Build native jank compiler (macOS)
2. Build jank device JIT libraries: `make ios-jit-device`
3. (Optional) Build AOT core libs: `make ios-jit-device-core-aot`
4. Generate Xcode project: `make ios-jit-device-project`
5. Build and run: `make ios-jit-device-run`

For hybrid mode, the AOT core lib .o files need to be added to the Xcode project.

### Files Modified

| File | Change |
|------|--------|
| `SdfViewerMobile/sdf_viewer_ios.mm` | Added weak symbols for AOT core libs, `has_aot_core_libs()`, `load_aot_core_libs()` |
| `Makefile` | Updated `ios-jit-device-core-aot` target, simplified `jank_aot_init.cpp` stub |

### Related Fixes (from previous work)

1. **Anonymous namespace issue**: Fixed undefined symbol `(anonymous namespace)::init_jank_runtime_impl()` by moving helpers out of anonymous namespace
2. **Stack overflow**: Added 8MB pthread wrapper for jank init
3. **Target triple**: Conditional `arm64-apple-ios17.0` vs `arm64-apple-ios17.0-simulator`
4. **Clang headers**: Added `/clang/include` before SDK headers for `size_t`/`ptrdiff_t`

## Usage

### Pure JIT Mode (Default)
```bash
make ios-jit-device
make ios-jit-device-run
```

### Hybrid AOT+JIT Mode (Faster Startup)
```bash
make ios-jit-device
make ios-jit-device-core-aot
# Add .o files from SdfViewerMobile/build-iphoneos-jit/core-aot/ to Xcode project
make ios-jit-device-run
```

## Notes

- AOT core libs are ~50MB of object files (clojure.core alone is huge)
- Hybrid mode significantly reduces startup time
- User code changes still get JIT-compiled immediately
- This is development-only - production apps should use full AOT
