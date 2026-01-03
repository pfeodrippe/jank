# Fast Compile Server - Comprehensive Research & Implementation Plan

## Date: 2025-12-26

## Executive Summary

After extensive research into jank's codebase, LLVM/Clang cross-compilation capabilities, and persistent compilation approaches, I've identified **three viable solutions** for making the iOS compile server fast. The **recommended approach** is to use the **existing standalone `compile-server` binary** with the persistent Clang CompilerInstance API.

---

## Current State

### The Problem
The iOS compile server spawns a new `clang++` process for every compilation request:
- **Process spawn**: ~50-100ms overhead
- **PCH parsing**: ~200-500ms for 52MB PCH
- **Actual compilation**: ~50-200ms
- **Total per eval**: ~300-800ms

### Current Architecture
```
iOS App (nREPL 5558)
    ↓ (code)
macOS Compile Server (port 5570) ← Currently runs as jank JIT process
    ↓ (popen clang++ every time!)
clang++ process
    ↓ (.o file)
iOS App (loads & executes)
```

### Key Discovery: Standalone Compile Server Already Exists!

**File:** `src/cpp/compile_server_main.cpp`
**CMake Target:** `jank_compile_server` (builds `compile-server` binary)
**Condition:** Built when `jank_test=ON`

This standalone binary:
- Links against `LLVM clang-cpp` (full LLVM/Clang libraries)
- Is compiled ahead-of-time (NOT JIT-compiled)
- Can include LLVM/Clang headers without conflicts
- Already has proper argument parsing and server logic

---

## Research Findings

### 1. Jank's CppInterOp/Clang-REPL Architecture

**Location:** `src/cpp/jank/jit/processor.cpp`

Jank uses CppInterOp which wraps clang-repl:
```cpp
// processor.cpp:228-229
interpreter.reset(static_cast<Cpp::Interpreter *>(
  Cpp::CreateInterpreter(args, {}, vfs, static_cast<int>(llvm::CodeModel::Large))));
```

**Key capabilities:**
- Cross-compilation via `-target` flag (already used for iOS)
- PCH support via VFS (Virtual File System)
- Target triple: `arm64-apple-ios17.0-simulator` or `arm64-apple-ios17.0`

**Limitation:** CppInterOp's `Cpp::Interpreter` is designed for JIT execution, not just object file emission. It uses LLJIT which executes code in-process.

### 2. LLVM ORC JIT Out-of-Process Execution (GSoC 2024 - BREAKTHROUGH!)

**Discovery:** LLVM now supports out-of-process JIT execution!

```bash
clang-repl --oop-executor=path/to/llvm-jitlink-executor \
           --orc-runtime=path/to/liborc_rt.a
```

Or connect to a running executor:
```bash
clang-repl --oop-executor-connect=ios-device:12345
```

**How it works:**
- Compilation happens on host (macOS)
- Object linking/execution happens on remote (iOS)
- Uses ORC-RPC for communication via sockets
- Already used by LLDB for expression evaluation

**Potential:** This could enable true interactive REPL on iOS!

### 3. Clang CompilerInstance API

**File:** `include/cpp/jank/compile_server/persistent_compiler.hpp` (already created)

Using Clang's C++ API directly:
```cpp
clang::CompilerInstance compiler;
compiler.setInvocation(invocation);
compiler.createDiagnostics(...);

// Remap source to in-memory buffer
compiler.getPreprocessorOpts().addRemappedFile(input_file, buffer.release());

// Set output to in-memory stream
compiler.setOutputStream(std::move(output_stream));

// Execute compilation
clang::EmitObjAction action;
compiler.ExecuteAction(action);
```

**Benefits:**
- PCH loaded once at startup
- No process spawn overhead
- All I/O in memory
- 4-10x faster compilation

**Limitation:** Can only work in the standalone binary, not JIT-compiled code.

### 4. Why JIT Mode Can't Use Persistent Compiler

When jank runs and JIT-compiles `server.hpp`, including `persistent_compiler.hpp` causes conflicts:

```
error: "We don't know how to get the definition of mbstate_t on your platform."
error: reference to unresolved using declaration (memcpy, etc.)
```

This is because LLVM's bundled libc++ headers conflict with system headers during JIT compilation.

---

## Solution Options

### Option 1: Standalone Compile Server with Persistent Compiler (RECOMMENDED)

**Architecture:**
```
iOS App (nREPL 5558)
    ↓ (code)
./compile-server binary (standalone, AOT-compiled)
    ↓ (persistent clang::CompilerInstance)
In-memory compilation (PCH loaded once!)
    ↓ (.o in memory)
iOS App (loads & executes)
```

**Implementation:**
1. Use preprocessor guard in `server.hpp`:
```cpp
#ifdef JANK_COMPILE_SERVER_BINARY
  #include <jank/compile_server/persistent_compiler.hpp>
#endif
```

2. Add `-DJANK_COMPILE_SERVER_BINARY=1` to compile-server build in CMakeLists.txt

3. Enable persistent compiler in standalone binary, keep popen fallback for JIT mode

**Performance:**
| Metric | Before (popen) | After (persistent) |
|--------|----------------|---------------------|
| PCH load | 200-500ms/request | 500ms once at startup |
| Process spawn | 50-100ms/request | 0ms |
| Disk I/O | 10-50ms/request | 0ms |
| **Total per eval** | **300-800ms** | **50-200ms** |

**Effort:** Low (1-2 days)

### Option 2: CppInterOp Cross-Compiler

**Idea:** Create a second CppInterOp interpreter configured for iOS cross-compilation.

```cpp
// Compile-time interpreter for iOS target
std::vector<char const *> args{
  "-std=c++20",
  "-target", "arm64-apple-ios17.0-simulator",
  "-isysroot", ios_sdk_path.c_str(),
  "-include-pch", pch_path.c_str(),
  // ... other flags
};
auto *cross_interp = Cpp::CreateInterpreter(args, {}, vfs);
```

**Challenge:** CppInterOp is designed for JIT execution. Need to extract object code before execution.

**Potential approach:**
1. Use `interpreter->getExecutionEngine()` to get LLJIT
2. Hook into object emission before JIT linking
3. Extract MachO object data

**Effort:** Medium (1 week)

### Option 3: LLVM ORC Out-of-Process Execution

**Idea:** Use LLVM's new out-of-process execution for true remote JIT.

**Architecture:**
```
iOS App
  ├── llvm-jitlink-executor (linked into app)
  └── Listens on socket for JIT requests

macOS
  └── clang-repl --oop-executor-connect=ios-simulator:12345
      └── Compiles code, sends objects to iOS for linking/execution
```

**Benefits:**
- Official LLVM feature
- True interactive REPL on iOS
- No object serialization needed

**Challenges:**
- llvm-jitlink-executor needs to be built for iOS
- Network latency for each symbol resolution
- More complex architecture

**Effort:** High (2-3 weeks)

### Option 4: Clang Daemon with IPC

**Idea:** Keep a single clang process running, communicate via IPC.

**Architecture:**
```
Compile Server
    ↓ (spawns once at startup)
clang --daemon mode (hypothetical)
    ↓ (IPC for each request)
Object files
```

**Challenge:** Clang doesn't have a native daemon mode. Would need to:
- Fork clang or use clang-server
- Implement custom IPC protocol
- Handle state between compilations

**Effort:** High (2+ weeks)

---

## Recommended Implementation Plan

### Phase 1: Standalone Compile Server with Persistent Compiler (Immediate Win)

**Goal:** 4-10x speedup with minimal effort

**Steps:**

1. **Add preprocessor guard to server.hpp:**
```cpp
// At top of file
#ifdef JANK_COMPILE_SERVER_BINARY
  #include <jank/compile_server/persistent_compiler.hpp>
#endif

// In server class
#ifdef JANK_COMPILE_SERVER_BINARY
    persistent_compiler persistent_compiler_;
#endif
```

2. **Update CMakeLists.txt:**
```cmake
target_compile_options(jank_compile_server PUBLIC
  ${jank_common_compiler_flags}
  ${jank_aot_compiler_flags}
  -DJANK_COMPILE_SERVER_BINARY=1
)
```

3. **Initialize persistent compiler in constructor:**
```cpp
#ifdef JANK_COMPILE_SERVER_BINARY
      if(persistent_compiler_.init(config_.clang_path, config_.target_triple, ...))
      {
        std::cout << "[compile-server] Using persistent compiler" << std::endl;
      }
#endif
```

4. **Use persistent compiler in cross_compile():**
```cpp
#ifdef JANK_COMPILE_SERVER_BINARY
      if(persistent_compiler_.is_initialized())
      {
        auto result = persistent_compiler_.compile(cpp_code, "compile_" + std::to_string(id));
        return { result.success, std::move(result.object_data), result.error };
      }
#endif
      // Fallback to popen...
```

5. **Update Makefile/scripts to use standalone binary:**
```bash
# Instead of: jank --ios-compile-server
# Use: ./compile-server --target sim --port 5570
```

### Phase 2: CppInterOp Cross-Compiler (Future Enhancement)

If Phase 1 isn't fast enough, investigate using CppInterOp for cross-compilation:

1. Study how CppInterOp emits objects
2. Create cross-compilation interpreter
3. Hook object emission before JIT linking

### Phase 3: LLVM ORC Out-of-Process (Long-term)

For true interactive iOS REPL:

1. Build llvm-jitlink-executor for iOS
2. Integrate into iOS app bundle
3. Connect clang-repl from macOS
4. Enable true remote JIT execution

---

## File Changes for Phase 1

### 1. `CMakeLists.txt`
```cmake
# Add to jank_compile_server target (around line 1516)
target_compile_options(jank_compile_server PUBLIC
  ${jank_common_compiler_flags}
  ${jank_aot_compiler_flags}
  -DJANK_COMPILE_SERVER_BINARY=1
)
```

### 2. `include/cpp/jank/compile_server/server.hpp`
- Add `#ifdef JANK_COMPILE_SERVER_BINARY` guards around persistent_compiler include and usage
- Keep popen fallback for JIT mode

### 3. `include/cpp/jank/compile_server/persistent_compiler.hpp`
- Already created, just needs to be enabled

### 4. User-facing scripts
- Update `make sdf-ios-server` to use standalone `./compile-server` binary
- Or add `--use-standalone` flag to jank

---

## Testing Plan

1. **Build standalone compile-server:**
```bash
cd compiler+runtime
./bin/configure -GNinja -DCMAKE_BUILD_TYPE=Debug -Djank_test=on
ninja compile-server
```

2. **Run standalone server:**
```bash
./build/compile-server --target sim --port 5570 \
  -I/path/to/project/vendor \
  -I/path/to/project
```

3. **Start iOS app and test:**
```bash
clj-nrepl-eval -p 5558 "(defn test [] (+ 1 2))"
```

4. **Benchmark:**
- Time first compilation (should include PCH load ~500ms)
- Time subsequent compilations (should be 50-200ms)

---

## Summary

| Approach | Speedup | Effort | Complexity |
|----------|---------|--------|------------|
| **Standalone + Persistent Compiler** | 4-10x | Low | Low |
| CppInterOp Cross-Compiler | 4-10x | Medium | Medium |
| LLVM ORC Out-of-Process | True remote JIT | High | High |
| Clang Daemon | 2-4x | High | High |

**Recommendation:** Implement Phase 1 (Standalone + Persistent Compiler) immediately. This provides the best ROI with minimal risk.

---

## References

- [Clang CompilerInstance](https://clang.llvm.org/doxygen/classclang_1_1CompilerInstance.html)
- [Cross-compilation using Clang](https://clang.llvm.org/docs/CrossCompilation.html)
- [GSoC 2024: Out-of-Process Execution for Clang-Repl](https://blog.llvm.org/posts/2024-10-23-out-of-process-execution-for-clang-repl/)
- [LLVM ORC JIT Documentation](https://llvm.org/docs/ORCv2.html)
- [ez-clang Remote REPL](https://echtzeit.dev/ez-clang/)
- [CppInterOp GitHub](https://github.com/compiler-research/CppInterOp)
