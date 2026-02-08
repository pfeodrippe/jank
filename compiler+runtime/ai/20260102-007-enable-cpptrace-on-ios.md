# Enable cpptrace on iOS for Stack Traces

## Date
2026-01-02

## Problem

Stack traces were completely missing on iOS because cpptrace was stubbed out for `JANK_TARGET_IOS`:

```cpp
#if !defined(JANK_TARGET_EMSCRIPTEN) && !defined(JANK_TARGET_IOS)
  #include <cpptrace/cpptrace.hpp>
  // ... real implementation
#else
  // ... stub that does nothing
#endif
```

When errors occurred, we got nice error messages but NO stack traces:
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
╚══════════════════════════════════════════════════════════════
                                    ↑↑↑ EMPTY!
```

## Solution

**File:** `/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp/jank/util/cpptrace.hpp`

Removed the `JANK_TARGET_IOS` check to enable cpptrace on iOS:

```cpp
// Enable cpptrace on iOS - we need it for debugging!
// TODO: Make this conditional on JIT vs AOT once we have a JANK_JIT flag
#if !defined(JANK_TARGET_EMSCRIPTEN)
  #include <cpptrace/cpptrace.hpp>
  #include <cpptrace/basic.hpp>
  #include <cpptrace/from_current.hpp>
  #include <cpptrace/formatting.hpp>
  #include <cpptrace/gdb_jit.hpp>
#else
  // stub for Emscripten only
#endif
```

## Rebuild Steps

```bash
# 1. Rebuild jank with the new cpptrace configuration
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./bin/compile 2>&1 | tee /tmp/jank-rebuild-cpptrace.log

# 2. Rebuild iOS app
cd ~/dev/something
make ios-jit-sim-build 2>&1 | tee /tmp/ios-rebuild-cpptrace.log

# 3. Run and check for stack traces
make ios-jit-sim-run 2>&1 | tee /tmp/ios-run-with-stacktrace.log
```

## Expected Result

Now when a C++ exception occurs, we should see a FULL stack trace:

```
╠══════════════════════════════════════════════════════════════
║ Stack Trace:
╠══════════════════════════════════════════════════════════════
Stack trace (most recent call first):
#0 0x... in jank::runtime::expect_object<...>() at rtti.hpp:39
#1 0x... in vybe.sdf.ios$sync_camera_from_cpp_BANG_() at ios.jank:35
#2 0x... in vybe.sdf.ios$main_loop() at ios.jank:78
...
```

## Why Was It Disabled?

Unknown - possibly:
- Early iOS port didn't have cpptrace working
- Thought it wasn't needed for AOT builds
- Performance concerns for production

Since iOS JIT is **debugging-only**, we absolutely need stack traces!

## Future Work

- Add a `JANK_JIT` build flag
- Make cpptrace conditional: enabled for JIT, disabled for AOT production builds
- Test if cpptrace actually works on iOS (might need additional frameworks/linker flags)

## Related Files

- `/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp/jank/util/cpptrace.hpp` - Modified
- `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm` - Error handler that uses cpptrace
