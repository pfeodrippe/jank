# CRITICAL: Build Environment Setup for jank

**ALWAYS set these environment variables before building or testing jank!**

## Environment Variables (use these exact values, NO command substitution)

```bash
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
CC=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang
CXX=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang++
```

## Building

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk CC=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang CXX=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang++ ninja -C build jank jank-test
```

## Configure (if needed)

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk CC=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang CXX=/Users/pfeodrippe/dev/jank/compiler+runtime/build/llvm-install/usr/local/bin/clang++ ./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on -Djank_local_clang=on
```

## Running Tests

**ALWAYS use tee to save results and avoid running tests twice:**

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime/build && ./jank-test 2>&1 | tee /Users/pfeodrippe/dev/jank/compiler+runtime/.tests-new.txt
```

Then check results:
```bash
tail -5 /Users/pfeodrippe/dev/jank/compiler+runtime/.tests-new.txt
```

**NEVER run tests multiple times** - it wastes time!

## Why This Matters
- jank requires its own bundled clang (version 22) to compile
- Using system clang or AppleClang will cause CMake errors or crashes
- The SDKROOT is needed for macOS SDK headers

## Common Error If Forgotten
```
CMake Error: Found Clang 22.0.0git to embed in jank, but trying to use AppleClang 16.0.0 to compile it.
```
