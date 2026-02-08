# iOS JIT Null Vector Element Investigation

## Problem Summary
When loading JIT-compiled modules on iOS, the app crashes during namespace initialization. The crash occurs when `refer` is called with an `:exclude` list (e.g., `(:refer-clojure :exclude [abs])`).

## Key Findings

### Debug Output Analysis
The crash occurs during `reduce` over a vector:
```
[SYMBOL DEBUG] created symbol ns='clojure.core' name='conj!'
[REDUCE DEBUG] reducing over persistent_vector at 0x13ee4d230 size=1
[REDUCE DEBUG]   pre-check elem[0] data=0x0 type=-1
[REDUCE DEBUG] null element in collection!
```

The vector at address `0x13ee4d230`:
- Has size=1
- Contains a null element (data=0x0)
- Was **NOT logged at construction time** despite all constructors having logging

### Root Cause Analysis

The most likely cause is that the **PCH (precompiled header) and/or AOT core libs were built before the debug logging was added**.

When iOS JIT compiles code:
1. The compile server uses a PCH file built from jank headers
2. Template constructors like `persistent_vector(std::in_place_t, Args&&...)` are defined in headers
3. If the PCH was built with old headers (without logging), the JIT code won't have logging
4. This could also mask other issues in how vectors are constructed

### Components that need rebuilding

1. **PCH**: Built by `./SdfViewerMobile/build-ios-pch.sh`
   - Uses headers from `$JANK_SRC/include/cpp`
   - Must be rebuilt when headers change

2. **Core libs**: Built by `./SdfViewerMobile/build_ios_jank_jit.sh simulator`
   - AOT-compiled clojure.core, clojure.set, etc.
   - Must be rebuilt when runtime source changes

### Codegen Path for `:exclude` vector

For `(:refer-clojure :exclude [abs])`:

1. Codegen generates: `make_box<persistent_vector>(std::in_place, make_box<symbol>("", "abs"))`
2. This calls `persistent_vector(std::in_place_t, object_ref)` constructor
3. Which initializes `data{ std::forward<object_ref>(arg) }`
4. immer::vector's initializer_list constructor is used

## Recommended Fix Steps

1. **Rebuild the iOS PCH**:
   ```bash
   cd ~/dev/something
   make ios-jit-pch
   ```

2. **Rebuild core libs**:
   ```bash
   make ios-jit-sim-core
   ```

3. **Clean and rebuild JIT-only build**:
   ```bash
   rm -rf ~/Library/Developer/Xcode/DerivedData/SdfViewerMobile-JIT-Only-*
   make ios-jit-only-sim-build
   ```

4. **Run with compile server**:
   ```bash
   # Terminal 1: Start compile server
   make sdf-ios-server

   # Terminal 2: Run iOS app
   make ios-jit-only-sim-run
   ```

## If Problem Persists

If the null vector issue persists after rebuilding, consider:

1. **Using lldb with iOS simulator** for debugging:
   ```bash
   # Get simulator UDID
   xcrun simctl list devices | grep "iPad Pro 13-inch (M4)"

   # Attach lldb to running app
   xcrun simctl launch --wait-for-debugger 'iPad Pro 13-inch (M4)' com.vybe.SdfViewerMobile-JIT-Only
   lldb -n SdfViewerMobile-JIT-Only
   ```

2. **Setting breakpoints on vector construction**:
   ```lldb
   b jank::runtime::obj::persistent_vector::persistent_vector
   ```

3. **Checking if immer::vector initialization is correct** with forwarded references

## Files Modified (Debug Logging)

- `include/cpp/jank/runtime/obj/persistent_vector.hpp` - in_place constructors
- `src/cpp/jank/runtime/obj/persistent_vector.cpp` - value_type constructors
- `src/cpp/jank/runtime/obj/symbol.cpp` - symbol constructor
- `src/cpp/jank/runtime/core/seq.cpp` - reduce function

## Resolution (2025-12-26)

**The fix was successful!** After rebuilding the PCH and core libs:

1. Ran `make ios-jit-pch` from `~/dev/something`
2. Ran `make ios-jit-sim-core` from `~/dev/something`
3. Rebuilt main jank after PCH outdated error
4. Ran `make sdf-ios-server` and `make ios-jit-only-sim-run`

**Result**: Zero null vector errors! The iOS app loaded successfully with messages:
- `[jank] JIT mode ready! App namespaces loaded via remote compile server.`
- `[jank-hybrid] All modules loaded! Ready for REPL.`
- `[jank] Runtime initialized successfully!`

### Root Cause Confirmed

The issue was that the PCH (precompiled header) was built with old headers that didn't match the runtime. When JIT code referenced template constructors defined in headers, the old PCH provided stale definitions.

**Important lesson**: Always rebuild the iOS PCH (`make ios-jit-pch`) when jank headers change!

## Debug Logging Cleanup

Debug logging has been removed from:
- `include/cpp/jank/runtime/obj/persistent_vector.hpp` - removed iostream include and debug in in_place constructors
- `src/cpp/jank/runtime/obj/persistent_vector.cpp` - removed iostream include and debug in value_type constructors
- `src/cpp/jank/runtime/obj/symbol.cpp` - removed debug in symbol constructor
- `src/cpp/jank/runtime/core/seq.cpp` - removed iostream include and debug in reduce function
