# Persistent Compiler JIT Compilation Limitation

## Date: 2025-12-26

## Summary

The persistent Clang compiler approach for faster iOS cross-compilation is **not working** because LLVM/Clang headers cannot be JIT-compiled by the jank runtime.

## Problem

The compile server runs as a jank process that JIT-compiles headers at runtime using CppInterOp/clang-repl. When `server.hpp` includes `persistent_compiler.hpp`, which in turn includes LLVM/Clang headers like:

```cpp
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/MemoryBuffer.h>
```

These headers conflict with the system standard library at JIT time, causing errors like:
- `error: "We don't know how to get the definition of mbstate_t on your platform."`
- `error: reference to unresolved using declaration` (for memcpy, etc.)
- Various FP_NAN, FP_INFINITE macro issues

This happens because:
1. LLVM's bundled libc++ has different implementation details than the system libc++
2. When JIT-compiling, the two conflict because they expect different runtime environments

## Current Status

The persistent compiler code exists in `persistent_compiler.hpp` but is **disabled** in `server.hpp`:

```cpp
// NOTE: persistent_compiler.hpp is disabled in JIT mode because LLVM/Clang headers
// conflict with the system standard library when JIT-compiled. The persistent compiler
// approach requires building as a standalone binary, not JIT-compiled.
// #include <jank/compile_server/persistent_compiler.hpp>
```

The compile server continues to use the popen-based approach which spawns a new clang process for each compilation.

## Solution Options

### Option 1: Standalone Compile Server Binary (Recommended)

Build the compile server as a separate binary that's compiled ahead-of-time (not JIT), which would allow including the LLVM/Clang headers without conflicts.

```
bin/jank-compile-server --port 5570 --resource-dir /path/to/resources
```

**Pros:**
- Full access to LLVM/Clang APIs
- PCH loaded once at startup
- No process spawn overhead
- 4-10x faster compilation

**Cons:**
- Separate binary to build and distribute
- Needs to be built with the same LLVM version as jank

### Option 2: Clang Daemon

Keep a long-running clang daemon process that accepts compilation requests via IPC/socket, avoiding the spawn overhead while still using the CLI approach.

**Pros:**
- No changes to jank build system
- PCH stays in memory

**Cons:**
- Need to implement IPC protocol
- Less direct than CompilerInstance API

### Option 3: Object Cache

Cache compiled objects keyed by (source hash, namespace). Only recompile when code changes.

**Pros:**
- Simple to implement
- Works with current popen approach

**Cons:**
- Still slow for first compilation
- Cache invalidation can be tricky
- User explicitly rejected this approach

## Files

- `include/cpp/jank/compile_server/persistent_compiler.hpp` - The persistent compiler implementation (exists but not used)
- `include/cpp/jank/compile_server/server.hpp` - Compile server (uses popen fallback)

## Next Steps

1. Build compile server as standalone binary with proper LLVM linkage
2. Add CMake target for `jank-compile-server` executable
3. Update `make sdf-ios-server` to use the standalone binary
4. Benchmark performance improvement
