# iOS Device JIT Fixes

## Problems Fixed

### 1. Redefinition Errors
iOS device JIT was failing with redefinition errors like:
```
error: redefinition of 'default_bits'
/private/var/.../include/immer/config.hpp:136:12: error: redefinition of 'default_bits'
/Users/pfeodrippe/dev/jank/compiler+runtime/third-party/immer/immer/config.hpp:136:12: note: previous definition is here
```

## Root Cause

### Issue 1: Wrong Target Triple
The `environment_ios.cpp` had hardcoded `arm64-apple-ios17.0-simulator` target triple for ALL iOS builds, including device. This should be:
- Simulator: `arm64-apple-ios17.0-simulator`
- Device: `arm64-apple-ios17.0`

### Issue 2: PCH has Mac Paths Embedded
The precompiled header (`incremental.pch`) was built on macOS with Mac filesystem paths like `/Users/pfeodrippe/dev/jank/compiler+runtime/third-party/immer/immer/config.hpp`. When:
1. PCH is loaded (contains headers from Mac paths)
2. JIT also includes headers from bundle paths (`/private/var/.../include/immer/config.hpp`)
3. Same headers get included twice = redefinition errors

**Why simulator worked but device didn't:**
- iOS Simulator shares the Mac filesystem, so Mac paths in PCH are still accessible
- Physical device is sandboxed and can't access Mac filesystem, but the PCH still has Mac paths embedded

## Solutions Applied

### Fix 1: Conditional Target Triple
Modified `environment_ios.cpp` and `clang_ios.cpp` to use `TARGET_OS_SIMULATOR`:

```cpp
#import <TargetConditionals.h>

// In add_system_flags():
args.emplace_back("-target");
#if TARGET_OS_SIMULATOR
args.emplace_back("arm64-apple-ios17.0-simulator");
#else
args.emplace_back("arm64-apple-ios17.0");
#endif
```

### Fix 2: Don't Bundle PCH for Device
Removed `incremental.pch` from `project-jit-device.yml`. The JIT processor has fallback code that parses the prelude header at runtime when no PCH is found (slower but correct).

**Files modified:**
- `compiler+runtime/src/cpp/jank/util/environment_ios.cpp` - Added `TargetConditionals.h`, conditional target triple
- `compiler+runtime/src/cpp/jank/util/clang_ios.cpp` - Added `TargetConditionals.h`, conditional target triple
- `SdfViewerMobile/project-jit-device.yml` - Removed PCH resource entry

## Testing
After fixes, rebuild jank device libraries:
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./bin/build-ios build-ios-device-jit Debug device jit
```

Then rebuild and run on device:
```bash
cd /path/to/your/project
make ios-jit-device-run
```

## Note
The simulator JIT still uses PCH (works because Mac filesystem is accessible).
Device JIT doesn't use PCH (slower startup, but no redefinition errors).

Future improvement: Build PCH specifically for iOS device with bundle-relative paths.

### 2. Unknown Type `size_t`/`ptrdiff_t`
After fixing redefinition errors, got errors like:
```
arm/_types.h:75:9: error: unknown type name 'ptrdiff_t'
arm/_types.h:85:9: error: unknown type name 'size_t'
```

**Cause:** Clang builtin headers (which define `size_t` via `__SIZE_TYPE__`) weren't being included before SDK headers.

**Fix:** Added `/clang/include` to system include paths in `environment_ios.cpp` BEFORE the SDK `sys_include` path:
```cpp
// Clang builtin headers - MUST come before SDK headers
static std::string clang_include_path;
clang_include_path = std::string(resource_dir().c_str()) + "/clang/include";
args.emplace_back("-isystem");
args.emplace_back(clang_include_path.c_str());
```

### 3. Stack Overflow
After fixing header issues, got stack overflow:
```
Thread 1: ___chkstk_darwin
```

**Cause:** iOS main thread has ~1MB stack, but jank's recursive codegen needs ~8MB.

**Fix:** Added large-stack thread wrapper in `sdf_viewer_ios.mm`:
```cpp
static bool init_jank_runtime_on_large_stack() {
    constexpr size_t STACK_SIZE = 8 * 1024 * 1024; // 8MB
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SIZE);
    // ... create thread and wait for completion
}
```

## Summary of All Changes

| File | Change |
|------|--------|
| `environment_ios.cpp` | Added `TARGET_OS_SIMULATOR` check for target triple |
| `environment_ios.cpp` | Added clang builtin include path before SDK headers |
| `clang_ios.cpp` | Added `TARGET_OS_SIMULATOR` check for target triple |
| `project-jit-device.yml` | Removed PCH from bundle |
| `sdf_viewer_ios.mm` | Added 8MB stack thread wrapper for jank init |
