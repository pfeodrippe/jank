# Master Plan: Embedded nREPL for WASM

**Goal:** Achieve a fully functional REPL experience for jank running in WebAssembly, enabling interactive development directly in the browser.

**Last Updated:** 2025-11-28
**Status:** Phase 1 COMPLETE ✅ | Phase 2 IN PROGRESS 🚧

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Current State Analysis](#current-state-analysis)
3. [Fundamental Constraints](#fundamental-constraints)
4. [Architecture Overview](#architecture-overview)
5. [Phase 1: Working Prototype](#phase-1-working-prototype-complete-)
6. [Phase 2: Production Readiness](#phase-2-production-readiness-current-)
7. [Phase 3: Compiler in WASM](#phase-3-compiler-in-wasm)
8. [Phase 4: Serverless Future](#phase-4-serverless-future)
9. [Alternative Approaches](#alternative-approaches)
10. [Performance Optimization](#performance-optimization)
11. [Testing Strategy](#testing-strategy)
12. [Migration Path](#migration-path)

---

## Executive Summary

**What Works Now:**
- ✅ Hot-reload system with ~180ms patch compilation
- ✅ Native nREPL server with 19+ operations
- ✅ WASM side module loading via Emscripten dlopen
- ✅ Node.js proxy bridging editor ↔ native jank ↔ browser
- ✅ Real-time function replacement via var registry
- ✅ Full editor integration (CIDER, Calva, etc.)

**The Reality:**
True client-only WASM REPL is **impossible** due to fundamental WASM security restrictions:
- No JIT compilation (no executable memory)
- No dlopen in browser without server (security sandbox)
- No way to generate machine code at runtime

**The Solution:**
Embrace a **hybrid architecture** that provides REPL-like experience through:
1. **Server-side compilation** (native jank or WASM-compiled jank)
2. **WASM side modules** as the "bytecode" format
3. **Hot-reload runtime** for instantaneous code updates
4. **Minimal latency** through caching and optimization

**Target Latency Budget:**
- Current: ~180ms (emcc compile) + ~1ms (dlopen)
- Goal: <100ms total (achievable through caching and optimization)
- Comparable to: Clojure JVM (50-200ms), ClojureScript (100-500ms)

---

## Current State Analysis

### What We Have (commit ca530dce)

#### 1. Native nREPL Server
**Location:** `compiler+runtime/src/cpp/jank/nrepl_server/`

**Features:**
- Full bencode protocol implementation
- 19+ operations: eval, load-file, complete, info, eldoc, etc.
- **New:** `wasm-compile-patch` operation for hot-reload
- Session management with per-session state
- Comprehensive error reporting with stacktraces
- Metadata extraction for symbols

**Status:** Production-ready, 1650 lines of test coverage

#### 2. Hot-Reload System
**Location:** `wasm-clang-interpreter-test/hot-reload-test/`

**Components:**

a) **hot_reload_server.cjs** (693 lines)
   - Three servers: nREPL (7889), WebSocket (7888), HTTP (8080)
   - Forwards nREPL ops to native jank (port 5555)
   - Detects `defn` → triggers wasm-compile-patch
   - Runs emcc to generate WASM side module (~180ms)
   - Broadcasts patches via WebSocket

b) **jank_hot_reload_client.js** (9523 bytes)
   - WebSocket client for browser
   - Receives WASM patches (base64 encoded)
   - Writes to Emscripten VFS
   - Calls `jank_hot_reload_load_patch(path, symbol)`

c) **hot_reload.cpp/hpp** (Runtime)
   - Var registry for function pointer swapping
   - dlopen/dlsym integration
   - 30+ runtime helpers (boxing, calling, data structures)
   - Statistics tracking

d) **wasm_patch_processor.cpp/hpp** (Codegen)
   - Generates C++ from analyzed jank AST
   - Supports: primitives, calls, locals, vectors, sets, do, let, if, fns
   - Uses runtime helpers instead of full jank objects
   - Exports `patch_symbol` metadata for registry

e) **wasm_compile_patch.hpp** (nREPL op)
   - New nREPL operation
   - Parses code, finds def/defn
   - Generates C++ via wasm_patch_processor
   - Returns C++ code + metadata (var name, namespace, etc.)

**Performance:**
- Patch compilation: ~180ms (emcc -O2 -sSIDE_MODULE=1)
- Patch loading: ~1ms (dlopen + dlsym + registry update)
- **Total:** ~181ms (acceptable for REPL workflow)

#### 3. WASM Build Infrastructure
**Location:** `bin/emscripten-bundle`, `CMakeLists.txt`

**Capabilities:**
- AOT compilation: jank → C++ → WASM
- Side module support with `-sSIDE_MODULE=1`
- Emscripten dlopen (MAIN_MODULE + SIDE_MODULE pattern)
- GC integration (Boehm GC via wasm_gc_stub.cpp)
- Platform stubs for browser environment

**Status:** Works for simple programs, needs module system

### What's Missing

1. **Complete Expression Coverage**
   - wasm_patch_processor supports basic forms
   - Missing: loop/recur, try/catch, defprotocol, defmulti, reify, proxy
   - Need: namespace management, require, import

2. **Browser I/O Integration**
   - println → needs console.log bridge
   - File I/O → needs VFS or IndexedDB
   - Network → needs fetch API wrapper

3. **Module System**
   - Loading namespaces as WASM modules
   - Dependency resolution
   - AOT pre-compiled core libraries

4. **Compiler in WASM**
   - Currently requires native jank binary
   - Could compile jank compiler to WASM
   - Still needs emcc (build tool, not embeddable)

5. **Editor Integration Polish**
   - Direct WebSocket from editors (bypass proxy)
   - Browser-based dev tools
   - Source maps for debugging

---

## Fundamental Constraints

### WASM Security Model (Unchangeable)

1. **No JIT Compilation**
   - WASM memory cannot be marked executable
   - Cannot generate machine code at runtime
   - **Implication:** Must pre-compile code server-side

2. **No Dynamic Linking in Browser**
   - Browser sandbox blocks native dlopen
   - Emscripten dlopen only works with `-sMAIN_MODULE`
   - Side modules must be served via HTTP/fetch
   - **Implication:** Need server to serve compiled modules

3. **No Multi-threading (yet)**
   - WASM threads experimental, not widely supported
   - SharedArrayBuffer requires CORS headers
   - **Implication:** Compilation must be async, non-blocking

4. **File System Restrictions**
   - No real filesystem access
   - Emscripten provides virtual FS (in-memory or IndexedDB)
   - **Implication:** Need VFS integration for file operations

### Build Tool Requirements

1. **Emscripten (emcc) Cannot Run in Browser**
   - emcc is a Python/Node.js toolchain
   - Requires LLVM backend (100+ MB binaries)
   - Requires linker (lld)
   - **Implication:** Compilation MUST happen server-side or in web worker with toolchain

2. **Alternative: WebAssembly Compilation**
   - Could compile LLVM to WASM (attempted in several projects)
   - Results in 50-100MB WASM file
   - Slow initialization (10-30 seconds)
   - **Implication:** Not practical for interactive REPL

### Network Latency

1. **Round-trip Time**
   - Editor → Server: ~10-50ms (local network)
   - Compilation: ~180ms (current)
   - Server → Browser: ~10-50ms
   - **Total:** ~200-280ms worst case

2. **Optimization Opportunities**
   - Caching compiled modules: ~0ms for unchanged code
   - Incremental compilation: ~50-100ms for small changes
   - Pre-compilation: ~0ms for library code

---

## Architecture Overview

### Three-Tier Architecture (Current)

```
┌─────────────────────────────────────────────────────────────┐
│ TIER 1: EDITOR (Emacs, VSCode, etc.)                        │
│   - CIDER, Calva, vim-iced, etc.                            │
│   - Sends nREPL messages over TCP                           │
│   - Port: 7889 (hot_reload_server)                          │
└────────────────────────┬────────────────────────────────────┘
                         │ nREPL Protocol (bencode)
                         ▼
┌─────────────────────────────────────────────────────────────┐
│ TIER 2: COMPILATION SERVER (Node.js or native)              │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ hot_reload_server.cjs (Node.js)                      │  │
│  │  - nREPL proxy (7889) → native jank (5555)          │  │
│  │  - WebSocket server (7888) → browsers                │  │
│  │  - HTTP server (8080) → static files + WASM modules  │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│  ┌──────────────▼───────────────────────────────────────┐  │
│  │ jank Native Binary (./build/jank repl --server)      │  │
│  │  - Full jank compiler                                │  │
│  │  - nREPL engine (port 5555)                          │  │
│  │  - wasm-compile-patch operation                      │  │
│  │  - AST analysis + C++ codegen                        │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│                 │ C++ code                                   │
│  ┌──────────────▼───────────────────────────────────────┐  │
│  │ Emscripten (emcc)                                    │  │
│  │  emcc -sSIDE_MODULE=1 -O2 -fPIC -o patch.wasm        │  │
│  │  ~180ms compilation time                             │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
└─────────────────┼────────────────────────────────────────────┘
                  │ WASM binary (base64 over WebSocket)
                  ▼
┌─────────────────────────────────────────────────────────────┐
│ TIER 3: BROWSER RUNTIME (Chrome, Firefox, Safari)           │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ jank.wasm (Main Module)                              │  │
│  │  - Full jank runtime                                 │  │
│  │  - Var registry                                      │  │
│  │  - Hot-reload infrastructure                         │  │
│  │  - Emscripten dlopen support                         │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│  ┌──────────────▼───────────────────────────────────────┐  │
│  │ jank_hot_reload_client.js                            │  │
│  │  - WebSocket connection to server                    │  │
│  │  - Receives WASM patches                             │  │
│  │  - Writes to Emscripten VFS                          │  │
│  │  - Calls jank_hot_reload_load_patch()                │  │
│  └──────────────┬───────────────────────────────────────┘  │
│                 │                                            │
│  ┌──────────────▼───────────────────────────────────────┐  │
│  │ Hot-Reload Runtime (C++)                             │  │
│  │  1. dlopen("patch.wasm")                             │  │
│  │  2. dlsym(handle, "jank_patch_symbols_123")          │  │
│  │  3. Extract function pointer from symbols            │  │
│  │  4. Update var registry                              │  │
│  │  5. New code is live! (~1ms)                         │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### Data Flow for `(defn foo [x] (+ x 1))` Evaluation

```
1. Editor → hot_reload_server (port 7889)
   Message: {op: "eval", code: "(defn foo [x] (+ x 1))"}

2. hot_reload_server → jank native (port 5555)
   Detects defn → converts to:
   Message: {op: "wasm-compile-patch", code: "(defn foo [x] (+ x 1))"}

3. jank native → AST analysis
   - Parse code
   - Find def expression: (defn foo ...)
   - Extract var name: "foo"
   - Extract namespace: "user" (from session state)
   - Analyze function: [x] → (+ x 1)

4. jank native → C++ generation (wasm_patch_processor)
   Generates:
   ```cpp
   extern "C" {
     struct patch_symbol {
       const char* qualified_name;
       void* fn_ptr;
     };

     jank_object_ptr jank_user_foo_123(jank_object_ptr x) {
       return jank_call_var("clojure.core/+", x, jank_box_integer(1));
     }

     patch_symbol jank_patch_symbols_123[] = {
       {"user/foo", (void*)jank_user_foo_123},
       {nullptr, nullptr}
     };
   }
   ```

5. jank native → hot_reload_server
   Response: {cpp-code: "...", var-name: "foo", ns-name: "user", ...}

6. hot_reload_server → emcc
   $ emcc generated_patch_123.cpp -o patch_123.wasm \
       -sSIDE_MODULE=1 -O2 -fPIC
   Time: ~180ms

7. hot_reload_server → browser (WebSocket)
   Message: {
     data: "<base64-encoded-wasm>",
     symbolName: "jank_patch_symbols_123",
     varName: "user/foo"
   }

8. Browser → jank_hot_reload_client.js
   - Decode base64 → binary
   - Write to VFS: /patches/patch_123.wasm
   - Call: jank_hot_reload_load_patch("/patches/patch_123.wasm",
                                       "jank_patch_symbols_123")

9. Browser → hot_reload.cpp
   ```cpp
   void* handle = dlopen("/patches/patch_123.wasm", RTLD_NOW);
   patch_symbol* symbols = (patch_symbol*)dlsym(handle, "jank_patch_symbols_123");

   for (int i = 0; symbols[i].qualified_name != nullptr; i++) {
     // symbols[0] = {"user/foo", &jank_user_foo_123}
     var* v = find_or_create_var(symbols[i].qualified_name);
     v->root = make_function_wrapper(symbols[i].fn_ptr, arity_info);
   }
   ```

10. Result: `user/foo` now points to new implementation!
    Next call to `(foo 10)` → returns 11 ✅
```

---

## Phase 1: Working Prototype (COMPLETE ✅)

**Status:** Completed in commit ca530dce "Fix nREPL"

**What Was Built:**

### 1.1 nREPL wasm-compile-patch Operation
- [x] Added `wasm_compile_patch.hpp` operation handler
- [x] Integration with nREPL engine
- [x] Comprehensive tests (test/cpp/jank/nrepl/engine.cpp)
- [x] Bencode protocol support

### 1.2 WASM Patch Codegen
- [x] wasm_patch_processor for C++ generation
- [x] Support for: primitives, arithmetic, locals, vectors, sets
- [x] Support for: do, let, if, fn (fixed arity)
- [x] Runtime helpers: boxing, unboxing, var calls
- [x] Unique symbol naming (jank_patch_symbols_N)

### 1.3 Hot-Reload Runtime
- [x] Var registry with function pointer swapping
- [x] dlopen/dlsym integration
- [x] load_patch() function
- [x] C API exports for WASM
- [x] Statistics tracking

### 1.4 Development Server
- [x] hot_reload_server.cjs with three protocols
- [x] nREPL proxy (editor ↔ jank)
- [x] WebSocket server (server ↔ browser)
- [x] HTTP server for static files
- [x] emcc integration (~180ms compile)
- [x] Patch broadcasting

### 1.5 Browser Client
- [x] jank_hot_reload_client.js
- [x] WebSocket connection
- [x] Patch reception and loading
- [x] VFS integration
- [x] Error handling

### 1.6 Documentation
- [x] wasm-clang-interpreter-test/hot-reload-test/README.md
- [x] Critical sections on dlsym caching
- [x] Example patches and tests

**Validation:**
- ✅ End-to-end test: Editor → defn → browser update in ~180ms
- ✅ Multiple patches work with unique symbol names
- ✅ Var registry correctly updates function pointers
- ✅ No regressions in native nREPL functionality

---

## Phase 2: Production Readiness (CURRENT 🚧)

**Goal:** Make the hot-reload system robust enough for real development work.

**Timeline:** 4-6 weeks of focused development

### 2.1 Complete Expression Coverage (Week 1-2)

**Priority:** HIGH - Blocking for practical use

**Tasks:**

#### 2.1.1 Control Flow
- [ ] **loop/recur**
  - Track loop bindings in codegen context
  - Generate while loop with mutable bindings
  - recur → update bindings + continue
  - Estimated: 2-3 days

- [ ] **try/catch/finally**
  - C++ exception handling
  - jank exception object wrapping
  - finally block execution
  - Estimated: 2-3 days

- [ ] **throw**
  - Create jank exception object
  - C++ throw integration
  - Estimated: 1 day

#### 2.1.2 Data Structures (Extended)
- [ ] **Maps** `{:a 1 :b 2}`
  - jank_make_hash_map() helper
  - Compile-time vs runtime map creation
  - Estimated: 1 day

- [ ] **Lists** `'(1 2 3)`
  - jank_make_list() helper
  - Quote handling
  - Estimated: 1 day

- [ ] **Lazy sequences**
  - Integration with existing seq abstraction
  - Estimated: 2 days

#### 2.1.3 Functions (Advanced)
- [ ] **Variable arity** `(defn foo [x & more] ...)`
  - va_args handling in C++
  - jank_object_ptr* array for rest args
  - Estimated: 2-3 days

- [ ] **Multi-arity** `(defn foo ([x] ...) ([x y] ...))`
  - Generate multiple C++ functions
  - Dispatch based on arg count
  - Estimated: 2 days

- [ ] **Closures**
  - Capture variables from outer scope
  - Heap-allocated closure context
  - jank_make_closure() helper
  - Estimated: 3-4 days (complex!)

#### 2.1.4 Namespace Management
- [ ] **ns declaration** `(ns my.namespace)`
  - Update codegen context with current namespace
  - Generate qualified names
  - Estimated: 1 day

- [ ] **require** `(require '[foo.bar :as fb])`
  - Load pre-compiled namespace module
  - Alias resolution
  - Estimated: 2 days

- [ ] **import** `(import java.util.Date)` (for native interop)
  - Class name resolution
  - Estimated: 1 day

- [ ] **refer** `(refer 'clojure.set :only [union])`
  - Symbol aliasing in var registry
  - Estimated: 1 day

#### 2.1.5 Advanced Features
- [ ] **defprotocol/deftype/defrecord**
  - Generate C++ structs
  - Protocol dispatch tables
  - Estimated: 5-7 days (major feature)

- [ ] **defmulti/defmethod**
  - Multimethod registry
  - Runtime dispatch
  - Estimated: 3-4 days

- [ ] **reify**
  - Anonymous type generation
  - Estimated: 2-3 days

**Acceptance Criteria:**
- [ ] All clojure.core functions can be redefined via patches
- [ ] Can evaluate arbitrary clojure code (within single namespace)
- [ ] Comprehensive test suite (200+ test cases)

**Risk Mitigation:**
- Start with most commonly used features (loop, maps, closures)
- Incremental implementation with tests
- Fallback: Mark unsupported forms as "not yet implemented" errors

---

### 2.2 Browser I/O Integration (Week 2-3)

**Priority:** MEDIUM - Needed for practical debugging

**Tasks:**

#### 2.2.1 Console Output
- [ ] **println → console.log**
  - Intercept jank println
  - Format jank objects as strings
  - Call JavaScript console.log via Emscripten EM_JS
  - Estimated: 1 day

- [ ] **prn → console.log** (readable format)
  - Estimated: 1 day

- [ ] **Error output → console.error**
  - Estimated: 0.5 days

#### 2.2.2 REPL Output Capture
- [ ] **Capture eval results**
  - Buffer output during patch execution
  - Send via WebSocket to editor
  - Display in REPL buffer
  - Estimated: 2 days

- [ ] **Stdout/stderr separation**
  - Separate channels for output vs errors
  - Estimated: 1 day

#### 2.2.3 File I/O (Virtual FS)
- [ ] **slurp → Emscripten VFS**
  - Read from virtual filesystem
  - Support for IndexedDB persistence
  - Estimated: 2 days

- [ ] **spit → Emscripten VFS**
  - Write to virtual filesystem
  - Estimated: 1 day

- [ ] **File browser API**
  - Load files from user's computer (File API)
  - Estimated: 2 days

#### 2.2.4 Network I/O
- [ ] **HTTP client → fetch API**
  - Wrap JavaScript fetch in jank function
  - Promise → callback integration
  - Estimated: 3 days

- [ ] **WebSocket support**
  - Expose WebSocket API to jank code
  - Estimated: 2 days

**Acceptance Criteria:**
- [ ] Can debug patch execution via console.log
- [ ] REPL shows eval results in editor
- [ ] Can load/save files from browser
- [ ] Can make HTTP requests from jank code

---

### 2.3 Module System (Week 3-4)

**Priority:** HIGH - Critical for code organization

**Tasks:**

#### 2.3.1 Pre-compiled Core Libraries
- [ ] **Compile clojure.core as WASM module**
  - AOT compile all clojure.core functions
  - Generate single core.wasm module
  - Load at startup (~100ms)
  - Estimated: 3 days

- [ ] **Compile clojure.string, clojure.set, etc.**
  - Standard library modules
  - Estimated: 2 days per module

- [ ] **Module loading infrastructure**
  - Dependency graph resolution
  - Parallel loading
  - Estimated: 2 days

#### 2.3.2 User Namespace Modules
- [ ] **Namespace → WASM module mapping**
  - my.namespace → my_namespace.wasm
  - HTTP serving from /modules/ path
  - Estimated: 2 days

- [ ] **require → dlopen**
  - Load namespace WASM module
  - Register all vars from module
  - Estimated: 2 days

- [ ] **Incremental loading**
  - Only load required namespaces
  - Lazy loading support
  - Estimated: 2 days

#### 2.3.3 Hot-Reload for Namespaces
- [ ] **Reload entire namespace**
  - Recompile all defs in namespace
  - Update all vars atomically
  - Estimated: 2 days

- [ ] **Dependency tracking**
  - Reload dependent namespaces
  - Invalidate caches
  - Estimated: 3 days

**Acceptance Criteria:**
- [ ] Can organize code into multiple namespaces
- [ ] Core libraries load in <200ms
- [ ] require works like in native jank
- [ ] Hot-reload preserves state across namespace reloads

---

### 2.4 Error Handling & Debugging (Week 4-5)

**Priority:** MEDIUM - Quality of life improvement

**Tasks:**

#### 2.4.1 Source Maps
- [ ] **C++ → jank source mapping**
  - Embed source location metadata in patches
  - Map C++ line numbers to jank code
  - Estimated: 3 days

- [ ] **Browser DevTools integration**
  - Show jank source in debugger
  - Estimated: 2 days

#### 2.4.2 Stack Traces
- [ ] **Capture stack on exception**
  - C++ stack unwinding
  - Extract function names from var registry
  - Estimated: 2 days

- [ ] **Pretty-print stack traces**
  - Format for readability
  - Show source locations
  - Estimated: 1 day

#### 2.4.3 Error Recovery
- [ ] **Graceful patch load failures**
  - Don't crash on compilation errors
  - Show error in editor
  - Keep old implementation
  - Estimated: 2 days

- [ ] **Runtime error handling**
  - try/catch in patch execution
  - Report errors via nREPL
  - Estimated: 2 days

**Acceptance Criteria:**
- [ ] Compilation errors show in editor with source location
- [ ] Runtime errors show readable stack traces
- [ ] Failed patch loads don't crash the app
- [ ] Can set breakpoints in browser DevTools

---

### 2.5 Performance Optimization (Week 5-6)

**Priority:** MEDIUM - Improve developer experience

**Tasks:**

#### 2.5.1 Compilation Caching
- [ ] **Cache compiled WASM modules**
  - Hash of source code → cached WASM
  - Serve from cache if unchanged (~0ms)
  - Estimated: 2 days

- [ ] **Incremental compilation**
  - Only recompile changed functions
  - Estimated: 3 days (complex)

- [ ] **Persistent cache**
  - Save cache to disk (Node.js)
  - Save to IndexedDB (browser)
  - Estimated: 2 days

#### 2.5.2 emcc Optimization
- [ ] **Reduce emcc startup time**
  - Keep emcc process running (daemon mode)
  - Estimated: 2 days

- [ ] **Parallel compilation**
  - Compile multiple patches in parallel
  - Estimated: 1 day

- [ ] **Optimize emcc flags**
  - Experiment with -O1, -O2, -O3, -Oz
  - Balance size vs speed
  - Estimated: 1 day

#### 2.5.3 Network Optimization
- [ ] **Compression**
  - gzip WASM modules before sending
  - Estimated: 1 day

- [ ] **Batch updates**
  - Combine multiple patches into one WASM module
  - Estimated: 2 days

- [ ] **Predictive pre-loading**
  - Load frequently used namespaces speculatively
  - Estimated: 2 days

**Acceptance Criteria:**
- [ ] Unchanged code evaluates in <10ms (cache hit)
- [ ] Changed code evaluates in <100ms (optimized compile)
- [ ] Batch updates reduce latency for multi-defn files

**Target Performance:**
```
Operation                    | Before   | After
-----------------------------|----------|--------
Cache hit (unchanged code)   | 180ms    | <10ms
Small change (1 function)    | 180ms    | <100ms
Large change (10 functions)  | 1800ms   | <300ms (batched)
Namespace load (clojure.core)| N/A      | <200ms
```

---

### 2.6 Testing & Quality Assurance (Week 6)

**Priority:** HIGH - Must be stable for release

**Tasks:**

#### 2.6.1 Unit Tests
- [ ] **wasm_patch_processor tests**
  - 100+ test cases for all expression types
  - Edge cases: nested closures, recursive functions, etc.
  - Estimated: 3 days

- [ ] **hot_reload tests**
  - Patch loading, var registry, statistics
  - Estimated: 2 days

- [ ] **nREPL integration tests**
  - wasm-compile-patch op
  - Error handling
  - Estimated: 2 days

#### 2.6.2 End-to-End Tests
- [ ] **Browser automation**
  - Puppeteer or Playwright
  - Test full workflow: editor → server → browser
  - Estimated: 3 days

- [ ] **Performance benchmarks**
  - Track latency over time
  - Regression detection
  - Estimated: 2 days

#### 2.6.3 Documentation
- [ ] **User guide**
  - Setup instructions
  - Editor configuration (CIDER, Calva)
  - Troubleshooting
  - Estimated: 2 days

- [ ] **Developer guide**
  - Architecture documentation
  - How to add new expression types
  - How to debug patches
  - Estimated: 2 days

- [ ] **API reference**
  - nREPL operations
  - Hot-reload C API
  - JavaScript API
  - Estimated: 1 day

**Acceptance Criteria:**
- [ ] 90%+ test coverage for new code
- [ ] All tests pass on CI
- [ ] Documentation is complete and accurate
- [ ] Can onboard a new developer in <1 hour

---

## Phase 3: Compiler in WASM (Future)

**Goal:** Run jank compiler entirely in browser (eliminate native server dependency).

**Timeline:** 8-12 weeks

**Status:** BLOCKED - Requires Phase 2 completion

### 3.1 Prerequisites

**Before starting Phase 3:**
- [x] Phase 1 complete
- [ ] Phase 2 complete (all production features working)
- [ ] Performance acceptable (<100ms compile time)
- [ ] Comprehensive test coverage

### 3.2 Compile jank Compiler to WASM

**Challenge:** jank compiler is a large C++ codebase with many dependencies.

**Tasks:**

#### 3.2.1 Remove Native Dependencies
- [ ] **Replace filesystem access**
  - Use Emscripten VFS instead of std::filesystem
  - Estimated: 3 days

- [ ] **Replace network I/O**
  - Use fetch API instead of ASIO
  - Estimated: 3 days

- [ ] **Replace process spawning**
  - No subprocesses in WASM (emcc is external)
  - Estimated: 1 day

#### 3.2.2 Build jank as WASM Library
- [ ] **CMake configuration**
  - Add JANK_TARGET_WASM build option
  - Disable native-only features
  - Estimated: 2 days

- [ ] **Compile to WASM**
  - emcc build of entire compiler
  - Size optimization (likely 10-20 MB)
  - Estimated: 5 days (debugging build issues)

- [ ] **C API for compiler**
  - Expose compile_to_cpp() function
  - Estimated: 2 days

#### 3.2.3 Integration
- [ ] **Load compiler.wasm in browser**
  - Initial load: 10-30 seconds (large file)
  - Estimate: 2 days

- [ ] **Call compiler from JavaScript**
  - Pass source code string
  - Receive C++ code string
  - Estimated: 1 day

- [ ] **Replace native jank with WASM jank**
  - hot_reload_server.cjs calls WASM compiler instead of native
  - Estimated: 2 days

**Acceptance Criteria:**
- [ ] jank compiler runs in browser
- [ ] Compilation time comparable to native (<300ms)
- [ ] WASM bundle size <20 MB (compressed)
- [ ] Initial load time <5 seconds on modern hardware

**Risk Assessment:**
- **HIGH:** Large WASM bundle may be slow to load
- **MEDIUM:** Compilation may be slower than native (V8 JIT vs native code)
- **LOW:** Memory usage (WASM has 4GB limit, should be sufficient)

**Fallback:**
- Keep native server as option for power users
- Offer "fast mode" (native) vs "portable mode" (WASM)

---

### 3.3 Optimize WASM Compiler Performance

**Tasks:**

#### 3.3.1 Code Splitting
- [ ] **Lazy load compiler modules**
  - Parser, analyzer, codegen as separate WASM modules
  - Load on demand
  - Estimated: 3 days

#### 3.3.2 Compilation Optimization
- [ ] **Optimize C++ codegen**
  - Reduce string allocations
  - Reuse buffers
  - Estimated: 3 days

- [ ] **Parallel compilation**
  - Use Web Workers for concurrent compilation
  - Estimated: 5 days

#### 3.3.3 Caching
- [ ] **Cache AST**
  - Parse once, analyze many times
  - Estimated: 2 days

- [ ] **Cache C++ code**
  - Same as Phase 2.5.1
  - Estimated: 0 days (already done)

**Target Performance:**
```
Operation                    | Native  | WASM
-----------------------------|---------|--------
First compilation (cold)     | 180ms   | <300ms
Cached compilation           | 10ms    | <20ms
Initial compiler load        | 0ms     | <5s
```

---

## Phase 4: Serverless Future (Speculative)

**Goal:** Eliminate server entirely, run everything in browser.

**Timeline:** 12+ weeks

**Status:** BLOCKED - Requires Phase 3 completion

**Major Challenge:** emcc cannot run in browser.

### 4.1 WebAssembly-to-WebAssembly Compiler

**The Problem:**
- emcc is a Python/Node.js toolchain (cannot run in browser)
- Requires LLVM backend (100+ MB native binaries)
- Requires linker (lld)

**Potential Solutions:**

#### Option A: Port emcc to WASM
- [ ] **Compile emcc toolchain to WASM**
  - Python interpreter in WASM (Pyodide exists!)
  - LLVM in WASM (attempted by several projects, 50-100 MB)
  - lld in WASM
  - Estimated: 20+ weeks (massive undertaking)

**Status:** Impractical for jank project alone. Would require collaboration with Emscripten team.

#### Option B: Custom WASM-to-WASM Compiler
- [ ] **Write custom code generator**
  - C++ → WASM directly (bypass emcc)
  - Use binaryen library (already in WASM)
  - Estimated: 12+ weeks

**Status:** Possible but significant engineering effort. Would be a novel contribution to ecosystem.

#### Option C: Use Existing WASM JIT
- [ ] **Leverage browser's WASM JIT**
  - Compile jank → WASM text format (WAT)
  - Use wabt.js to assemble WAT → WASM binary
  - Estimated: 8 weeks

**Status:** Most practical approach. WASM text format is human-readable and easier to generate than binary.

### 4.2 WAT Code Generator (Recommended Path)

**Architecture:**

```
┌─────────────────────────────────────────────────────────────┐
│ Browser                                                      │
│                                                              │
│  jank code → jank compiler (WASM) → WAT text                │
│  WAT text → wabt.js (WASM assembler) → WASM binary          │
│  WASM binary → WebAssembly.instantiate() → executable code  │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

**Tasks:**

#### 4.2.1 WAT Code Generator
- [ ] **Design WAT template**
  - Function signatures
  - Memory management
  - Imports/exports
  - Estimated: 3 days

- [ ] **Implement expression → WAT mapping**
  - Primitives: i32.const, f64.const, etc.
  - Arithmetic: i32.add, i32.mul, etc.
  - Calls: call, call_indirect
  - Control flow: if, loop, br
  - Estimated: 5 days

- [ ] **Runtime integration**
  - Interface with jank runtime (memory, GC, etc.)
  - Estimated: 5 days

#### 4.2.2 Integration with wabt.js
- [ ] **Load wabt.js**
  - WASM assembler library (~1 MB)
  - Estimated: 1 day

- [ ] **Assemble WAT → WASM**
  - Parse WAT text
  - Generate binary
  - Estimated: 2 days

- [ ] **Instantiate WASM module**
  - WebAssembly.instantiate()
  - Link with runtime
  - Estimated: 2 days

#### 4.2.3 Testing
- [ ] **Comprehensive test suite**
  - All expression types
  - Edge cases
  - Estimated: 5 days

**Acceptance Criteria:**
- [ ] Can compile jank code to WASM entirely in browser
- [ ] No server required (static file hosting only)
- [ ] Compilation time <200ms
- [ ] Generated WASM code is correct and performant

**Timeline:** 8-10 weeks for WAT generator approach

---

### 4.3 Deployment Model (Static Hosting)

**Once Phase 4 is complete:**

```
┌─────────────────────────────────────────────────────────────┐
│ CDN / Static File Hosting (GitHub Pages, Netlify, etc.)     │
│                                                              │
│  Files:                                                      │
│    - index.html                                              │
│    - jank.wasm (main runtime + compiler)                    │
│    - jank.js (Emscripten glue code)                         │
│    - wabt.js (WASM assembler)                               │
│    - core.wasm (pre-compiled clojure.core)                  │
│    - app code (user's jank source files)                    │
│                                                              │
│  No backend required!                                        │
└──────────────────────────────────────────────────────────────┘
```

**Benefits:**
- Zero server costs (static hosting is free/cheap)
- Infinite scalability (CDN handles traffic)
- Works offline (Progressive Web App)
- Easy deployment (git push)

**Tradeoffs:**
- Large initial download (20-30 MB for compiler + runtime)
- Slower compilation than native
- Requires modern browser (WASM, SharedArrayBuffer)

---

## Alternative Approaches

### Option 1: Interpreter-Based REPL (Abandoned)

**Idea:** Interpret jank code directly in WASM without compilation.

**Implementation:**
- Write interpreter in C++ (compile to WASM)
- Walk AST and execute each node
- No code generation needed

**Pros:**
- No compilation step (0ms compile time)
- Simpler implementation
- Smaller bundle size

**Cons:**
- **10-100x slower execution** (interpretation overhead)
- Unacceptable for real applications
- Still need parser/analyzer (most of compiler complexity)

**Decision:** Rejected. Performance is critical for usability.

---

### Option 2: Pre-compilation with Dynamic Linking (Current Approach ✅)

**Idea:** Pre-compile code to WASM on server, dynamically link in browser.

**Implementation:**
- Server compiles jank → C++ → WASM
- Browser loads WASM via dlopen (Emscripten MAIN_MODULE + SIDE_MODULE)
- Var registry swaps function pointers

**Pros:**
- **Fast execution** (native WASM speed)
- **Acceptable compile time** (~180ms)
- Works today with existing tools (emcc)
- Proven architecture (used by ClojureScript, others)

**Cons:**
- Requires server for compilation
- Network latency for remote servers
- Not fully self-contained

**Decision:** Chosen for Phases 1-3. Best tradeoff between complexity and performance.

---

### Option 3: Source-to-Source Compilation (ClojureScript approach)

**Idea:** Compile jank → JavaScript, execute in browser.

**Implementation:**
- jank compiler generates JavaScript instead of C++
- Browser executes JavaScript (JIT compiled by V8)
- No WASM involved

**Pros:**
- Could reuse ClojureScript infrastructure
- JavaScript JIT is very fast
- No compilation server needed (JavaScript is interpreted)

**Cons:**
- **Abandons jank's C++ runtime** (not jank anymore, it's ClojureScript!)
- Loses native interop capabilities
- Loses performance benefits of WASM

**Decision:** Not viable. This would make jank = ClojureScript. The whole point of jank is native performance.

---

### Option 4: Hybrid Interpretation + JIT

**Idea:** Interpret code on first run, compile hot functions to WASM.

**Implementation:**
- Interpreter for initial execution
- Profile execution to find hot spots
- Compile hot functions to WASM (on server or in browser)
- Switch to compiled version dynamically

**Pros:**
- Fast startup (no compilation)
- Fast steady-state (compiled hot code)
- Adaptive optimization (like JVM)

**Cons:**
- **Extremely complex** (two execution engines)
- Requires profiling infrastructure
- Unpredictable performance (depends on heuristics)
- Still slower than ahead-of-time compilation for small programs

**Decision:** Interesting for future optimization, but too complex for initial implementation.

---

### Option 5: WebAssembly Text Format (WAT) Direct Generation

**Idea:** Generate WASM text format directly, skip C++ intermediate.

**Implementation:**
- jank compiler → WAT (WASM text format)
- wabt.js assembles WAT → WASM binary
- No emcc needed!

**Pros:**
- Eliminates emcc dependency
- Can run entirely in browser
- Simpler toolchain

**Cons:**
- Need to write new code generator (significant work)
- Lose access to C++ standard library
- Manual memory management in WAT

**Decision:** Best option for Phase 4 (serverless future). Too complex for Phase 1-3.

---

## Performance Optimization

### Latency Budget Breakdown

**Current (Phase 1):**
```
Editor → Server:          10-50ms   (network latency)
Parse + Analyze:          5-10ms    (jank compiler)
C++ Codegen:              5-10ms    (wasm_patch_processor)
emcc Compile:             150-200ms (WASM generation)
Server → Browser:         10-50ms   (network latency)
dlopen + dlsym:          1-2ms     (WASM loading)
Registry Update:          <1ms      (pointer swap)
─────────────────────────────────────────────
TOTAL:                    181-323ms
```

**Target (Phase 2):**
```
Cache Hit:                <10ms     (serve from cache)
Cache Miss (optimized):   <100ms    (faster emcc, incremental)
```

**Target (Phase 3 - WASM Compiler):**
```
First Load:               3-5s      (load compiler.wasm)
Subsequent:               <200ms    (compile in WASM)
```

**Target (Phase 4 - WAT Generator):**
```
Generate WAT:             20-50ms   (direct generation)
Assemble WASM:            30-100ms  (wabt.js)
Instantiate:              5-10ms    (WebAssembly API)
─────────────────────────────────────────────
TOTAL:                    55-160ms  (BEST CASE)
```

### Optimization Techniques

#### 1. Compilation Caching
```javascript
// hot_reload_server.cjs
const cache = new Map(); // hash → WASM binary

async function compilePatch(cppCode, hash) {
  if (cache.has(hash)) {
    return cache.get(hash); // 0ms!
  }

  const wasm = await runEmcc(cppCode); // 180ms
  cache.set(hash, wasm);
  return wasm;
}
```

**Impact:** Unchanged code goes from 180ms → <1ms

#### 2. Incremental Compilation
```
Change: (defn foo [x] (+ x 1)) → (defn foo [x] (+ x 2))
        ^^^^^^^^^^^^^^^^^^^^^^^
        Only this changed!

Optimization:
  - Reuse parsed AST for unchanged functions
  - Only re-generate C++ for changed function
  - Link with cached object files

Result: 180ms → 50ms
```

#### 3. Parallel Compilation
```javascript
// Compile multiple patches concurrently
async function compilePatches(patches) {
  return Promise.all(patches.map(p => compilePatch(p)));
}

// 10 patches × 180ms = 1800ms sequentially
// 10 patches × 180ms = 200ms in parallel (8 cores)
```

#### 4. Code Splitting
```
Instead of:
  jank.wasm (30 MB) - takes 5s to load

Do:
  jank-runtime.wasm (2 MB) - loads in 500ms
  jank-compiler.wasm (10 MB) - loads on demand
  clojure-core.wasm (5 MB) - loads on demand
```

#### 5. Predictive Pre-loading
```javascript
// Detect when user opens a file
onFileOpen("my_namespace.jank") => {
  // Pre-compile in background
  preloadNamespace("my_namespace");

  // Pre-load dependencies
  preloadNamespace("clojure.core");
  preloadNamespace("clojure.string");
}

// When user evals, code is already compiled!
```

---

## Testing Strategy

### Unit Tests

**wasm_patch_processor (codegen)**
```cpp
TEST_CASE("Generate C++ for primitive values") {
  // Input: 42
  auto result = generate("42", context);
  REQUIRE(result.contains("jank_box_integer(42)"));
}

TEST_CASE("Generate C++ for function call") {
  // Input: (+ 1 2)
  auto result = generate("(+ 1 2)", context);
  REQUIRE(result.contains("jank_call_var(\"clojure.core/+\""));
}

TEST_CASE("Generate C++ for let binding") {
  // Input: (let [x 10] (+ x 1))
  auto result = generate("(let [x 10] (+ x 1))", context);
  REQUIRE(result.contains("jank_object_ptr x = jank_box_integer(10)"));
}

// 100+ similar tests for all expression types
```

**hot_reload (runtime)**
```cpp
TEST_CASE("Load patch and update var") {
  auto patch_path = "/tmp/test_patch.wasm";
  auto symbol_name = "jank_patch_symbols_1";

  load_patch(patch_path, symbol_name);

  auto var = find_var("user/foo");
  REQUIRE(var != nullptr);
  REQUIRE(var->root != nullptr);
}

TEST_CASE("Multiple patches with same var name") {
  load_patch("patch1.wasm", "jank_patch_symbols_1");
  load_patch("patch2.wasm", "jank_patch_symbols_2");

  auto var = find_var("user/foo");
  // Should have latest implementation
  auto result = call_var(var, make_integer(10));
  REQUIRE(unbox_integer(result) == 12); // from patch2
}
```

**nREPL (protocol)**
```cpp
TEST_CASE("wasm-compile-patch operation") {
  auto request = bencode::encode({
    {"op", "wasm-compile-patch"},
    {"code", "(defn foo [x] (+ x 1))"},
    {"session", "test-session"}
  });

  auto response = engine.handle_message(request);

  REQUIRE(response.contains("cpp-code"));
  REQUIRE(response.contains("var-name"));
  REQUIRE(response["var-name"] == "foo");
}
```

### Integration Tests

**End-to-End Workflow**
```javascript
// test/e2e/hot_reload_test.js
describe('Hot Reload', () => {
  it('should update function in browser', async () => {
    // 1. Start servers
    const jankServer = spawn('./build/jank', ['repl', '--server']);
    const hotReloadServer = spawn('node', ['hot_reload_server.cjs']);

    // 2. Open browser with Puppeteer
    const browser = await puppeteer.launch();
    const page = await browser.newPage();
    await page.goto('http://localhost:8080');

    // 3. Send nREPL message
    const client = new NREPLClient('localhost', 7889);
    const response = await client.eval('(defn foo [x] (+ x 1))');

    // 4. Wait for patch to load in browser
    await page.waitForFunction(() => {
      return window.jank_get_var('user/foo') !== undefined;
    });

    // 5. Call function in browser
    const result = await page.evaluate(() => {
      return window.jank_call('user/foo', 10);
    });

    expect(result).toBe(11);
  });
});
```

### Performance Tests

**Latency Benchmarks**
```javascript
// test/perf/latency_test.js
describe('Compilation Latency', () => {
  it('should compile small function in <100ms', async () => {
    const start = Date.now();
    await compilePatch('(defn foo [x] (+ x 1))');
    const elapsed = Date.now() - start;

    expect(elapsed).toBeLessThan(100);
  });

  it('should serve cached patches in <10ms', async () => {
    await compilePatch('(defn foo [x] (+ x 1))'); // warm cache

    const start = Date.now();
    await compilePatch('(defn foo [x] (+ x 1))'); // cache hit
    const elapsed = Date.now() - start;

    expect(elapsed).toBeLessThan(10);
  });
});
```

---

## Migration Path

### For Existing jank Users

**Phase 1-2: Opt-in Hot Reload**
- Existing native jank continues to work
- Hot reload is optional feature for web deployment
- Use `./bin/emscripten-bundle --hot-reload` flag

**Phase 3: WASM Compiler Available**
- Native jank still primary development tool
- WASM compiler for deployment
- Use `./bin/emscripten-bundle --wasm-compiler` flag

**Phase 4: Serverless Option**
- Native jank for desktop development
- WASM jank for web deployment
- Both share same language semantics

**No Breaking Changes:**
- Native jank remains fully supported
- WASM is deployment target, not replacement
- Same source code runs on both platforms

---

## Success Criteria

### Phase 1 (COMPLETE ✅)
- [x] Proof of concept working end-to-end
- [x] Latency <300ms for simple functions
- [x] Editor integration with CIDER
- [x] Documented and tested

### Phase 2 (Production Readiness)
- [ ] All common Clojure expressions supported
- [ ] Browser I/O working (console, files, network)
- [ ] Module system with pre-compiled core
- [ ] Error handling and debugging
- [ ] Latency <100ms (cached) or <200ms (uncached)
- [ ] Comprehensive test coverage
- [ ] Production deployments using the system

### Phase 3 (Compiler in WASM)
- [ ] jank compiler runs in browser
- [ ] No native server dependency for compilation
- [ ] Latency <300ms for WASM compilation
- [ ] Initial load <5s

### Phase 4 (Serverless)
- [ ] Fully self-contained (static hosting only)
- [ ] Latency <200ms for client-side compilation
- [ ] Works offline (PWA)

---

## Open Questions

1. **Memory Management:**
   - How large can WASM heap grow? (4GB limit)
   - How to handle GC in long-running browser sessions?
   - Should we expose GC controls to user code?

2. **Security:**
   - Sandboxing user-generated WASM code
   - Resource limits (CPU, memory)
   - Safe evaluation of untrusted code

3. **Deployment:**
   - Best practices for production WASM apps
   - Error reporting / telemetry
   - Versioning and updates

4. **Tooling:**
   - Browser DevTools integration
   - Profiling WASM code
   - Time-travel debugging

5. **Standards:**
   - nREPL extensions for WASM (new ops?)
   - WASM module format for Clojure namespaces
   - Compatibility with other Clojure dialects

---

## Next Steps (Immediate Actions)

### Week 1: Complete Expression Coverage Foundation
1. Implement loop/recur in wasm_patch_processor
2. Implement try/catch/finally
3. Add map and list literals
4. Write tests for new expression types

### Week 2: Browser I/O Basics
1. Hook println to console.log
2. Capture eval output for REPL display
3. Test with complex output (nested data structures)

### Week 3: Module System Foundation
1. Compile clojure.core to WASM module
2. Implement require → dlopen mapping
3. Test namespace loading

### Week 4: Performance Optimization
1. Implement compilation caching
2. Optimize emcc flags
3. Benchmark and profile

### Week 5-6: Testing & Documentation
1. Write comprehensive test suite
2. Document architecture
3. Create user guide
4. Demo video

---

## Conclusion

**The Path Forward:**

jank's WASM nREPL is a **pragmatic hybrid architecture** that achieves REPL-like interactive development in the browser despite WASM's fundamental limitations (no JIT, no dlopen without server).

**Key Insight:**
We don't need to compile code in the browser—we just need to *load* code in the browser quickly. Pre-compilation on a server (or WASM-compiled jank) + side module loading gives us the best of both worlds: native performance with REPL-like latency.

**Current Status:**
- Phase 1 is COMPLETE and working
- Phase 2 is the critical path to production use
- Phase 3 and 4 are ambitious long-term goals

**Recommendation:**
Focus on Phase 2 for the next 6 weeks. Get expression coverage, I/O, modules, and performance optimization done. Ship it. Learn from real usage. Then revisit Phase 3/4 based on actual user needs.

**This is achievable.** The hard parts are done (nREPL server, hot-reload runtime, codegen). The remaining work is "just" adding features and optimizing. Let's do this! 🚀

---

**Author:** Claude + jank exploration
**Date:** 2025-11-28
**Version:** 1.0
