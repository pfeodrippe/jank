# iOS JIT - Pre-compiled Header Implementation Plan

Date: 2025-12-22

## Executive Summary

The iOS JIT crashes when trying to parse `jank/prelude.hpp` at runtime because the CppInterOp interpreter's incremental parsing mode fails with a null `DeclContext`. The solution is to **pre-build the PCH on macOS** and bundle it with the iOS app, avoiding runtime header parsing entirely.

## Investigation Findings

### The Crash

```
clang::Decl::castFromDeclContext(clang::DeclContext const*) + 0
clang::Sema::forRedeclarationInCurContext() const + 16
clang::Sema::ActOnTag(...) + 2876
clang::Parser::ParseClassSpecifier(...) + 6480
```

**Root cause**: The `Sema::CurContext` (current DeclContext) is null when the parser attempts to parse a namespace or class declaration. This indicates that the `TranslationUnitDecl` was never properly created during interpreter initialization.

### Why Runtime Parsing Fails on iOS

1. **Incremental parsing mode limitations**: The CppInterOp interpreter uses Clang's incremental compilation mode (`-fincremental-extensions`), which has complex initialization requirements.

2. **Missing initialization steps**: On iOS, the interpreter may not be receiving all necessary initialization that normally happens when a PCH is loaded.

3. **PCH provides more than just speed**: Loading a PCH properly initializes the AST context, creates the TranslationUnitDecl, and sets up the sema context - all of which are required for subsequent parsing to work.

### Version Compatibility

- **macOS Clang**: 22.0.0git (commit 4e5928689f2399dc6aede8dde2536a98a96a1802)
- **iOS LLVM**: 22.0.0git

Both builds use the same LLVM version, so PCH files should be compatible as long as:
- Same include paths are used
- Same preprocessor defines are set
- Same language standard is used

## Implementation Plan

### Phase 1: Create PCH Build Script

Create a script that builds the iOS PCH using the macOS clang with iOS target settings:

**Script location**: `/Users/pfeodrippe/dev/something/SdfViewerMobile/build-ios-pch.sh`

**Key flags needed**:
- `-target arm64-apple-ios17.0-simulator` - iOS simulator target
- `-isysroot <iOS SDK path>` - iOS SDK headers
- `-std=gnu++20` - Match runtime C++ standard
- `-Xclang -fincremental-extensions` - Required for CppInterOp compatibility
- `-Xclang -emit-pch` - Generate PCH
- `-Xclang -fmodules-embed-all-files` - Embed all files in PCH
- `-fno-modules-validate-system-headers` - Avoid header timestamp issues
- `-fpch-instantiate-templates` - Pre-instantiate templates
- `-Xclang -fno-validate-pch` - Disable PCH validation
- `-Xclang -fno-pch-timestamp` - No timestamp in PCH

### Phase 2: Modify find_pch() for iOS

Update `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/util/clang_ios.cpp`:

```cpp
jtl::option<jtl::immutable_string> find_pch(jtl::immutable_string const &)
{
    // Look for pre-bundled PCH in app resources
    static jtl::immutable_string result;
    if(result.empty())
    {
        auto pch_path = jtl::immutable_string{ resource_dir() } + "/incremental.pch";
        if(std::filesystem::exists(pch_path.c_str()))
        {
            result = pch_path;
        }
    }
    if(!result.empty())
    {
        return result;
    }
    return jtl::none;
}
```

### Phase 3: Update iOS App Build

1. Add the PCH build script to the Xcode pre-build phase
2. Bundle the generated PCH in `jank-resources/`
3. Update `project-jit.yml` to include the PCH

### Files to Modify

1. **NEW**: `SdfViewerMobile/build-ios-pch.sh` - PCH build script
2. **MODIFY**: `jank/util/clang_ios.cpp` - Update `find_pch()` to return bundled PCH
3. **MODIFY**: `SdfViewerMobile/project-jit.yml` - Bundle PCH as resource

## PCH Build Command

```bash
JANK_DIR=/Users/pfeodrippe/dev/jank/compiler+runtime
IOS_SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
LLVM_CLANG=$JANK_DIR/build/llvm-install/usr/local/bin/clang++

$LLVM_CLANG \
  -target arm64-apple-ios17.0-simulator \
  -isysroot "$IOS_SDK" \
  -std=gnu++20 \
  -DIMMER_HAS_LIBGC=1 -DIMMER_TAGGED_NODE=0 -DHAVE_CXX14=1 \
  -DFOLLY_HAVE_JEMALLOC=0 -DFOLLY_HAVE_TCMALLOC=0 \
  -DFOLLY_ASSUME_NO_JEMALLOC=1 -DFOLLY_ASSUME_NO_TCMALLOC=1 \
  -DJANK_TARGET_IOS=1 \
  -I$JANK_DIR/include/cpp \
  -I$JANK_DIR/src/cpp \
  -I$JANK_DIR/third-party/immer \
  -I$JANK_DIR/third-party/bdwgc/include \
  -I$JANK_DIR/third-party/bpptree/include \
  -I$JANK_DIR/third-party/boost-preprocessor/include \
  -I$JANK_DIR/third-party/boost-multiprecision/include \
  -I$JANK_DIR/third-party/folly \
  -I$JANK_DIR/third-party/stduuid/include \
  -Xclang -fincremental-extensions \
  -Xclang -emit-pch \
  -Xclang -fmodules-embed-all-files \
  -fno-modules-validate-system-headers \
  -fpch-instantiate-templates \
  -Xclang -fno-validate-pch \
  -Xclang -fno-pch-timestamp \
  -x c++-header \
  -o jank-resources/incremental.pch \
  -c $JANK_DIR/include/cpp/jank/prelude.hpp
```

## Expected Outcome

With the pre-built PCH:
1. The iOS JIT processor will find and load the PCH at startup
2. All jank types will be immediately available without runtime parsing
3. The interpreter's AST context will be properly initialized
4. JIT compilation of jank code will work correctly

## Additional Fix: RTTI Mismatch

During implementation, a linker error occurred:
```
Undefined symbols for architecture arm64:
  "typeinfo for llvm::ErrorInfoBase", referenced from:
      typeinfo for llvm::ErrorInfo<llvm::ErrorList, llvm::ErrorInfoBase>
```

**Root cause**: iOS LLVM was built without RTTI (`LLVM_ENABLE_RTTI=OFF`), but jank was being compiled with `-frtti`.

**Solution**: Modified `CMakeLists.txt` to use `-fno-rtti` for iOS JIT builds:

```cmake
if(jank_ios_jit)
  set(jank_rtti_flag -fno-rtti)
else()
  set(jank_rtti_flag -frtti)
endif()
```

## Risk Mitigation

1. **Version mismatch**: If the macOS and iOS LLVM builds diverge, PCH may become incompatible. Solution: Track LLVM commits in both builds.

2. **Path differences**: Paths embedded in PCH must match runtime paths. Solution: Use relative includes and ensure bundle paths match.

3. **SDK version**: PCH built with one iOS SDK version should work with compatible SDK versions. Solution: Use deployment target (17.0) rather than specific SDK version.

4. **RTTI compatibility**: iOS JIT builds must use `-fno-rtti` to match LLVM. This is handled automatically in CMakeLists.txt.
