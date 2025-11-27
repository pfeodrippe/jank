# Plan: Make jank.compiler Work in WASM

## Executive Summary

This document outlines a comprehensive plan to bring jank's full compiler capabilities to WASM, enabling runtime code evaluation and JIT compilation directly in the browser. The key enabler is **CppInterOp's existing WASM support** - it already compiles clang-repl to WASM!

**Current State:**
- jank WASM: AOT compilation only (pre-compiled C++ → emscripten → WASM)
- No runtime `eval`, no JIT, no dynamic code loading in browser
- `jank_eval()` in WASM just parses - doesn't analyze or execute new code

**Goal:**
- Full `eval` support in browser: `(eval '(+ 1 2))` → `3`
- REPL in browser
- Dynamic module loading
- Hot code reloading

---

## Architecture Overview

### Current Native jank Compilation Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           NATIVE JANK PIPELINE                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  jank source  →  Lexer  →  Parser  →  Analyzer  →  Codegen  →  JIT  →  Exec│
│    (.jank)      (lex.cpp)  (parse.cpp) (analyze/)  (codegen/)  (jit/)       │
│                                                                             │
│                                          ↓                                  │
│                                    C++ source code                          │
│                                          ↓                                  │
│                              Cpp::Interpreter (clang-repl)                  │
│                                          ↓                                  │
│                                  LLVM IR → Native code                      │
│                                          ↓                                  │
│                                   Function pointer                          │
│                                          ↓                                  │
│                                      Execution                              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Proposed WASM Compilation Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           WASM JANK PIPELINE                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  jank source  →  Lexer  →  Parser  →  Analyzer  →  Codegen  →  JIT  →  Exec│
│    (.jank)      (lex.cpp)  (parse.cpp) (analyze/)  (codegen/)  (jit/)       │
│                             ✅         ✅           ✅         🚧           │
│                                                                             │
│                                          ↓                                  │
│                                    C++ source code                          │
│                                          ↓                                  │
│                         Cpp::Interpreter (clang-repl for WASM!)             │
│                                    ↑                                        │
│                    CppInterOp WASM build already exists!                    │
│                                          ↓                                  │
│                             LLVM IR → WASM code                             │
│                                          ↓                                  │
│                              WebAssembly.instantiate()                      │
│                                          ↓                                  │
│                                      Execution                              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Key Discovery: CppInterOp WASM Support

**CppInterOp already supports WASM builds!** This is documented in:
- `third-party/cppinterop/Emscripten-build-instructions.md`
- `third-party/cppinterop/docs/Emscripten-build-instructions.rst`

### What CppInterOp WASM Provides

1. **clang-repl compiled to WASM** - The C++ interpreter runs in the browser
2. **LLVM targeting WebAssembly** - Generates WASM code at runtime
3. **Full C++ parsing & execution** - Same capabilities as native
4. **Proven by xeus-cpp** - C++ Jupyter kernel runs in browser via JupyterLite

### Build Requirements (from CppInterOp docs)

```bash
# Emscripten 3.1.73
emsdk install 3.1.73
emsdk activate 3.1.73

# LLVM 19.x built for WASM
emcmake cmake -DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten \
              -DLLVM_TARGETS_TO_BUILD="WebAssembly" \
              -DLLVM_ENABLE_PROJECTS="clang;lld" \
              ...

# CppInterOp built for WASM
emcmake cmake -DClang_DIR=$LLVM_BUILD_DIR/lib/cmake/clang ...
```

---

## Implementation Phases

### Phase 0: Prerequisites & Dependencies (Foundation) ✅ COMPLETED

**Goal:** Get LLVM + CppInterOp compiled to WASM

**Status:** ✅ **COMPLETED** (Nov 27, 2025)

#### Key Findings

1. **LLVM 22 has native WASM interpreter support!** No need for LLVM 19 patches.
   - `clang/lib/Interpreter/Wasm.cpp` - Native `WasmIncrementalExecutor` class
   - `#ifdef __EMSCRIPTEN__` conditionals built into LLVM 22
   - Uses `wasm-ld` and `dlopen()` internally for incremental compilation

2. **Emscripten 4.0.20 works** (Homebrew version) - no need for emsdk 3.1.73

3. **CppInterOp version bounds updated** to support LLVM 22 (was limited to 19.1.x)

#### Completed Steps

##### Step 0.1: Emscripten Version ✅
```bash
# Using Homebrew emscripten 4.0.20 (works fine!)
emcc --version
# emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) 4.0.20
```

##### Step 0.2: Build LLVM 22 for WASM ✅

Built in `/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/`

**Patches Applied:**
1. `CrossCompile.cmake` - Fixed native tools build to use `clang`/`clang++` directly
2. `Wasm.cpp` - Added `/tmp/` prefix for temp files

**Build Command:**
```bash
mkdir -p /Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build
cd /Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build

emcmake cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten \
  -DLLVM_TARGETS_TO_BUILD="WebAssembly" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_LIBEDIT=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DCLANG_BUILD_TOOLS=OFF \
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_ENABLE_ARCMT=OFF \
  /Users/pfeodrippe/dev/llvm-project/llvm

emmake make clangInterpreter lldWasm lldCommon -j8
```

**Libraries Built:**
- `libclangInterpreter.a` (513KB) - Clang REPL/interpreter
- `liblldWasm.a` (1.1MB) - WebAssembly linker
- `liblldCommon.a` (245KB) - LLD common code
- Plus all LLVM/Clang dependencies (~100+ libraries)

##### Step 0.3: Build CppInterOp for WASM ✅

Built in `/Users/pfeodrippe/dev/jank/wasm-llvm-build/cppinterop-build/`

**Version Bounds Updated:**
- `CMakeLists.txt` modified to accept LLVM 13.0 - 22.x (was 13.0 - 19.1.x)

**Build Command:**
```bash
cd /Users/pfeodrippe/dev/jank/wasm-llvm-build/cppinterop-build

emcmake cmake -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/lib/cmake/llvm \
  -DLLD_DIR=/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/lib/cmake/lld \
  -DClang_DIR=/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/lib/cmake/clang \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ON \
  -DSYSROOT_PATH=/opt/homebrew/Cellar/emscripten/4.0.20/libexec/cache/sysroot \
  /Users/pfeodrippe/dev/jank/compiler+runtime/third-party/cppinterop

emmake make -j8
```

**Libraries Built:**
- `libclangCppInterOp.so` (86.5MB WASM module!) - Full CppInterOp for WASM

#### Test Verification ✅

Test file compiled successfully to WASM:
```cpp
// test_interpreter.cpp - compiles to WASM object file
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Interpreter/Interpreter.h>
// ... uses LLVM 22 API
```

**Estimated Size:** ~100MB WASM bundle (CppInterOp alone is 86MB)

---

### Phase 1: Enable Analyzer in WASM Build

**Current State:** Analyzer is excluded from WASM builds
**Goal:** Include analyzer (it's pure C++, should work)

#### Step 1.1: Modify CMakeLists.txt

```cmake
# Current (around line 300):
if(NOT jank_target_wasm)
  list(APPEND jank_lib_sources
    src/cpp/jank/analyze/processor.cpp
    ...
  )
endif()

# Change to:
list(APPEND jank_lib_sources
  src/cpp/jank/analyze/processor.cpp
  src/cpp/jank/analyze/cpp_util.cpp
  src/cpp/jank/analyze/local_frame.cpp
  # All analyze/expr/*.cpp files
)
```

#### Step 1.2: Handle Conditional Compilation

Check for any JIT-specific code in analyzer:
```cpp
#ifndef JANK_TARGET_WASM
  // JIT-specific code
#else
  // WASM alternative or skip
#endif
```

#### Step 1.3: Test Analyzer in WASM

```clojure
;; Test: analyzer should work
(let [expr '(+ 1 2)]
  (jank.compiler/analyze expr))  ; Returns analyzed expression
```

**Estimated Effort:** 1-2 days

---

### Phase 2: Enable Codegen in WASM Build

**Goal:** Generate C++ code from analyzed expressions in WASM

#### Step 2.1: Include C++ Codegen (processor.cpp)

The C++ string-based codegen (`codegen/processor.cpp`) generates C++ source code. This is what we need for CppInterOp.

```cmake
# Add to WASM build:
list(APPEND jank_lib_sources
  src/cpp/jank/codegen/processor.cpp
)
```

#### Step 2.2: Skip LLVM IR Codegen

The LLVM IR codegen (`codegen/llvm_processor.cpp`) requires full LLVM. Skip it initially:

```cmake
# Only for native builds:
if(NOT jank_target_wasm)
  list(APPEND jank_lib_sources
    src/cpp/jank/codegen/llvm_processor.cpp
  )
endif()
```

#### Step 2.3: Test Codegen in WASM

```clojure
;; Test: codegen should produce C++ string
(let [analyzed (jank.compiler/analyze '(defn foo [x] (+ x 1)))]
  (jank.compiler/codegen analyzed))
;; Returns: "struct foo__1234 { ... };"
```

**Estimated Effort:** 1-2 days

---

### Phase 3: Integrate CppInterOp JIT in WASM

**Goal:** Execute generated C++ code via clang-repl in browser

This is the core challenge. We need to:
1. Link jank with CppInterOp WASM build
2. Adapt `jit/processor.cpp` for WASM
3. Handle WASM-specific constraints

#### Step 3.1: Create WASM JIT Processor

```cpp
// src/cpp/jank/jit/processor_wasm.cpp

#include <clang/Interpreter/CppInterOp.h>

namespace jank::jit
{
  processor::processor()
  {
    // WASM-specific initialization
    std::vector<char const*> args{
      "-target", "wasm32-unknown-emscripten",
      "-std=c++20",
      // Include jank runtime headers (embedded or fetched)
    };

    interpreter.reset(static_cast<Cpp::Interpreter*>(
      Cpp::CreateInterpreter(args, {})
    ));
  }

  void processor::eval_string(jtl::immutable_string const& code) const
  {
    auto err = interpreter->ParseAndExecute({code.data(), code.size()});
    if (err) {
      // Handle error
    }
  }
}
```

#### Step 3.2: Handle WASM Module Loading

In WASM, compiled code becomes a new WebAssembly module that needs to be instantiated:

```cpp
// The compiled WASM needs to be loaded via JavaScript interop
extern "C" {
  void* wasm_instantiate_module(const char* wasm_bytes, size_t size);
  void* wasm_get_function(void* module, const char* name);
}
```

#### Step 3.3: Link with CppInterOp WASM Libraries

```cmake
if(jank_target_wasm)
  target_link_libraries(jank
    ${CPPINTEROP_WASM_DIR}/libCppInterOp.a
    ${LLVM_WASM_DIR}/lib/libclangInterpreter.a
    ${LLVM_WASM_DIR}/lib/libLLVMWebAssemblyCodeGen.a
    # ... other LLVM libs
  )
endif()
```

**Estimated Effort:** 1-2 weeks

---

### Phase 4: Runtime Context Adaptation

**Goal:** Make runtime context work with WASM JIT

#### Step 4.1: Modify context.hpp

```cpp
// include/cpp/jank/runtime/context.hpp

struct context {
#ifdef JANK_TARGET_WASM
  jit::processor jit_prc;  // Now enabled for WASM too!
#else
  jit::processor jit_prc;
#endif
  // ...
};
```

#### Step 4.2: Enable eval_string Full Path

```cpp
// src/cpp/jank/runtime/context.cpp

object_ref context::eval_string(jtl::immutable_string_view const& code)
{
  // Parse
  auto parsed = read_string(code);

#ifdef JANK_TARGET_WASM
  // Full compilation path now works in WASM!
  auto analyzed = analyze(parsed);
  auto cpp_code = codegen(analyzed);
  jit_prc.eval_string(cpp_code);
  // Execute and return result
#else
  // Existing native path
#endif
}
```

**Estimated Effort:** 3-5 days

---

### Phase 5: Header & PCH Handling

**Challenge:** JIT compilation needs C++ headers (jank runtime, standard library)

#### Option A: Embed Headers in WASM Bundle

```cpp
// Embed critical headers at compile time
const char* embedded_headers[] = {
  {"jank/runtime/object.hpp", "...header content..."},
  {"jank/runtime/core.hpp", "...header content..."},
  // ...
};

// Use VFS (Virtual File System) in CppInterOp
for (auto& [name, content] : embedded_headers) {
  vfs[name] = content;
}
```

#### Option B: Fetch Headers via HTTP

```javascript
// JavaScript side
async function loadJankHeaders() {
  const headers = await fetch('/jank-headers.tar.gz');
  // Extract and provide to WASM via Emscripten FS
}
```

#### Option C: Precompiled Header (PCH) for WASM

Create a minimal PCH with common includes:
```cpp
// wasm_pch.hpp
#include <jank/runtime/context.hpp>
#include <jank/runtime/core.hpp>
// ... minimal set
```

**Recommended:** Start with Option A (embedded headers), migrate to PCH for performance.

**Estimated Effort:** 3-5 days

---

### Phase 6: Testing & Validation

#### Test 1: Basic Eval

```clojure
(eval '(+ 1 2))
;; Expected: 3
```

#### Test 2: Define Function at Runtime

```clojure
(eval '(defn runtime-fn [x] (* x x)))
(runtime-fn 5)
;; Expected: 25
```

#### Test 3: Require at Runtime

```clojure
(require '[some.new.namespace :as ns])
(ns/some-fn)
```

#### Test 4: REPL Loop

```javascript
// JavaScript side
const repl = new JankREPL();
const result = await repl.eval("(+ 1 2 3)");
console.log(result); // 6
```

**Estimated Effort:** 1 week

---

## Technical Challenges & Solutions

### Challenge 1: WASM Code Size

**Problem:** LLVM + Clang is huge (~50-100MB)

**Solutions:**
1. **Lazy loading:** Load compiler only when `eval` is first called
2. **Separate bundles:** Core runtime (~5MB) + Compiler extension (~50MB)
3. **Streaming compilation:** Use `WebAssembly.compileStreaming()`
4. **Tree shaking:** Build LLVM with minimal components

```javascript
// Lazy load compiler
async function enableEval() {
  if (!window.jankCompiler) {
    window.jankCompiler = await import('./jank-compiler.wasm');
  }
}
```

### Challenge 2: Memory Limits

**Problem:** WASM has memory limits (default 256MB-4GB)

**Solutions:**
1. Configure larger initial memory: `INITIAL_MEMORY=512MB`
2. Enable memory growth: `ALLOW_MEMORY_GROWTH=1`
3. Implement incremental GC for compiled code

### Challenge 3: No Native Dynamic Linking

**Problem:** WASM can't `dlopen()` like native

**Solutions:**
1. **In-memory linking:** CppInterOp handles this via LLVM ORC JIT
2. **WebAssembly.instantiate():** New modules linked via JS
3. **Symbol table sharing:** Export jank runtime symbols

### Challenge 4: Single-Threaded Execution

**Problem:** WASM is primarily single-threaded

**Solutions:**
1. Compilation is synchronous (blocking) - acceptable for REPL
2. For production: Use Web Workers for background compilation
3. `SharedArrayBuffer` for true threading (requires COOP/COEP headers)

### Challenge 5: Startup Time

**Problem:** Initializing LLVM/Clang is slow (seconds)

**Solutions:**
1. **Pre-initialize:** Load compiler during idle time
2. **Snapshot:** Use Emscripten's memory snapshot feature
3. **Streaming:** `WebAssembly.instantiateStreaming()` starts faster

---

## Bundle Architecture

### Option 1: Single Bundle (Simple)

```
jank-wasm.wasm (100MB+)
├── libjank.a (runtime)
├── libLLVM.a (compiler)
├── libClang.a (C++ parsing)
└── libCppInterOp.a (interpreter)
```

**Pros:** Simple deployment
**Cons:** Large initial download

### Option 2: Split Bundles (Recommended)

```
jank-runtime.wasm (5MB)     ← Always loaded
├── libjank.a
└── Pre-compiled clojure.core

jank-compiler.wasm (80MB)   ← Loaded on-demand
├── libLLVM.a
├── libClang.a
└── libCppInterOp.a
```

**Loading strategy:**
```javascript
// Always load runtime
const runtime = await loadJankRuntime();

// Lazy load compiler when needed
document.getElementById('repl').addEventListener('focus', async () => {
  await runtime.loadCompiler();
});
```

### Option 3: Interpreter Fallback (Alternative)

If LLVM proves too heavy, implement a pure jank interpreter:

```
jank-runtime.wasm (5MB)
├── AOT-compiled code (fast)
└── jank interpreter (for eval)
```

**Trade-offs:**
- Much smaller bundle
- Slower eval execution
- More implementation work

---

## Implementation Timeline (Estimated)

| Phase | Task | Effort | Dependencies |
|-------|------|--------|--------------|
| 0 | Build LLVM + CppInterOp for WASM | 1 week | emsdk 3.1.73 |
| 1 | Enable analyzer in WASM | 1-2 days | Phase 0 |
| 2 | Enable codegen in WASM | 1-2 days | Phase 1 |
| 3 | Integrate CppInterOp JIT | 1-2 weeks | Phase 0, 2 |
| 4 | Runtime context adaptation | 3-5 days | Phase 3 |
| 5 | Header/PCH handling | 3-5 days | Phase 3 |
| 6 | Testing & validation | 1 week | Phase 4, 5 |
| **Total** | | **4-6 weeks** | |

---

## Success Metrics

1. **`eval` works:** `(eval '(+ 1 2))` returns `3` in browser
2. **`defn` at runtime:** Functions can be defined and called
3. **REPL functional:** Interactive coding in browser
4. **Reasonable size:** Compiler bundle < 100MB gzipped
5. **Acceptable startup:** < 5 seconds to first eval
6. **No regressions:** AOT-only mode still works

---

## Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| LLVM WASM build fails | Medium | High | Use emscripten-forge pre-built |
| Bundle too large | High | Medium | Split bundles, lazy loading |
| Performance too slow | Medium | Medium | Profile, optimize hot paths |
| Memory exhaustion | Low | High | Configure larger memory, GC |
| CppInterOp incompatibilities | Low | High | Work with upstream maintainers |

---

## Alternative Approach: Pure jank Interpreter

If CppInterOp/LLVM proves impractical for WASM, consider implementing a pure jank interpreter:

### Interpreter Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    INTERPRETER PATH                         │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  jank source → Parser → Analyzer → Interpreter → Result     │
│                                                             │
│  No C++ codegen, no LLVM                                    │
│  Directly interpret analyzed expressions                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Interpreter Implementation

jank already has `evaluate.cpp` which interprets expressions directly!

```cpp
// src/cpp/jank/evaluate.cpp
object_ref eval(expression_ref e) {
  switch (e->kind) {
    case expression_kind::call:
      return eval_call(e);
    case expression_kind::if_:
      return eval_if(e);
    // ... 30+ expression types
  }
}
```

**This could be the fastest path to WASM eval!**

### Interpreter vs JIT Trade-offs

| Aspect | Interpreter | JIT (CppInterOp) |
|--------|-------------|------------------|
| Bundle size | Small (~5MB) | Large (~100MB) |
| Eval speed | Slow (10-100x) | Fast (native) |
| Implementation | Exists partially | Needs integration |
| cpp/ interop | Limited | Full |
| Startup time | Fast | Slow |

**Recommendation:** Start with interpreter for quick wins, add JIT as optimization.

---

## Appendix: Existing Code References

### JIT Processor (Native)
- File: `src/cpp/jank/jit/processor.cpp`
- Creates `Cpp::Interpreter` from CppInterOp
- Uses `ParseAndExecute()` for C++ evaluation

### C++ Codegen
- File: `src/cpp/jank/codegen/processor.cpp`
- Generates C++ structs for jank functions
- `compilation_target::wasm_aot` already exists!

### Analyzer
- File: `src/cpp/jank/analyze/processor.cpp`
- Pure C++, no JIT dependencies
- Should work in WASM as-is

### Evaluate (Interpreter)
- File: `src/cpp/jank/evaluate.cpp`
- Direct expression interpretation
- Already handles all expression types

### CppInterOp WASM Build
- File: `third-party/cppinterop/Emscripten-build-instructions.md`
- Proven to work with xeus-cpp
- Requires LLVM 19.x patches

---

## Next Steps (Actionable)

1. **Verify CppInterOp WASM build** - Build standalone CppInterOp for WASM, run tests
2. **Enable analyzer in WASM** - Add to CMakeLists, test basic analysis
3. **Enable codegen in WASM** - Add processor.cpp, test code generation
4. **Prototype interpreter eval** - Use existing evaluate.cpp for quick eval
5. **Integrate CppInterOp** - Link and test full JIT path
6. **Benchmark & optimize** - Measure performance, split bundles

---

## Conclusion

Making jank.compiler work in WASM is **feasible** thanks to CppInterOp's existing WASM support. The main challenges are:

1. **Bundle size** - LLVM is large, but manageable with lazy loading
2. **Build complexity** - Multi-stage build (LLVM → CppInterOp → jank)
3. **Integration work** - Adapting jank's JIT processor for WASM

The **fastest path to eval** may be the interpreter approach using existing `evaluate.cpp`, with JIT as a future optimization for performance-critical use cases.

**CppInterOp + clang-repl in WASM = Full C++ REPL in browser = jank REPL in browser!**

---

**Last Updated:** Nov 27, 2025
**Author:** Claude (with jank architecture context)

---

## Phase 0 Completion Summary

**Date:** Nov 27, 2025

### Artifacts Created

| Artifact | Location | Size |
|----------|----------|------|
| LLVM 22 WASM build | `/Users/pfeodrippe/dev/jank/wasm-llvm-build/llvm-build/` | ~500MB |
| CppInterOp WASM build | `/Users/pfeodrippe/dev/jank/wasm-llvm-build/cppinterop-build/` | ~100MB |
| libclangCppInterOp.so | `.../cppinterop-build/lib/` | 86.5MB |

### Files Modified

1. **`llvm-project/llvm/cmake/modules/CrossCompile.cmake`**
   - Fixed native tools build for cross-compilation
   - Changed to use `clang`/`clang++` explicitly

2. **`llvm-project/clang/lib/Interpreter/Wasm.cpp`**
   - Added `/tmp/` prefix for temp object files

3. **`compiler+runtime/third-party/cppinterop/CMakeLists.txt`**
   - Updated version bounds: LLVM 13.0 - 22.x (was 19.1.x)

### Key Learnings

1. LLVM 22 has native WASM interpreter support - simpler than expected!
2. Emscripten 4.0.20 works fine - no need for specific emsdk version
3. CppInterOp builds successfully but tests need API updates for LLVM 22
4. Bundle size is large (~100MB) but manageable with lazy loading

### Next Steps (Phase 1+)

1. Enable jank analyzer in WASM build
2. Enable jank codegen in WASM build
3. Integrate CppInterOp into jank's JIT processor
4. Test end-to-end eval in browser
