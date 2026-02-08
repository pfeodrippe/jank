# Incremental Compiler for iOS Cross-Compilation - WORKING!

## Date: 2025-12-27

## Summary

Successfully implemented and tested the `incremental_compiler` class using `clang::IncrementalCompilerBuilder` and `clang::Interpreter` for TRUE incremental iOS cross-compilation. Headers are parsed ONCE at startup (~1.5s), and subsequent compilations are dramatically faster.

## Performance Results

### Before (persistent_compiler)
- Every compilation: ~1600ms (re-parsed headers each time)
- `(+ 1 2)` via nREPL: ~1.6s total

### After (incremental_compiler)
- Header parsing: 1531ms (ONCE at startup)
- `(+ 1 2)`: **22ms** (parse: 7ms, emit: 14ms)
- `(defn test-fn [x] (* x x))`: **24ms** (parse: 8ms, emit: 15ms)
- `(test-fn 5)`: **29ms** (parse: 11ms, emit: 17ms)
- Full namespace compilation (~2MB): 200-900ms

**Compilation speedup: 50-70x for simple expressions!**

### End-to-End nREPL Timing
- Before: ~1.6s
- After: ~0.25s (6x faster)

The remaining time is network latency and iOS processing, not compilation.

## Key Implementation Details

### 1. Resource Directory Fix

The `-resource-dir` flag must be added BEFORE `-isysroot` to ensure clang's built-in headers (stdarg.h, stddef.h, etc.) are found before the iOS SDK headers.

```cpp
// Correct order in init():
args_storage.push_back("-resource-dir");
args_storage.push_back(resource_dir.string());  // e.g., .../lib/clang/22
args_storage.push_back("-isysroot");
args_storage.push_back(sysroot);  // iOS SDK path
```

### 2. Header Parsing at Startup

```cpp
interpreter_->Parse("#include <jank/prelude.hpp>");
// Headers now in AST, subsequent parses skip them
```

### 3. Fast Compilation Loop

Each compile request:
1. Parse new code (7-200ms depending on complexity)
2. Emit ARM64 object code (14-700ms)
3. Return object bytes to iOS app

No header re-parsing!

## Files Modified

1. `include/cpp/jank/compile_server/persistent_compiler.hpp`
   - `incremental_compiler` class with proper `-resource-dir` handling
   - Lines 324-350: Resource directory detection and argument ordering

## Testing Commands

```bash
# Start compile-server
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./build/compile-server --target sim --port 5570 \
    --module-path ... \
    -I ...

# Run iOS app (in another terminal)
cd /Users/pfeodrippe/dev/something
make ios-jit-only-sim-run

# Test nREPL speed
time clj-nrepl-eval -p 5558 "(+ 1 2)"
```

## Compile-Server Log Shows Success

```
[incremental-compiler] Using clang resource dir: .../lib/clang/22
[incremental-compiler] Initialized for arm64-apple-ios17.0-simulator in 2ms
[incremental-compiler] Parsed runtime headers in 1531ms
[compile-server] Headers parsed - ready for fast compilation!
...
[compile-server] Compiling code (id=18) in ns=user: (+ 1 2)
[incremental-compiler] Compiled compile_18 (41792 bytes) - parse: 7ms, emit: 14ms, total: 22ms
```

## Next Steps

1. Consider adding PCH support to incremental_compiler for even faster startup
2. Profile larger compilations to identify optimization opportunities
3. Consider caching common patterns in the interpreter AST
