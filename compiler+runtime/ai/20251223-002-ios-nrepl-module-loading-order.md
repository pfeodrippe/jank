# iOS nREPL Module Loading Order Fix

## Problem
When running iOS JIT device builds with nREPL, the app crashed with a stack overflow:
- `EXC_BAD_ACCESS (code=2)`
- Crash in `___chkstk_darwin` during `visit_object` → `get_in`

## Root Cause
The `jank.nrepl-server.asio` native module was being loaded BEFORE `clojure.core` was initialized. This caused the nREPL code to try to use core functions that weren't yet loaded, leading to infinite recursion and stack overflow.

## Solution
Modified `/Users/pfeodrippe/dev/jank/compiler+runtime/bin/ios-bundle` to insert the nREPL asio loading inside `jank_aot_init()` at the correct position:

```
Order of loading in jank_aot_init():
1. Core libraries (clojure.core-native, clojure.core, string, set, walk, template, test)
2. [JIT mode only] nREPL native module (jank.nrepl-server.asio)
3. Application modules (including jank.nrepl-server.server which depends on asio)
```

## Files Changed
1. `bin/ios-bundle` (lines 667-690): Added `#if defined(JANK_IOS_JIT)` block to load nREPL asio after core libs
2. `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm`: Removed manual loading of nREPL asio before `jank_aot_init()`
3. `/Users/pfeodrippe/dev/something/Makefile`: Changed from generating a stub `jank_aot_init.cpp` to copying the ios-bundle generated one

## Key Insight
The `jank_aot_init()` function is auto-generated and controls the exact load order. For JIT mode, we need to conditionally insert native module loading at the right position - after core libs but before user modules that depend on them.

## Build System Issue
The Makefile was generating a STUB `jank_aot_init.cpp` for JIT builds that did nothing:
```cpp
extern "C" void jank_aot_init() {
    std::cout << "[jank] JIT mode - skipping AOT module loading" << std::endl;
}
```

This was wrong because JIT mode is actually HYBRID mode (AOT core libs + JIT user code). The fix was to copy the proper `jank_aot_init.cpp` from the AOT build instead:
```makefile
@cp SdfViewerMobile/build-iphoneos/generated/jank_aot_init.cpp SdfViewerMobile/build-iphoneos-jit/generated/
```

## Critical Insight: Conditional Compilation Doesn't Work for libvybe_aot.a

Initially tried to use `#if defined(JANK_IOS_JIT)` to conditionally load nREPL asio:
```cpp
#if defined(JANK_IOS_JIT)
    jank_load_jank_nrepl_server_asio();
    jank_module_set_loaded("jank.nrepl-server.asio");
#endif
```

**This doesn't work because:**
1. `jank_aot_init.cpp` is compiled into `libvybe_aot.a`
2. `libvybe_aot.a` is built by ios-bundle WITHOUT `JANK_IOS_JIT` defined
3. The JIT device project links against this same `libvybe_aot.a`
4. So the `#if` block is NEVER compiled

**The fix:** Remove the conditional and ALWAYS load nREPL asio:
```cpp
// Always load it (not guarded by JANK_IOS_JIT) since it's linked into the binary
jank_load_jank_nrepl_server_asio();
jank_module_set_loaded("jank.nrepl-server.asio");
```

This is safe because:
- The nREPL asio native module is always linked into both AOT and JIT builds
- For pure AOT builds that don't use nREPL, it just loads a module that never gets used
