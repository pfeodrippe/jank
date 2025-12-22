# iOS JIT Full Implementation

**Date**: 2025-12-22
**Status**: WORKING - jank iOS JIT runtime initializes successfully on iOS simulator!

## Summary

This document describes the full implementation of iOS JIT support, including all the fixes required to get the jank JIT runtime building, linking, and initializing on iOS simulator.

## Key Achievements

1. **iOS JIT library builds successfully** (284MB libjank.a)
2. **iOS JIT app links successfully** (with 220MB libllvm_merged.a)
3. **iOS JIT runtime initializes successfully** - GC, context, and JIT processor all work!

## Key Changes

### 1. CMakeLists.txt Changes

#### Source Selection for iOS JIT (line 775)
Changed from:
```cmake
elseif(jank_target_ios)
```
To:
```cmake
elseif(jank_target_ios AND NOT jank_ios_jit)
```
This allows iOS JIT builds to use the full native build sources instead of WASM-like stubs.

#### iOS JIT-specific Sources (lines 888-898)
Added iOS JIT-specific stubs:
```cmake
if(jank_ios_jit)
  list(APPEND jank_lib_sources
    src/cpp/jank/util/environment_wasm.cpp
    src/cpp/jank/runtime/perf_wasm.cpp
    src/cpp/jank/nrepl_server/native_header_stubs_ios.cpp
    src/cpp/jank/gc_wasm_stub.cpp
    # LLVM RTTI stub (LLVM is built without RTTI, jank needs RTTI)
    src/cpp/jank/llvm_rtti_stub.cpp
  )
endif()
```

#### FTXUI Exclusion (lines 874-885)
Excluded FTXUI-based files for iOS JIT (not available on iOS):
```cmake
if(NOT jank_ios_jit)
  list(APPEND jank_lib_sources
    src/cpp/jank/ui/highlight.cpp
    src/cpp/jank/error/report.cpp
    src/cpp/jank/util/clang.cpp
  )
else()
  # iOS JIT: Use iOS-specific clang stubs (no external clang executable)
  list(APPEND jank_lib_sources
    src/cpp/jank/util/clang_ios.cpp
  )
endif()
```

#### Folly Build for iOS JIT (line 1234)
Changed folly build condition:
```cmake
if(NOT jank_target_wasm AND (NOT jank_target_ios OR jank_ios_jit))
```

#### Link folly for iOS JIT (lines 1138-1141)
```cmake
if(jank_ios_jit)
  target_link_libraries(jank_lib PRIVATE folly_lib)
endif()
```

### 2. Source Code Changes

#### sha256.cpp - CommonCrypto for iOS
Used CommonCrypto instead of OpenSSL for SHA256 on iOS:
```cpp
#if defined(JANK_TARGET_IOS)
  #include <CommonCrypto/CommonDigest.h>
#else
  #include <openssl/sha.h>
#endif
```

#### jit/processor.cpp - Disable Perf Profiling on iOS
Added iOS guard around perf profiling code (which uses unavailable LLVM plugins):
```cpp
#if !defined(JANK_TARGET_IOS)
    if(util::cli::opts.perf_profiling_enabled)
    {
      // ... perf profiling code
    }
#endif
```

#### jit/processor.cpp - Skip PCH Build on iOS (lines 171-197)
Made PCH optional for iOS (cannot build PCH at runtime on iOS):
```cpp
#if !defined(JANK_TARGET_IOS)
    /* On desktop, build PCH on demand if not found. */
    auto pch_path{ util::find_pch(binary_version) };
    if(pch_path.is_none())
    {
      auto const res{ util::build_pch(args, binary_version) };
      if(res.is_err())
      {
        throw res.expect_err();
      }
      pch_path = res.expect_ok();
    }
    auto const &pch_path_str{ pch_path.unwrap() };
    args.emplace_back("-include-pch");
    args.emplace_back(strdup(pch_path_str.c_str()));
#else
    /* On iOS, PCH is optional - if it exists (pre-bundled), use it.
     * Otherwise skip PCH and let JIT compile headers on demand. */
    auto pch_path{ util::find_pch(binary_version) };
    if(pch_path.is_some())
    {
      auto const &pch_path_str{ pch_path.unwrap() };
      args.emplace_back("-include-pch");
      args.emplace_back(strdup(pch_path_str.c_str()));
    }
#endif
```

### 3. New Files Created

#### clang_ios.cpp
iOS-specific stubs for clang utilities (no external clang executable on iOS):
```cpp
namespace jank::util
{
  jtl::option<jtl::immutable_string> find_clang()
  {
    // On iOS, clang is embedded as libraries - return a placeholder path
    static jtl::immutable_string result{ "/embedded/clang++" };
    return result;
  }

  jtl::option<jtl::immutable_string> find_clang_resource_dir()
  {
    // Resource dir should be bundled in the app at runtime
    static jtl::immutable_string result;
    if(result.empty())
    {
      result = jtl::immutable_string{ resource_dir() } + "/clang";
    }
    return result;
  }

  jtl::result<void, error_ref> invoke_clang(std::vector<char const *>)
  {
    return error::system_failure("External clang invocation not supported on iOS");
  }

  jtl::option<jtl::immutable_string> find_pch(jtl::immutable_string const &)
  {
    return jtl::none;
  }

  jtl::result<jtl::immutable_string, error_ref>
  build_pch(std::vector<char const *>, jtl::immutable_string const &)
  {
    return error::system_failure("PCH building not supported on iOS - PCH must be pre-bundled");
  }

  jtl::immutable_string default_target_triple()
  {
    return "arm64-apple-ios17.0-simulator";
  }
}
```

#### llvm_rtti_stub.cpp
RTTI stub for iOS JIT (LLVM is built without RTTI but jank needs RTTI):
```cpp
#if defined(JANK_IOS_JIT)

#include <llvm/Support/Error.h>

namespace
{
  class ForceRTTI : public llvm::ErrorInfo<ForceRTTI>
  {
  public:
    static char ID;
    void log(llvm::raw_ostream &) const override {}
    std::error_code convertToErrorCode() const override { return std::error_code(); }
  };
  char ForceRTTI::ID = 0;
}

// Explicit instantiation to force typeinfo emission
template class llvm::ErrorInfo<llvm::ErrorList, llvm::ErrorInfoBase>;

#endif
```

#### native_header_stubs_ios.cpp
Stubs for nrepl_server functions that require Boost Asio (not available on iOS):
```cpp
namespace jank::nrepl_server::asio
{
  bool is_native_header_macro(runtime::ns::native_alias const &, std::string const &)
  {
    return false;
  }

  bool is_native_header_function_like_macro(runtime::ns::native_alias const &, std::string const &)
  {
    return false;
  }
}
```

## iOS App Configuration

### marching_cubes.hpp Fix
Fixed tinygltf duplicate symbols for iOS JIT:
```cpp
#if defined(SDF_ENGINE_IMPLEMENTATION) || (!defined(SDF_AOT_BUILD) && !defined(JANK_IOS_JIT))
#define TINYGLTF_IMPLEMENTATION
#endif
```

### project-jit.yml Updates
1. Added jank_aot_init.cpp stub source
2. Added `-lfolly` to linker flags
3. Configured JIT-specific build directory
4. Added `CODE_SIGN_ALLOW_ENTITLEMENTS_MODIFICATION: YES`

### build_ios_jit.sh Updates
1. Added libfolly.a to library copying

## Library Sizes (iOS Simulator JIT)
- libjank.a: 284MB
- libfolly.a: 2.9MB
- libllvm_merged.a: 220MB
- libgc.a: 766KB
- libjankzip.a: 398KB

## What Works
- Full jank JIT library builds for iOS simulator (arm64)
- Folly SharedMutex and synchronization primitives
- SHA256 using CommonCrypto
- JIT processor initialization (without PCH, without perf profiling)
- Stub implementations for missing desktop features
- **Runtime context creation!**
- **CppInterOp interpreter creation!**

## What's Excluded for iOS JIT
- Boost Asio (nrepl_server uses stubs for native header completion)
- FTXUI (terminal UI)
- cpptrace (stack traces)
- Perf profiling (Linux-specific LLVM plugins)
- PCH building at runtime (must be pre-bundled if needed)
- External clang invocation

## Runtime Verification

Successful output from iOS simulator:
```
============================================
   SDF Viewer Mobile - iOS Edition
   (with jank AOT runtime)
============================================

[jank] Initializing Boehm GC...
[jank] Creating runtime context...
[jank] JIT mode - skipping AOT module loading
[jank] Runtime initialized successfully!
[jank] Calling vybe.sdf.ios/-main...
[jank] Error calling -main: invalid call with 0 args to unbound@0x... for var #'vybe.sdf.ios/-main
```

The `-main` error is expected - JIT mode hasn't loaded any jank source files yet.

## Next Steps
1. **Load jank source files at runtime** - need to implement a mechanism to load the bundled jank sources
2. **Pre-bundle PCH** (optional) - for faster JIT compilation
3. **Test actual JIT compilation** - compile jank code at runtime
4. **Test on physical device** - may require additional iOS LLVM build configuration

## Key Differences: AOT vs JIT

| Feature | AOT | JIT |
|---------|-----|-----|
| Compilation | Pre-compiled at build time | At runtime |
| Module loading | `jank_aot_init()` loads pre-compiled modules | Need to load jank source files |
| PCH | Not needed | Optional (speeds up compilation) |
| LLVM | Not needed at runtime | Embedded (220MB merged library) |
| App size | Smaller | Larger (includes LLVM) |
| nREPL | Not available | Available! |
