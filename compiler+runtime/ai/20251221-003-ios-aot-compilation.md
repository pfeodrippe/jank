# iOS AOT Compilation for jank

## Date: 2025-12-21

## Summary
Successfully implemented iOS AOT compilation for jank, allowing jank code to be compiled to C++ and cross-compiled for iOS arm64.

## Key Concepts

### WASM AOT Approach Works for iOS Too
Since iOS has the same constraints as WASM (no JIT, no LLVM at runtime), we can reuse the WASM AOT codegen. This generates pure C++ that can be compiled by any C++ compiler.

### Define JANK_TARGET_WASM and JANK_TARGET_EMSCRIPTEN for iOS
Rather than adding `JANK_TARGET_IOS` checks everywhere, we define both `JANK_TARGET_WASM=1` and `JANK_TARGET_EMSCRIPTEN=1` for iOS builds. This allows iOS to reuse all existing WASM code paths without modifying them.

```cmake
# In CMakeLists.txt for iOS:
target_compile_definitions(jank_lib PUBLIC
  JANK_TARGET_IOS=1
  JANK_TARGET_WASM=1
  JANK_TARGET_EMSCRIPTEN=1
  IMMER_HAS_LIBGC=1
  IMMER_TAGGED_NODE=0
)
```

### iOS 17.0+ Required
iOS needs std::format which is only available in iOS 16.3+. We use iOS 17.0 as the minimum deployment target to be safe.

## Files Created/Modified

### New Files
- `/compiler+runtime/cmake/toolchain-ios.cmake` - CMake toolchain for iOS cross-compilation
- `/compiler+runtime/bin/build-ios` - Script to build jank runtime for iOS
- `/compiler+runtime/bin/ios-bundle` - Script to generate core libraries (clojure.core, etc.) for iOS

### Modified Files
- `/compiler+runtime/CMakeLists.txt` - Added `jank_target_ios` option
- `/compiler+runtime/cmake/dependency/bdwgc.cmake` - Added iOS support
- `/compiler+runtime/include/cpp/jank/runtime/oref.hpp` - Fixed gnu::used attribute
- `/compiler+runtime/include/cpp/jank/runtime/core/allocator_fwd.hpp` - Fixed gnu::used attribute

## Build Process

1. Build native jank compiler (required for AOT generation):
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk \
CC=$PWD/build/llvm-install/usr/local/bin/clang \
CXX=$PWD/build/llvm-install/usr/local/bin/clang++ \
ninja -C build jank
```

2. Build jank runtime for iOS:
```bash
./bin/build-ios
```

3. Generate core libraries for iOS:
```bash
./bin/ios-bundle --skip-build
```

4. From the project using jank, use Makefile targets:
```bash
make sdf-ios      # Build all iOS components
make sdf-ios-run  # Build and run in iPad simulator
```

## Architecture Notes

### iOS uses same sources as WASM
- `c_api_wasm.cpp` - Entry point for WASM/iOS (no LLVM dependencies)
- `wasm_stub.cpp` - Stub implementations for missing functionality
- `environment_wasm.cpp` - Environment handling for WASM/iOS
- Cpptrace stub in `include/cpp/jank/util/cpptrace.hpp`

### Libraries Produced
- `libjank.a` - Main jank runtime (8.9MB)
- `libjankzip.a` - ZIP handling library
- `libgc.a` - Boehm GC library
- Core library .o files (clojure_core_generated.o, etc.)

## Bug Fix: gnu::used Attribute

The `gnu::used` attribute was incorrectly placed on extern declarations in header files. This attribute should only be on the definition:

```cpp
// WRONG - in header file:
[[gnu::visibility("default"), gnu::used]]
extern thread_local allocator *current_allocator;

// CORRECT - attribute only on definition in .cpp:
// Header:
extern thread_local allocator *current_allocator;

// arena.cpp:
[[gnu::visibility("default"), gnu::used]]
thread_local allocator *current_allocator{ nullptr };
```
