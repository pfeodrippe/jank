# iOS Native Backtrace for Stack Traces

## Date
2026-01-02

## Problem

cpptrace doesn't compile on iOS due to libdwarf dependency issues:
```
dwarf_debuglink.c:642:17: error: call to undeclared function 'getcwd'
dwarf_elfread.c:308:35: error: call to undeclared function 'sysconf'
```

libdwarf is missing POSIX headers when cross-compiling for iOS simulator.

## Solution

Use iOS's native `backtrace()` and `backtrace_symbols()` functions from `<execinfo.h>` instead of cpptrace.

### Changes Made

**1. Keep cpptrace disabled for iOS** (`include/cpp/jank/util/cpptrace.hpp`):
```cpp
// cpptrace doesn't build on iOS (libdwarf issues), use native backtrace instead
#if !defined(JANK_TARGET_EMSCRIPTEN) && !defined(JANK_TARGET_IOS)
  #include <cpptrace/cpptrace.hpp>
  // ...
#else
  // stub for iOS and Emscripten
#endif
```

**2. Add execinfo.h** (`SdfViewerMobile/sdf_viewer_ios.mm`):
```cpp
#include <execinfo.h>
```

**3. Use native backtrace in error handler** (`SdfViewerMobile/sdf_viewer_ios.mm:467-475`):
```cpp
// Use native iOS backtrace (cpptrace has build issues on iOS)
void* callstack[128];
int frames_count = backtrace(callstack, 128);
char** strs = backtrace_symbols(callstack, frames_count);

for (int i = 0; i < frames_count; ++i) {
    std::cerr << "║ " << strs[i] << "\n";
}
free(strs);
```

## Advantages of Native Backtrace

- ✅ No build issues - works out of the box on iOS
- ✅ No external dependencies (libdwarf, zstd, etc.)
- ✅ Fast compilation
- ✅ Native iOS API, well-supported

## Limitations

The output format is less detailed than cpptrace:

**Native backtrace output:**
```
0   SdfViewerMobile    0x000000010234abcd _ZN4jank7runtime12expect_objectINS0_3obj7integerEEENS0_4orefIT_EENS0_10object_refE + 123
```

**cpptrace output (desktop):**
```
#0 jank::runtime::expect_object<jank::runtime::obj::integer>() at rtti.hpp:39
```

But it's good enough for debugging! You can use `atos` to symbolicate:
```bash
atos -o SdfViewerMobile.app/SdfViewerMobile 0x000000010234abcd
```

## Rebuild Steps

```bash
cd ~/dev/something
make ios-jit-sim-build
make ios-jit-sim-run
```

## Expected Output

When an error occurs:
```
╔══════════════════════════════════════════════════════════════
║ C++ Standard Exception
╠══════════════════════════════════════════════════════════════
║ invalid object type (expected real found nil); value=nil
║
║ Note: This is a C++ exception, not a jank exception.
║ It may be from C++ interop code or runtime type checking.
╠══════════════════════════════════════════════════════════════
║ Stack Trace:
╠══════════════════════════════════════════════════════════════
║ 0   SdfViewerMobile    0x... _ZN4jank7runtime12expect_object...
║ 1   SdfViewerMobile    0x... _ZN4vybe3sdf3ios...
║ 2   SdfViewerMobile    0x... _ZN4vybe3sdf3ios$main_loop...
╚══════════════════════════════════════════════════════════════
```

## Future Work

- Could try patching libdwarf to add missing POSIX headers for iOS
- Could build cpptrace with a different backend (not libdwarf)
- For production AOT builds, could disable backtrace entirely

## Related Files

- `/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp/jank/util/cpptrace.hpp` - Keep stub for iOS
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm` - Native backtrace implementation
- `ai/20260102-007-enable-cpptrace-on-ios.md` - Previous attempt (failed due to libdwarf)
