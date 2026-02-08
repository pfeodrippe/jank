# iOS nREPL AOT Investigation - 2025-12-23

## Summary

Attempted to get nREPL working on iOS. The nREPL server code (asio.cpp) is already AOT compiled into libjank.a for iOS JIT builds. However, the issue is that nREPL's `bootstrap_runtime_once()` function tries to load clojure.core, which requires JIT compilation on the current iOS JIT build.

## Key Findings

### 1. nREPL Server is Already AOT Compiled
- The nREPL server code in `src/cpp/jank/nrepl_server/asio.cpp` is compiled into libjank.a
- Added C API functions `jank_nrepl_start_server()` and `jank_nrepl_stop_server()` for iOS
- These are called from `sdf_viewer_ios.mm` to start the nREPL server

### 2. Bootstrap Requires clojure.core
The nREPL server calls `bootstrap_runtime_once()` which does:
```cpp
__rt_ctx->load_module("/clojure.core", module::origin::latest).expect_ok();
dynamic_call(__rt_ctx->in_ns_var->deref(), make_box<obj::symbol>("user"));
dynamic_call(__rt_ctx->intern_var("clojure.core", "refer").expect_ok(),
             make_box<obj::symbol>("clojure.core"));
```

### 3. JIT Compiling clojure.core Fails on iOS
When trying to JIT compile clojure.core on iOS simulator:
- GC runs out of memory: "Too many retries in GC_alloc_large"
- This causes the nREPL startup to fail with "Unknown exception"

### 4. Hybrid Mode Exists for Device (Not Simulator)
The Makefile has `ios-jit-device-core-aot` target that:
- Uses `./bin/ios-bundle` to AOT compile core libs
- Generates .o files for clojure.core, clojure.string, etc.
- These can be linked into the app for hybrid mode

### 5. Simulator JIT Missing AOT Core
- `ios-jit-sim-run` doesn't have equivalent AOT core libs
- Need to add `ios-jit-sim-core-aot` target similar to device

## What Works

1. Native modules load successfully:
   - clojure.core-native
   - jank.nrepl-server.asio

2. nREPL server code compiles and links

3. nREPL C API functions work

## What Doesn't Work

1. JIT compiling clojure.core on iOS - runs out of GC memory
2. Therefore nREPL bootstrap fails

## Solution Options

### Option A: Add AOT Core Libs for Simulator JIT
1. Create `ios-jit-sim-core-aot` target in Makefile
2. Use `./bin/ios-bundle` with `simulator` target to generate .o files
3. Link these .o files into the JIT app
4. Modify `jank_aot_init()` to load these modules
5. Update Xcode project to include the .o files

### Option B: Use Full AOT Build
1. Use `sdf-ios-sim-run` instead of `ios-jit-sim-run`
2. This has all modules AOT compiled
3. But no JIT capability for user code evaluation

### Option C: Pre-load Modules Before nREPL
1. Have iOS app call `load_module("/clojure.core")` during init on large stack
2. This happens before nREPL starts
3. But still hits the same GC issue

## Recommended Approach

**Option A (Hybrid Mode)** is the best approach because:
- It provides AOT core libs for fast startup
- JIT is still available for user code evaluation
- Similar to how ClojureScript handles this

## Changes Made

### Modified Files
1. `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/nrepl_server/asio.cpp`
   - Added `jank_nrepl_start_server()` C API function
   - Added `jank_nrepl_stop_server()` C API function

2. `/Users/pfeodrippe/dev/something/SdfViewerMobile/sdf_viewer_ios.mm`
   - Added code to load native modules in correct order
   - Added nREPL server startup code

3. `/Users/pfeodrippe/dev/something/Makefile`
   - Added `ios-jit-sync-lib` target to rebuild and sync libjank.a
   - Added dependency to `ios-jit-sim-run`

4. `/Users/pfeodrippe/dev/jank/CLAUDE.md`
   - Added Rule 2: Never do manual operations - always update scripts

## Next Steps

1. Add `ios-jit-sim-core-aot` Makefile target
2. Update Xcode project to link AOT .o files
3. Modify iOS app to call `jank_aot_init()` which loads AOT modules
4. Test nREPL with AOT core libs
