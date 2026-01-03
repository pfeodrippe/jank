# iOS JIT PCH Mismatch Fix

## Problem

The iOS JIT simulator app was crashing when trying to compile `#include <vulkan/sdf_engine.hpp>` with a SIGSEGV in `clang::DeclContext::addHiddenDecl` at address 0x10 (null pointer dereference).

The first header `#include <vybe/vybe_sdf_math.h>` compiled successfully, but the second header crashed the clang parser.

## Root Cause

The iOS app was using the **desktop macOS PCH** (68MB) instead of the **iOS-specific PCH** (52MB).

The desktop PCH is built with:
- macOS target
- macOS SDK headers
- macOS libc++ headers

The iOS app requires a PCH built with:
- `-target arm64-apple-ios17.0-simulator`
- `-isysroot` pointing to iOS Simulator SDK
- iOS SDK's libc++ headers (`-nostdinc++` + `-isystem "$IOS_SDK/usr/include/c++/v1"`)

When the clang interpreter at runtime tries to parse headers using the wrong PCH, the AST structures become incompatible, leading to null DeclContext pointers during namespace parsing.

## Solution

Run the iOS-specific PCH build script instead of copying the desktop PCH:

```bash
cd /Users/pfeodrippe/dev/something/SdfViewerMobile
./build-ios-pch.sh
```

This script builds the PCH with the correct iOS target and SDK settings.

## Key Files

- `/Users/pfeodrippe/dev/something/SdfViewerMobile/build-ios-pch.sh` - iOS PCH build script
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/jank-resources/incremental.pch` - iOS PCH output location
- `/Users/pfeodrippe/dev/jank/compiler+runtime/build/incremental.pch` - Desktop PCH (DO NOT copy to iOS!)

## Symptoms of Wrong PCH

- First header compiles successfully (may use cached data)
- Second or subsequent headers crash in clang parser
- SIGSEGV in `DeclContext::addHiddenDecl` or similar parser functions
- Address 0x10 or similar low addresses (null pointer + offset)

## Prevention

Always rebuild the iOS PCH when jank headers change:

```bash
# After updating jank code
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./bin/compile  # Build desktop jank

# Then rebuild iOS PCH
cd /Users/pfeodrippe/dev/something/SdfViewerMobile
./build-ios-pch.sh  # Build iOS-specific PCH

# Then run iOS build
cd /Users/pfeodrippe/dev/something
make ios-jit-sim-run
```
