# iOS JIT Implementation

**Date**: 2025-12-22
**Status**: COMPLETE - iOS JIT library builds successfully for simulator

## What Was Fixed

The following issues were resolved to enable iOS JIT builds:

1. **cpptrace stubbing**: Added `JANK_TARGET_IOS` guards to all cpptrace includes:
   - `include/cpp/jank/util/cpptrace.hpp` - main stub header (already had iOS guard)
   - `include/cpp/jank/util/try.hpp` - added iOS guard for `from_current.hpp`
   - `src/cpp/jank/jit/processor.cpp` - changed to use `jank/util/cpptrace.hpp`
   - Changed stub from `class stacktrace` to `struct stacktrace` for consistency

2. **Warning suppressions**: Added iOS-specific flags in CMakeLists.txt:
   - `-Wno-mismatched-tags` - for struct/class forward declaration differences
   - `-Wno-invalid-offsetof` - for non-standard-layout types
   - `-Wno-range-loop-construct` - for range-based for loop copy warnings
   - Flags added to `jank_aot_compiler_flags` AFTER `-Wall` to ensure correct order

## Overview

This implementation enables true JIT compilation on iOS during development by:
1. Building LLVM/CppInterOp for iOS (ARM64)
2. Embedding the JIT engine in the iOS app
3. Using proper entitlements for development builds

**Important**: JIT on iOS only works:
- When launched from Xcode (development mode)
- With proper entitlements signed
- NOT on App Store builds

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        iOS JIT Architecture                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  iOS Device (ARM64)                                                      │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │                                                                      │ │
│  │  jank iOS App (with LLVM embedded)                                  │ │
│  │  ├── Cpp::Interpreter (clang-repl)                                  │ │
│  │  ├── LLVM ORC JIT (AArch64 backend)                                │ │
│  │  ├── CppInterOp                                                     │ │
│  │  └── jank runtime                                                   │ │
│  │                                                                      │ │
│  │  nREPL eval → analyze → codegen → JIT compile → execute             │ │
│  │                                                                      │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                          │
│  Requirements:                                                           │
│  - Launched from Xcode                                                   │
│  - jank-jit.entitlements signed                                         │
│  - Development provisioning profile                                      │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## Files Changed/Created

### New Files
1. `bin/build-ios-llvm` - Script to build LLVM/CppInterOp for iOS
2. `ios/jank-jit.entitlements` - Entitlements for JIT code signing

### Modified Files
1. `CMakeLists.txt` - Added `jank_ios_jit` option and iOS JIT configuration

## Build Process

### Step 1: Build LLVM for iOS

```bash
# Build for both device and simulator (takes ~2 hours, needs ~50GB disk)
./bin/build-ios-llvm both

# Or build for specific target
./bin/build-ios-llvm device     # arm64-apple-ios
./bin/build-ios-llvm simulator  # arm64-apple-ios-simulator
```

Output locations (default `~/dev/ios-llvm-build/`, override with `IOS_LLVM_BUILD_ROOT`):
- Device: `~/dev/ios-llvm-build/ios-llvm-device/`
- Simulator: `~/dev/ios-llvm-build/ios-llvm-simulator/`

The script automatically:
- Uses jank's local LLVM source (`build/llvm`)
- Uses jank's native tablegen tools for cross-compilation
- Patches LLVM cmake to support iOS (adds iOS to Darwin-like system checks)
- Patches CppInterOp to support LLVM 22

### Step 2: Configure iOS Build with JIT

```bash
# For simulator
cmake -B build-ios-sim \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -Djank_target_ios=ON \
  -Djank_ios_jit=ON \
  -DIOS_LLVM_DIR=/path/to/build/ios-llvm-simulator

# For device
cmake -B build-ios-device \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -Djank_target_ios=ON \
  -Djank_ios_jit=ON \
  -DIOS_LLVM_DIR=/path/to/build/ios-llvm-device
```

### Step 3: Sign with Entitlements

In Xcode project:
1. Add `ios/jank-jit.entitlements` to the project
2. In Build Settings → Code Signing Entitlements, set to `ios/jank-jit.entitlements`
3. Use development provisioning profile

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `jank_target_ios` | OFF | Build for iOS platform |
| `jank_ios_jit` | OFF | Enable JIT on iOS (requires iOS LLVM build) |
| `IOS_LLVM_DIR` | Auto | Path to iOS LLVM installation |

When `jank_ios_jit` is ON:
- Real `processor.cpp` is used instead of `processor_stub_wasm.cpp`
- CppInterOp is linked from iOS LLVM build
- `JANK_TARGET_IOS` and `JANK_IOS_JIT` are defined
- `JANK_TARGET_WASM` is NOT defined (unlike non-JIT iOS builds)

## Entitlements Required

```xml
<key>com.apple.security.get-task-allow</key>
<true/>

<key>com.apple.security.cs.allow-jit</key>
<true/>

<key>com.apple.security.cs.allow-unsigned-executable-memory</key>
<true/>
```

## Limitations

1. **Only works during development** - JIT requires entitlements that Apple won't approve for App Store
2. **Must be launched from Xcode** - The `get-task-allow` entitlement only works when debugging
3. **Large binary size** - LLVM adds ~500MB to the app
4. **Simulator vs Device** - Need separate LLVM builds for each

## Comparison with Non-JIT iOS

| Aspect | iOS without JIT | iOS with JIT |
|--------|-----------------|--------------|
| Binary size | ~10-50MB | ~500MB+ |
| nREPL eval | Server-compiled dylib patches | Local JIT |
| App Store | Allowed | NOT allowed |
| Xcode required | No | Yes (for JIT) |
| Speed | Native (AOT) | Native (JIT) |

## Future Work

1. **PCH for iOS JIT** - Pre-compiled headers for faster JIT startup
2. **Selective LLVM components** - Reduce binary size by only including needed components
3. **TestFlight support** - Investigate if JIT can work on TestFlight builds
