# iOS AOT Cross-Compilation Plan

## Overview

This document outlines the plan to enable jank's AOT compilation to target iOS (arm64-apple-ios15.0) so that the same jank codebase used for desktop (via `make sdf-clean`) can be used for iOS apps without relying on weak symbol stubs.

## Key Insight: Follow the WASM Pattern!

jank already supports WASM cross-compilation without JIT. The pattern is:

1. **Native jank runs on host (macOS)** to generate C++ code with `--codegen wasm-aot`
2. **C++ code is saved** (not JIT compiled) - includes jank runtime headers and native headers
3. **Emscripten cross-compiles** the generated C++ + runtime (`libjank.a`) to WASM
4. **Separate runtime build**: `c_api_wasm.cpp` provides runtime without LLVM/JIT dependencies

**For iOS, we follow the exact same pattern:**
1. **Native jank runs on macOS** to generate C++ code with `--codegen ios-aot` (or reuse `cpp`)
2. **C++ code is saved** including `vulkan/sdf_engine.hpp` and other native headers
3. **Xcode/clang cross-compiles** the generated C++ + runtime for iOS arm64
4. **iOS runtime build**: `libjank.a` built with `-Djank_target_ios=ON`

The WASM script `bin/emscripten-bundle` shows exactly how this works!

## Current State Analysis

### Desktop AOT Flow (`make sdf-clean`)

1. **Source**: `vybe.sdf` requires `["vulkan/sdf_engine.hpp" :as sdfx :scope "sdfx"]`
2. **Compilation**: jank compiles modules to `.o` files for host platform (macOS x86_64/arm64)
3. **Code Generation**: Direct C++ interop calls like `(sdfx/init shader-dir)` become `sdfx::init(...)`
4. **Linking**: clang links `.o` files with:
   - `libjank-standalone.a` (jank runtime)
   - `libsdf_deps.dylib` (ImGui, Flecs, stb, miniaudio)
   - System libraries (SDL3, Vulkan, shaderc)

### WASM AOT Flow (bin/emscripten-bundle)

1. **Native jank** generates C++ with `--codegen wasm-aot --save-cpp`
2. **Context.cpp skips JIT**: `if(util::cli::opts.codegen == util::cli::codegen_type::wasm_aot) { skip JIT }`
3. **Emscripten builds runtime**: `-Djank_target_wasm=ON` builds `libjank.a` without LLVM
4. **em++ links everything**: generated C++ + libjank.a + libgc.a → .wasm

### Current iOS Approach (Weak Stubs - Problematic)

1. **Source**: Separate `vybe_sdf_ios.jank` file using `cpp/raw` with weak symbols
2. **Problem**: No proper C++ header interop - can't use `["vulkan/sdf_engine.hpp" :as sdfx]`
3. **Problem**: Maintaining two codebases (desktop + iOS)
4. **Problem**: Weak symbols are overridden at link time by `sdf_viewer_ios.mm`

### Why Weak Stubs Are Used

The weak stubs exist because:
1. jank's JIT can't process iOS SDK headers (it runs on macOS host)
2. jank's AOT uses host target triple, not iOS target
3. The module `.o` files are compiled for macOS, not iOS arm64

## Goal

Enable the **same jank source code** (e.g., `vybe.sdf`) to be AOT-compiled for iOS, with proper C++ header interop.

## Technical Challenges

### Challenge 1: Cross-Compilation Target Triple

**Current**: jank uses `llvm::sys::getDefaultTargetTriple()` which returns host triple
**Needed**: Support for `--target arm64-apple-ios15.0` flag

**Files to Modify**:
- `src/cpp/jank/util/cli.cpp` - Add `--target` CLI option
- `src/cpp/jank/util/clang.cpp` - `default_target_triple()` should check CLI option
- `src/cpp/jank/runtime/context.cpp` - Use target triple for LLVM target machine
- `src/cpp/jank/aot/processor.cpp` - Pass target to clang driver

### Challenge 2: iOS SDK Sysroot

**Current**: Uses macOS SDK automatically
**Needed**: `--sysroot $(xcrun --sdk iphoneos --show-sdk-path)`

**Files to Modify**:
- `src/cpp/jank/util/cli.cpp` - Add `--sysroot` CLI option
- `src/cpp/jank/util/clang.cpp` - Pass sysroot to clang invocations
- `src/cpp/jank/aot/processor.cpp` - Pass sysroot to final link

### Challenge 3: JIT vs AOT Header Processing

**Problem**: During AOT compilation, jank still uses JIT to:
1. Parse and analyze jank source
2. Load modules and resolve C++ headers
3. The JIT uses host clang which can't process iOS headers

**Solution Options**:

#### Option A: Separate Header Processing
- For cross-compilation, skip JIT compilation of header-dependent code
- Generate stub declarations that will be resolved at link time
- Use `--save-cpp` to generate C++ source, then cross-compile separately

#### Option B: Two-Stage Compilation
1. Stage 1: On macOS, generate LLVM IR with abstract C++ calls
2. Stage 2: On macOS (or CI), compile LLVM IR to iOS arm64 object files

#### Option C: Header Abstraction Layer (Recommended)
- Create a common C API for native functions (like current weak stubs, but proper)
- jank code calls C functions, not C++ templates
- C functions are implemented in `.cpp` files that include iOS-specific headers
- This is what `vybe_sdf_ios.jank` already does, but cleaner

### Challenge 4: iOS Runtime Library

**Current**: `libjank-standalone.a` is built for macOS
**Needed**: Cross-compile jank runtime for iOS

**CMake Changes Required**:
```cmake
# New iOS toolchain file: cmake/toolchain-ios.cmake
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
set(CMAKE_OSX_SYSROOT iphoneos)
```

**Build Command**:
```bash
cmake -B build-ios -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ios.cmake \
  -Djank_target_ios=ON
ninja -C build-ios libjank-standalone.a
```

### Challenge 5: Third-Party Dependencies for iOS

Need iOS versions of:
- BDWGC (Boehm GC) - Needs iOS build
- folly - May need modifications for iOS
- CppInterOp - **Critical**: Might not work on iOS without JIT

**CppInterOp Issue**:
- CppInterOp uses Clang's JIT features
- iOS doesn't support JIT due to code signing
- **Solution**: For iOS AOT, use C codegen (`--codegen cpp`) instead of LLVM IR

## Recommended Implementation Strategy

### Phase 0: Quick Win - Use WASM Codegen Today! (Immediate)

**The WASM codegen already works for iOS!** We just need to:

1. **Generate C++ with existing `--codegen wasm-aot`**:
```bash
# On macOS, use native jank to generate C++
cd ~/dev/something
./path/to/jank \
  --module-path src \
  --codegen wasm-aot \
  --save-cpp \
  --save-cpp-path SdfViewerMobile/generated/vybe_sdf_generated.cpp \
  -I vulkan \
  -I vendor/imgui \
  compile-module vybe.sdf
```

2. **Build iOS runtime** (one-time setup):
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# Create iOS toolchain file (similar to WASM approach)
cmake -B build-ios -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ios.cmake \
  -Djank_target_ios=ON

ninja -C build-ios libjank.a
```

3. **Cross-compile generated C++ for iOS** (in Xcode):
   - Add `vybe_sdf_generated.cpp` to Xcode project
   - Add jank include paths (`include/cpp`, `third-party/*`)
   - Link with `libjank.a` (iOS build)

**Why this works**: The WASM codegen generates pure C++ that:
- Includes jank headers (not LLVM/JIT)
- Includes native headers from `:require` directives
- Can be compiled by any C++ compiler (including iOS clang)

### Phase 1: Create iOS Build Infrastructure

**Goal**: Formalize the iOS build like WASM has `bin/emscripten-bundle`

1. **Create `cmake/toolchain-ios.cmake`**:
```cmake
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
set(CMAKE_OSX_SYSROOT iphoneos)

# Use system clang for iOS cross-compilation
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
```

2. **Add `-Djank_target_ios=ON`** option in CMakeLists.txt:
```cmake
option(jank_target_ios "Build for iOS (arm64-apple-ios)" OFF)

if(jank_target_ios)
  target_compile_definitions(jank_lib PUBLIC JANK_TARGET_IOS=1)
  # Disable JIT/LLVM features
  # Use c_api_ios.cpp (or reuse c_api_wasm.cpp)
endif()
```

3. **Create `bin/ios-bundle` script** (like `bin/emscripten-bundle`):
```bash
#!/bin/bash
# Generate C++ with native jank
./build/jank --codegen wasm-aot --save-cpp --save-cpp-path "$OUTPUT" compile-module "$MODULE"

# Cross-compile for iOS
xcrun -sdk iphoneos clang++ \
  -target arm64-apple-ios15.0 \
  -isysroot $(xcrun --sdk iphoneos --show-sdk-path) \
  -I include/cpp \
  -I third-party/immer \
  -c "$OUTPUT" -o "$OUTPUT.o"
```

### Phase 2: Rename/Add Codegen Type (Optional Polish)

1. Add `--codegen ios-aot` as alias for `wasm-aot` (both skip JIT, generate pure C++)
2. Or rename `wasm-aot` to `cpp-aot` since it's not WASM-specific

### Phase 3: Full Integration (Long Term)

1. Build BDWGC for iOS (already done for WASM, similar process)
2. Integrate with Xcode project generation (xcodegen)
3. Create iOS-specific `c_api_ios.cpp` (or verify `c_api_wasm.cpp` works for iOS)

## Tested Working Approach

### Success: Simple Modules Work with WASM AOT

Tested and confirmed working:
```bash
./build/jank \
  --module-path /tmp/jank_test \
  --codegen wasm-aot \
  --save-cpp \
  --save-cpp-path /tmp/test_simple.cpp \
  compile-module test.simple

# Output:
# [jank] Saved generated C++ to: /tmp/test_simple.cpp
# [jank] WASM AOT mode: skipping JIT compilation
```

The generated C++:
- Includes jank runtime headers
- Generates proper jank_load_* functions
- Can be compiled by any C++ compiler (including iOS clang)

### Challenge: Native Headers Require JIT Library Loading

For modules with native headers (like `vybe.sdf`), the WASM AOT mode:
1. **Still needs to JIT-compile native headers** for type information
2. **Requires native libraries loaded into JIT** for symbol resolution
3. Once libraries are loaded, C++ generation works

Example command that progresses through modules:
```bash
./build/jank \
  --module-path src \
  --codegen wasm-aot \
  --save-cpp \
  --save-cpp-path /tmp/test_vybe_sdf.cpp \
  -I . \
  -I vendor/imgui -I vendor/imgui/backends \
  -I vendor/flecs/distr -I vendor \
  -I /opt/homebrew/include -I /opt/homebrew/include/SDL3 \
  -L /opt/homebrew/lib \
  --jit-lib /opt/homebrew/lib/libvulkan.dylib \
  --jit-lib /opt/homebrew/lib/libSDL3.dylib \
  --jit-lib /opt/homebrew/lib/libshaderc_shared.dylib \
  compile-module vybe.sdf
```

This successfully compiles several modules (vybe.sdf.math, vybe.util, vybe.sdf.shader partial) before hitting additional library dependencies.

### Key Insight

The WASM approach proves that:
1. **Header parsing happens on HOST (macOS)** - not the target
2. **Libraries are needed for JIT symbol resolution** - on the host
3. **Generated C++ is target-independent** - can be cross-compiled

For iOS, we just need to:
1. Provide all native libraries for macOS JIT during C++ generation
2. Cross-compile the generated C++ for iOS arm64
3. Link with iOS-built runtime and frameworks

## Immediate Action Items

### For ~/dev/something iOS Project

**Option 1: Use Desktop-Generated C++ (Recommended)**

1. Run `make sdf` or similar on desktop to verify jank code works
2. Use the already-generated C++ from the JIT cache
3. Cross-compile that C++ for iOS:
```bash
# The generated C++ is in ~/.jank/<version>/cache/
# Or use --save-cpp to save it explicitly
xcrun -sdk iphoneos clang++ \
  -target arm64-apple-ios15.0 \
  -I /Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp \
  -I /Users/pfeodrippe/dev/jank/compiler+runtime/third-party/immer \
  -c generated_module.cpp -o generated_module.o
```

**Option 2: Full WASM AOT for iOS (More Work)**

Requires adding all native libraries as `--jit-lib` to the compile-module command

## CLI Changes Required in jank

```cpp
// src/cpp/jank/util/cli.cpp additions

// New option
cli.add_option("--target",
               opts.target_triple,
               "Target triple for cross-compilation (e.g., arm64-apple-ios15.0)");

cli.add_option("--sysroot",
               opts.sysroot,
               "System root for cross-compilation SDK");

// In options struct
struct options {
  std::string target_triple;  // Empty = use host
  std::string sysroot;        // Empty = use default
  // ...
};
```

```cpp
// src/cpp/jank/util/clang.cpp modifications

jtl::immutable_string default_target_triple()
{
  // If cross-compilation target is specified, use it
  if(!util::cli::opts.target_triple.empty())
  {
    return util::cli::opts.target_triple;
  }

  // Otherwise use host triple (current behavior)
  // ...
}
```

## Testing Plan

1. **Unit Tests**: Add tests for CLI option parsing
2. **Integration Test**:
   - Simple jank program with C interop
   - Compile with `--target arm64-apple-ios15.0`
   - Verify generated object file is arm64
3. **End-to-End Test**:
   - Build SdfViewerMobile with jank AOT code
   - Run on iOS Simulator
   - Verify rendering works

## Dependencies

- LLVM must be built with ARM target support (should be by default)
- iOS SDK via Xcode Command Line Tools
- MoltenVK and SDL3 xcframeworks for iOS

## Timeline Estimate

- Phase 1 (C API Abstraction): 2-3 days
- Phase 2 (Cross-Compilation): 1-2 weeks
- Phase 3 (Full iOS AOT): 2-4 weeks

## Open Questions

1. Should we support iOS Simulator (x86_64) as well as device (arm64)?
2. How to handle code signing for AOT-compiled binaries?
3. Should the iOS runtime include nREPL server support? (Probably no for App Store)

## References

- [LLVM Cross-Compilation](https://llvm.org/docs/HowToCrossCompileLLVM.html)
- [CMake iOS Toolchain](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling-for-ios-tvos-or-watchos)
- [MoltenVK iOS Setup](https://github.com/KhronosGroup/MoltenVK)
