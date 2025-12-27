# Standalone Compile Server Working

## Date: 2025-12-26

## Summary

The standalone `compile-server` binary is now working for iOS cross-compilation. It successfully:
- Loads clojure.core and native libraries at startup
- Listens for iOS app connections on port 5570
- Receives jank code from the iOS app
- Cross-compiles C++ to ARM64 object files using clang
- Sends the object files back to the iOS app for execution

## Current Status

### Working: Standalone Compile Server with popen

The standalone binary works with the **popen fallback approach** (spawning clang per compilation):

```bash
./build/compile-server --target sim --port 5570
```

**Advantages over JIT mode:**
- clojure.core is loaded once at startup (~3-4 seconds)
- No JIT compilation overhead for server code
- Faster startup for each nREPL eval (no context creation)
- More stable - AOT-compiled binary

### Not Yet Working: Persistent Compiler (CompilerInstance)

The persistent compiler approach (using Clang's C++ API) was implemented but disabled due to API issues:

```cpp
// persistent_compiler.hpp is_initialized() returns false
// TODO: Fix CompilerInvocation::CreateFromArgs usage
```

**Issue:** `CompilerInvocation::CreateFromArgs` doesn't recognize CLI-style arguments like `-c`, `-target`, etc. Need to configure the invocation programmatically instead.

## Files Modified

1. **CMakeLists.txt** - Added `-DJANK_COMPILE_SERVER_BINARY=1` flag
2. **server.hpp** - Added ifdef guards for persistent compiler
3. **persistent_compiler.hpp** - API fixes, currently disabled

## Build & Run

```bash
# Build
ninja -C build compile-server

# Run for simulator
./build/compile-server --target sim --port 5570

# Run for device
./build/compile-server --target device --port 5570
```

## Next Steps

1. **Fix persistent compiler**: Configure `CompilerInvocation` programmatically instead of using CLI args
2. **Add to Makefile**: Create `make sdf-ios-standalone-server` target
3. **Benchmark**: Compare popen vs persistent compiler performance
4. **Handle defn bug**: The `defn` issue is a separate jank analysis bug, not related to compile server

## Sample Output

```
=== jank Compile Server ===
Target: iOS Simulator
Port: 5570
Loading native libraries...
Loading clojure.core...
[compile-server] Using dev build include path: "/Users/pfeodrippe/dev/jank/compiler+runtime/include/cpp"
[compile-server] Added third-party include paths
Starting compile server...
[persistent-compiler] Initialized for arm64-apple-ios17.0-simulator
[compile-server] Persistent compiler initialized!
[compile-server] Started on port 5570
[compile-server] Target: arm64-apple-ios17.0-simulator
[compile-server] SDK: /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk

Ready to accept connections. Press Ctrl+C to stop.

[compile-server] Client connected: 127.0.0.1:49270
[compile-server] Compiling code (id=38) in ns=user: (+ 100 200 300)
[compile-server] Cross-compiling: clang-22 -c -target arm64-apple-ios17.0-simulator ...
[compile-server] Clang exited with status: 0
[compile-server] Compiled successfully, object size: 35016 bytes
```

## Test Results

```clojure
(+ 100 200 300)
=> 600  ;; Works!
```
