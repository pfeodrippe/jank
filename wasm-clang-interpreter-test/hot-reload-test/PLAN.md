# jank WASM Hot-Reload: Implementation Plan

**Date:** November 28, 2025
**Status:** Phase 5 COMPLETE - jank compiler integration implemented!

---

## Quick Start

### Running Hot-Reload with jank Compiler

**Terminal 1: Start jank nREPL server (compiler backend)**
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./build/jank repl --server
# Starts nREPL on port 5555
```

**Terminal 2: Start hot-reload server**
```bash
cd /Users/pfeodrippe/dev/jank/wasm-clang-interpreter-test/hot-reload-test
node hot_reload_server.cjs
# Starts HTTP on 8080, WebSocket on 7888, Editor nREPL on 7889
```

**Browser:**
Open `http://localhost:8080/eita_hot_reload.html`

**Editor (Emacs):**
```
M-x cider-connect RET localhost RET 7889 RET
```

**Or test directly:**
```bash
curl -X POST -d "(defn ggg [v] (+ 49 v))" http://localhost:8080/eval
```

---

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────────┐
│                        CURRENT (Bash Script) - LIMITED                      │
│                                                                             │
│  User Code ──► Bash Script (regex) ──► C++ Template ──► emcc ──► WASM      │
│                    ❌ Can't parse:                                          │
│                       - docstrings                                          │
│                       - ^:export metadata                                   │
│                       - #() anonymous fns                                   │
│                       - complex nested expressions                          │
└────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────┐
│                        NEW (jank Compiler) - FULL SUPPORT                   │
│                                                                             │
│  User Code ──► jank Lexer ──► Parser ──► Analyzer ──► Codegen ──► C++      │
│                                                   (wasm_patch)              │
│                    ✅ Handles ALL jank syntax:                              │
│                       - docstrings, metadata                                │
│                       - #() anonymous functions                             │
│                       - let, if, loop, recur                                │
│                       - destructuring                                       │
│                       - macros (expanded before codegen)                    │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Current State

### ✅ What's Working

1. **HOT_RELOAD=1 Build Mode** (Step 1)
   - `bin/emscripten-bundle` supports `-sMAIN_MODULE=2`
   - Adds `-fPIC` for position-independent code
   - Enables `dlopen()` for dynamic loading
   - Location: `compiler+runtime/bin/emscripten-bundle` (lines 709-712, 1165-1189)

2. **Hot-Reload Registry** (Step 2)
   - C++ registry class loads SIDE_MODULE patches via `dlopen()`
   - `register_symbol()` creates `native_function_wrapper` and binds to vars
   - C API: `jank_hot_reload_load_patch()`, `jank_hot_reload_get_stats()`
   - Supports arities 0-4
   - Location: `compiler+runtime/src/cpp/jank/runtime/hot_reload.cpp`

3. **Runtime Helpers Exported** (Step 3)
   - `jank_box_integer()`, `jank_unbox_integer()` for integer boxing
   - `jank_call_var()` for calling any var by name
   - `jank_make_keyword()`, `jank_make_vector()`, `jank_make_set()`
   - `jank_println()` for output

4. **Emscripten dlsym Caching Fix**
   - Each patch has unique symbol names (e.g., `jank_patch_symbols_42`)
   - Server increments patch ID for each new patch
   - See README.md "Critical Emscripten Lesson" section

### ❌ What's NOT Working

The **bash script** (`generate-wasm-patch-auto`) cannot parse complex jank:
```clojure
;; This FAILS with the bash script:
(defn ggg
  "Docstring here"
  ^:export
  [v]
  (println :FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!
           (+ 50 v)
           (set/union #{999} (set (mapv #(* 3 %) [1 5 v]))))
  (+ 48 v))
```

The bash script fails because:
- Can't parse docstrings after function name
- Can't handle `^:export` metadata
- Can't parse `#()` anonymous function syntax
- Can't handle multiple body expressions

**SOLUTION:** Use jank's actual compiler!

---

## Phase 5: jank Compiler Integration (NEW!)

### Goal

Replace the bash script with jank's real compiler pipeline:
```
jank source → Lexer → Parser → Analyzer → Codegen(wasm_patch) → C++ → emcc → WASM
```

### Why This Works

jank already has:
1. **Full parser** that handles ALL Clojure syntax (`src/cpp/jank/read/parse.cpp`)
2. **Analyzer** that resolves symbols, macroexpands, validates (`src/cpp/jank/analyze/processor.cpp`)
3. **Codegen** that generates C++ from analyzed AST (`src/cpp/jank/codegen/processor.cpp`)
4. **`wasm_aot` compilation target** already exists!

The `wasm_aot` target already generates standalone C++ for WASM. We just need a
`wasm_patch` target that generates SIDE_MODULE compatible code.

---

### Step 5.1: Add `compilation_target::wasm_patch`

**File:** `compiler+runtime/include/cpp/jank/codegen/llvm_processor.hpp`

```cpp
enum class compilation_target : u8
{
  module,
  function,
  eval,
  wasm_aot,    // Existing: standalone C++ modules for WASM
  wasm_patch   // NEW: SIDE_MODULE patches for hot-reload
};
```

**File:** `compiler+runtime/src/cpp/jank/codegen/processor.cpp`

For `wasm_patch` target, the generated code must:
1. Use `extern "C"` for all exported symbols
2. Import runtime helpers from main module (not include jank headers!)
3. Export a `jank_patch_symbols_N()` function with metadata
4. Use unique symbol names to avoid dlsym caching

**Generated patch structure:**
```cpp
// Auto-generated WASM hot-reload patch
// Patch ID: 42

extern "C" {

// Import runtime helpers from main module
extern void *jank_box_integer(int64_t value);
extern int64_t jank_unbox_integer(void *obj);
extern void *jank_call_var(const char *ns, const char *name, int argc, void **args);
extern void *jank_make_keyword(const char *ns, const char *name);
extern void *jank_make_vector(int argc, void **elements);
extern void *jank_make_set(int argc, void **elements);
extern void *jank_println(int argc, void **args);
// ... other helpers

// Patch symbol metadata
struct patch_symbol {
  const char *qualified_name;
  const char *signature;
  void *fn_ptr;
};

// The patched function - UNIQUE NAME
__attribute__((visibility("default")))
void *jank_eita_ggg_42(void *v) {
  // Generated from jank code:
  // (println :FROM_CLJ... (+ 50 v) ...)
  // (+ 48 v)

  void *kw = jank_make_keyword("", "FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!");
  void *add_args[] = { jank_box_integer(50), v };
  void *sum = jank_call_var("clojure.core", "+", 2, add_args);
  // ... rest of the body

  void *result_args[] = { jank_box_integer(48), v };
  return jank_call_var("clojure.core", "+", 2, result_args);
}

// Patch metadata export - UNIQUE NAME
__attribute__((visibility("default")))
patch_symbol *jank_patch_symbols_42(int *count) {
  static patch_symbol symbols[] = {
    { "eita/ggg", "1", (void *)jank_eita_ggg_42 }
  };
  *count = 1;
  return symbols;
}

}
```

---

### Step 5.2: Modify Codegen for `wasm_patch`

**Key changes in `processor.cpp`:**

1. **No jank headers** - Patches can't include jank headers (would duplicate code)
2. **Extern C helpers** - All runtime calls go through C helper functions
3. **Flat structure** - No namespaces, just extern "C" functions
4. **Patch metadata** - Export `jank_patch_symbols_N()` function

**Code generation mapping:**

| jank Expression | Generated C++ (wasm_patch) |
|-----------------|---------------------------|
| `42` | `jank_box_integer(42)` |
| `3.14` | `jank_box_double(3.14)` |
| `"hello"` | `jank_make_string("hello")` |
| `:keyword` | `jank_make_keyword("", "keyword")` |
| `:ns/kw` | `jank_make_keyword("ns", "kw")` |
| `(+ a b)` | `jank_call_var("clojure.core", "+", 2, (void*[]){a, b})` |
| `[1 2 3]` | `jank_make_vector(3, (void*[]){...})` |
| `#{1 2}` | `jank_make_set(2, (void*[]){...})` |
| `(println x)` | `jank_println(1, (void*[]){x})` |
| Local `x` | `x` (parameter or let binding) |

**For `let` bindings:**
```clojure
(let [x 42] (+ x 1))
```
Generates:
```cpp
void *x = jank_box_integer(42);
void *args[] = { x, jank_box_integer(1) };
return jank_call_var("clojure.core", "+", 2, args);
```

**For `if`:**
```clojure
(if (pos? x) x 0)
```
Generates:
```cpp
void *cond_args[] = { x };
void *cond = jank_call_var("clojure.core", "pos?", 1, cond_args);
if (jank_truthy(cond)) {
  return x;
} else {
  return jank_box_integer(0);
}
```

---

### Step 5.3: Add CLI Option for Patch Generation

**File:** `compiler+runtime/include/cpp/jank/util/cli.hpp`

```cpp
enum class codegen_type : u8
{
  llvm_ir,
  cpp,
  wasm_aot,
  wasm_patch  // NEW
};
```

**Usage:**
```bash
./build/jank run --codegen wasm-patch --patch-id 42 --save-cpp-path patch.cpp input.jank
```

**File:** `compiler+runtime/src/cpp/main.cpp`

Add handling for `wasm_patch` codegen type to:
1. Parse the jank code
2. Generate patch C++
3. Save to specified path

---

### Step 5.4: Add nREPL Op for Patch Generation

**File:** `compiler+runtime/include/cpp/jank/nrepl_server/ops/wasm_patch.hpp` (NEW)

New nREPL operation: `wasm-compile-patch`

**Request:**
```clojure
{:op "wasm-compile-patch"
 :code "(defn ggg [v] (+ 48 v))"
 :patch-id 42
 :session "..."}
```

**Response:**
```clojure
{:cpp "extern \"C\" { ... }"
 :symbols [{:name "eita/ggg" :arity 1}]
 :session "..."
 :status ["done"]}
```

**Implementation:**

```cpp
inline std::vector<bencode::value::dict> engine::handle_wasm_compile_patch(message const &msg)
{
  auto const code(msg.get("code"));
  auto const patch_id(msg.get_integer("patch-id").value_or(0));

  // 1. Parse the jank code
  read::lex::processor l_prc{ code };
  read::parse::processor p_prc{ l_prc };
  auto parsed = p_prc.next();

  // 2. Analyze
  auto &session(ensure_session(msg.session()));
  analyze::processor a_prc{ __rt_ctx, parsed.unwrap() };
  auto analyzed = a_prc.analyze();

  // 3. Generate patch C++
  codegen::processor cg_prc{ analyzed, "patch", codegen::compilation_target::wasm_patch };
  cg_prc.set_patch_id(patch_id);
  auto const cpp_code = cg_prc.declaration_str();

  // 4. Return the generated C++
  bencode::value::dict response;
  response.emplace("cpp", cpp_code);
  response.emplace("session", session.id);
  response.emplace("status", bencode::list_of_strings({ "done" }));
  return { response };
}
```

---

### Step 5.5: Update Server to Use jank Compiler

**File:** `hot-reload-test/hot_reload_server.cjs`

Replace bash script call with jank compiler call:

```javascript
async function compileDefnToWasm(code, patchId) {
  // Option A: CLI (simpler)
  const { execSync } = require('child_process');
  const cppPath = `/tmp/patch_${patchId}.cpp`;
  const wasmPath = `/tmp/patch_${patchId}.wasm`;

  // Write code to temp file
  fs.writeFileSync('/tmp/patch.jank', code);

  // Run jank compiler to generate C++
  execSync(`./build/jank run --codegen wasm-patch --patch-id ${patchId} --save-cpp-path ${cppPath} /tmp/patch.jank`);

  // Compile C++ to WASM
  execSync(`emcc ${cppPath} -o ${wasmPath} -sSIDE_MODULE=1 -O2 -fPIC`);

  return {
    wasm: fs.readFileSync(wasmPath),
    symbolName: `jank_patch_symbols_${patchId}`
  };
}

// Option B: nREPL (more elegant)
async function compileDefnToWasmNrepl(code, patchId) {
  const response = await nreplEval({
    op: 'wasm-compile-patch',
    code: code,
    'patch-id': patchId
  });

  const cpp = response.cpp;
  // ... compile cpp to wasm
}
```

---

### Step 5.6: Test with Complex Expression

**Test case:**
```clojure
(defn ggg
  "Docstring"
  ^:export
  [v]
  (println :FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!
           (+ 50 v)
           (set/union #{999} (set (mapv #(* 3 %) [1 5 v]))))
  (+ 48 v))
```

**Expected generated C++:**
```cpp
extern "C" {

extern void *jank_box_integer(int64_t value);
extern void *jank_call_var(const char *ns, const char *name, int argc, void **args);
extern void *jank_make_keyword(const char *ns, const char *name);
extern void *jank_make_vector(int argc, void **elements);
extern void *jank_make_set(int argc, void **elements);
extern void *jank_println(int argc, void **args);

struct patch_symbol { const char *qualified_name; const char *signature; void *fn_ptr; };

// Anonymous function from #(* 3 %)
void *jank_anon_42_0(void *p1) {
  void *args[] = { jank_box_integer(3), p1 };
  return jank_call_var("clojure.core", "*", 2, args);
}

__attribute__((visibility("default")))
void *jank_eita_ggg_42(void *v) {
  // (println :FROM_CLJ... (+ 50 v) (set/union #{999} (set (mapv #(* 3 %) [1 5 v]))))
  void *kw = jank_make_keyword("", "FROM_CLJ_..._I_MEAN_JANK_IN_WASM!!");

  void *add_args[] = { jank_box_integer(50), v };
  void *sum = jank_call_var("clojure.core", "+", 2, add_args);

  void *vec_elems[] = { jank_box_integer(1), jank_box_integer(5), v };
  void *vec = jank_make_vector(3, vec_elems);

  // mapv with anonymous function - needs function object
  void *mapped = jank_call_var_with_fn("clojure.core", "mapv", jank_anon_42_0, vec);

  void *set_result = jank_call_var("clojure.core", "set", 1, (void*[]){ mapped });

  void *set_999_elems[] = { jank_box_integer(999) };
  void *set_999 = jank_make_set(1, set_999_elems);

  void *union_args[] = { set_999, set_result };
  void *final_set = jank_call_var("clojure.set", "union", 2, union_args);

  void *println_args[] = { kw, sum, final_set };
  jank_println(3, println_args);

  // Return value: (+ 48 v)
  void *result_args[] = { jank_box_integer(48), v };
  return jank_call_var("clojure.core", "+", 2, result_args);
}

__attribute__((visibility("default")))
patch_symbol *jank_patch_symbols_42(int *count) {
  static patch_symbol symbols[] = {
    { "eita/ggg", "1", (void *)jank_eita_ggg_42 }
  };
  *count = 1;
  return symbols;
}

}
```

---

## Implementation Order

### Phase 5a: Proof of Concept (Manual) ✅ COMPLETE
1. [x] Manually write the expected C++ output for the complex test case
   - Created `complex_patch_poc.cpp` (985 bytes compiled)
   - Demonstrates all required patterns:
     - Anonymous functions wrapped with `jank_make_fn_wrapper`
     - Keywords, vectors, sets
     - Calling clojure.core and clojure.set functions
     - Multiple body expressions
2. [x] Added missing runtime helpers to `hot_reload.cpp`:
   - `jank_nil_value()` - return nil
   - `jank_truthy(void*)` - check if truthy (for `if` expressions)
   - `jank_make_symbol(ns, name)` - create symbols
   - `jank_make_fn_wrapper(fn_ptr, arity)` - wrap C function as callable
3. [x] Verify it compiles with emcc to SIDE_MODULE

### Phase 5b: Codegen Changes ✅ COMPLETE
1. [x] Created `wasm_patch_processor` class
   - File: `compiler+runtime/include/cpp/jank/codegen/wasm_patch_processor.hpp`
   - File: `compiler+runtime/src/cpp/jank/codegen/wasm_patch_processor.cpp`
   - Generates extern "C" functions (no C++ namespaces)
   - Uses runtime helper calls (jank_box_integer, jank_call_var, etc.)
   - Generates unique symbol names to avoid Emscripten dlsym caching
2. [x] Implemented expression visitors:
   - `gen_primitive_literal()` - integers, reals, booleans, nil, keywords, strings
   - `gen_call()` - function calls via jank_call_var
   - `gen_local_reference()` - parameter and let binding references
   - `gen_var_deref()` - var dereferencing
   - `gen_vector()` - vector literals via jank_make_vector
   - `gen_set()` - set literals via jank_make_set
   - `gen_do()` - do blocks (multiple expressions)
   - `gen_let()` - let bindings
   - `gen_if()` - if expressions via jank_truthy
   - `gen_function()` - anonymous functions via jank_make_fn_wrapper

### Phase 5c: CLI Integration ✅ COMPLETE (via codegen enum)
1. [x] Updated `codegen_type` enum with `wasm_patch` target
   - File: `compiler+runtime/include/cpp/jank/util/cli.hpp`

### Phase 5d: nREPL Integration ✅ COMPLETE
1. [x] Created `wasm-compile-patch` nREPL op handler
   - File: `compiler+runtime/include/cpp/jank/nrepl_server/ops/wasm_compile_patch.hpp`
   - Parses jank code via `__rt_ctx->analyze_string()`
   - Finds def expression in analyzed AST
   - Generates C++ via wasm_patch_processor
   - Returns cpp-code, var-name, ns-name, patch-id
2. [x] Registered op in engine dispatch
   - File: `compiler+runtime/include/cpp/jank/nrepl_server/engine.hpp`
   - Added dispatch case for "wasm-compile-patch"
   - Added declaration for `handle_wasm_compile_patch`
3. [x] Updated hot_reload_server.cjs
   - Connects to jank nREPL on port 5555
   - Sends wasm-compile-patch op with jank code
   - Receives C++ code from jank compiler
   - Compiles to WASM using emcc
   - Broadcasts to browsers
   - Falls back to bash script if jank connection fails

### Phase 5e: Edge Cases (Supported)
1. [x] Handle anonymous functions `#()` - via gen_function
2. [x] Handle `let` bindings - via gen_let
3. [x] Handle `if`/`when`/`cond` - via gen_if (when/cond macroexpand to if)
4. [ ] Handle `loop`/`recur` - NOT YET IMPLEMENTED
5. [ ] Handle multiple arities - NOT YET IMPLEMENTED

---

## Files Modified/Created

### New Files Created

| File | Purpose |
|------|---------|
| `include/cpp/jank/codegen/wasm_patch_processor.hpp` | WASM patch code generator header |
| `src/cpp/jank/codegen/wasm_patch_processor.cpp` | WASM patch code generator implementation (480+ lines) |
| `include/cpp/jank/nrepl_server/ops/wasm_compile_patch.hpp` | nREPL op handler for wasm-compile-patch |

### Files Modified

| File | Changes |
|------|---------|
| `include/cpp/jank/util/cli.hpp` | Added `wasm_patch` to codegen_type enum |
| `include/cpp/jank/nrepl_server/engine.hpp` | Added dispatch for "wasm-compile-patch" op, include, declaration |
| `src/cpp/jank/runtime/hot_reload.cpp` | Added missing runtime helpers (jank_truthy, jank_nil_value, jank_make_fn_wrapper, jank_make_symbol) |
| `hot-reload-test/hot_reload_server.cjs` | Added jank nREPL client, compileCppToWasm, generatePatchWithJank |

---

## Runtime Helpers Available

All required helpers are implemented in `hot_reload.cpp`:

| Helper | Status | Purpose |
|--------|--------|---------|
| `jank_box_integer` | ✅ | Box int64_t to object |
| `jank_unbox_integer` | ✅ | Unbox object to int64_t |
| `jank_box_double` | ✅ | Box double to object |
| `jank_unbox_double` | ✅ | Unbox object to double |
| `jank_make_string` | ✅ | Create string object |
| `jank_make_keyword` | ✅ | Create keyword |
| `jank_make_symbol` | ✅ | Create symbol |
| `jank_make_vector` | ✅ | Create vector |
| `jank_make_set` | ✅ | Create set |
| `jank_call_var` | ✅ | Call var by ns/name with args |
| `jank_deref_var` | ✅ | Deref var by ns/name |
| `jank_println` | ✅ | Print args |
| `jank_truthy` | ✅ | Check if value is truthy (for if) |
| `jank_make_fn_wrapper` | ✅ | Wrap C fn pointer as callable object |
| `jank_nil_value` | ✅ | Return nil value |

### Not Yet Implemented (for future features)

| Helper | Purpose |
|--------|---------|
| `jank_cons` | Cons onto sequence |
| `jank_first` | Get first of sequence |
| `jank_rest` | Get rest of sequence |
| `jank_make_map` | Create hash map |
| `jank_recur` | Recur target for loop |

---

## Success Criteria

### Minimum Viable (Phase 5a-5c) ✅ COMPLETE
- [x] Simple defn compiles correctly: `(defn foo [x] (+ x 1))`
- [x] Keywords work: `(defn foo [] :hello)`
- [x] Vectors work: `(defn foo [] [1 2 3])`
- [x] Sets work: `(defn foo [] #{1 2})`

### Full Support (Phase 5d-5e) ✅ MOSTLY COMPLETE
- [x] Complex test case compiles (wasm_patch_processor generates C++)
- [x] Anonymous functions `#()` work (via gen_function)
- [x] `let` bindings work (via gen_let)
- [x] `if`/`when` work (via gen_if + macroexpansion)
- [ ] Multiple arities - NOT YET
- [ ] `loop`/`recur` - NOT YET
- [x] Server integration complete (hot_reload_server.cjs connects to jank nREPL)

### Remaining Work
- [ ] End-to-end browser testing with jank compiler
- [ ] Add loop/recur support
- [ ] Add multiple arity support
- [ ] Add map literal support

---

*Last Updated: November 28, 2025*

---

## Previous Implementation (Reference)

The sections below document the original bash-script based approach, kept for reference.

### ✅ Phase 1: Understand jank Codegen (COMPLETE)

**Goal:** Understand how jank generates C++ code for functions

**Completed:**
- Examined jank codegen in `processor.cpp` and `llvm_processor.cpp`
- Identified compilation targets: module, function, eval, wasm_aot
- Understood function generation pattern with jit_function structs
- Discovered that patches need exported runtime helpers for boxing/unboxing

**Key Finding:** Patches call exported C helper functions (`jank_box_integer`, `jank_unbox_integer`) from the main module rather than duplicating jank runtime code.

---

### ✅ Phase 2: Create Patch Generator (COMPLETE)

**Goal:** Generate correct C++ patch code from jank source

**Implementation:** Created `bin/generate-wasm-patch` script

**Usage:**
```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./bin/generate-wasm-patch <namespace/fn-name> <arity> <expression>

# Example:
./bin/generate-wasm-patch eita/ggg 1 "(+ 49 v)"
```

**Supported Expressions:**
- `(+ <number> <param>)` - Add constant to parameter
- `(- <number> <param>)` - Subtract param from constant
- `(* <number> <param>)` - Multiply constant and parameter

**Output:**
- `<fn-name>_patch.cpp` - Generated C++ source
- `<fn-name>_patch.wasm` - Compiled SIDE_MODULE (~350 bytes)

**Generated Code Template:**
```cpp
extern "C" {
  extern void *jank_box_integer(int64_t value);
  extern int64_t jank_unbox_integer(void *obj);

  __attribute__((visibility("default")))
  void *jank_<ns>_<fn>(void *p0) {
    int64_t value = jank_unbox_integer(p0);
    return jank_box_integer(value + N);
  }

  __attribute__((visibility("default")))
  patch_symbol* jank_patch_symbols(int *count) {
    static patch_symbol symbols[] = {{"ns/fn", "1", (void*)jank_<ns>_<fn>}};
    *count = 1;
    return symbols;
  }
}
```

**Deliverable:** ✅ Working `generate-wasm-patch` script that compiles to WASM

---

### ✅ Phase 3: Export Runtime Helper Functions (COMPLETE)

**Goal:** Make runtime functions available to patches

**Implementation:** Added C helper functions to `hot_reload.cpp`

**Exported Functions:**
```cpp
extern "C" {
  // Box an integer value into a jank object_ref
  EMSCRIPTEN_KEEPALIVE void *jank_box_integer(int64_t value);

  // Unbox an integer from a jank object_ref (returns 0 if not an integer)
  EMSCRIPTEN_KEEPALIVE int64_t jank_unbox_integer(void *obj);

  // Add two boxed integers and return a new boxed integer
  EMSCRIPTEN_KEEPALIVE void *jank_add_integers(void *a, void *b);
}
```

**Files Modified:**
- `compiler+runtime/include/cpp/jank/runtime/hot_reload.hpp` - declarations
- `compiler+runtime/src/cpp/jank/runtime/hot_reload.cpp` - implementations

**Deliverable:** ✅ Patches can call runtime functions via imported symbols

---

### Phase 4: WebSocket Integration (Optional)

**Goal:** Enable eval from browser devtools

**Tasks:**
1. [ ] Add WebSocket library (websocketpp or uWebSockets)
2. [ ] Implement `start-hot-reload-server` in C++
3. [ ] Handle eval requests → compile → send patch
4. [ ] Embed `jank_hot_reload_client.js` in HTML output

**Deliverable:** `jankEval('(defn foo [x] (+ x 1))')` from browser

---

## Recommended Approach

### Quick Win: Phase 3 First

The fastest path to a working demo:

1. **Export runtime helpers** from main module
2. **Manually write** a correct C++ patch using those helpers
3. **Verify** end-to-end hot-reload works

Then:

4. **Automate** with compiler integration (Phase 2)
5. **Add WebSocket** for browser eval (Phase 4)

### Implementation Order

```
Week 1:
  ├─ Phase 1: Research codegen (1 day)
  └─ Phase 3: Export helpers (1-2 days)
  └─ Test: Manual patch that works

Week 2:
  └─ Phase 2: Compiler integration (3-4 days)
  └─ Test: Auto-generated patches

Week 3 (optional):
  └─ Phase 4: WebSocket server (2-3 days)
  └─ Test: Browser devtools eval
```

---

## Technical Details

### Patch C++ Template

Based on jank codegen patterns, a patch should look like:

```cpp
#include <jank/runtime/object.hpp>
#include <jank/runtime/obj/number.hpp>
// ... minimal includes

namespace jank::runtime {

// The patched function implementation
object_ref patched_ggg(object_ref v) {
  auto val = try_object<obj::integer>(v);
  return make_box<obj::integer>(val->data + 49);
}

} // namespace jank::runtime

extern "C" {

// Patch metadata
struct patch_symbol {
  const char *qualified_name;
  const char *signature;
  void *fn_ptr;
};

__attribute__((visibility("default")))
patch_symbol* jank_patch_symbols(int *count) {
  static patch_symbol symbols[] = {
    {"eita/ggg", "1", (void*)jank::runtime::patched_ggg}
  };
  *count = 1;
  return symbols;
}

}
```

### Compilation Command

```bash
emcc patch.cpp -o patch.wasm \
  -sSIDE_MODULE=1 \
  -O2 \
  -fPIC \
  -I/path/to/jank/include/cpp \
  -std=c++20
```

### Files to Modify

| File | Change |
|------|--------|
| `compiler+runtime/src/cpp/jank/codegen/processor.cpp` | Add patch mode |
| `compiler+runtime/include/cpp/jank/runtime/hot_reload.hpp` | Add helper exports |
| `compiler+runtime/bin/emscripten-bundle` | Export helper functions |
| `src/jank/jank/nrepl_server/hot_reload.jank` | Implement `compile-to-wasm-patch` |

---

## Success Criteria

### ✅ Minimum Viable Hot-Reload (COMPLETE!)

1. [x] User edits `(defn ggg [v] (+ v 48))` to `(+ v 49)`
2. [x] Compile to patch.wasm (~347 bytes)
3. [x] Load via `jank_hot_reload_load_patch()`
4. [x] Calling `ggg(10)` returns `59` instead of `58`
5. [x] Total time < 500ms (achieved ~200ms with emcc)

### Full Hot-Reload (Future)

1. [ ] WebSocket server on port 7888
2. [ ] Browser connects automatically
3. [ ] `jankEval('(defn foo [x] (* x 2))')` from devtools
4. [ ] Function updates in < 200ms
5. [ ] State preserved across patches

---

## Next Steps

**Phase 5 COMPLETE:**
1. ✅ Runtime helper functions exported (all needed helpers in `hot_reload.cpp`)
2. ✅ Manual patch generator script (`bin/generate-wasm-patch`)
3. ✅ End-to-end hot-reload tested and working (bash script approach)
4. ✅ **Automatic patch generator** (`bin/generate-wasm-patch-auto`)
5. ✅ **jank compiler integration** - `wasm_patch_processor` class generates C++ from AST
6. ✅ **nREPL integration** - `wasm-compile-patch` op in jank native server
7. ✅ **Server integration** - `hot_reload_server.cjs` connects to jank nREPL

**Data Flow (Complete):**
```
Editor ──► hot_reload_server.cjs (7889) ──► jank nREPL (5555)
                │                                  │
                │ (transparent proxy)              ▼
                │                           eval/load-file/etc
                │                                  │
                ◄──────────────────────────────────┘
                │                           (response relayed)
                │
                └──► [if defn detected]
                           │
                           ▼
                    wasm-compile-patch op → C++
                           │
                           ▼
                    emcc → WASM
                           │
                           ▼
                    WebSocket → Browser → dlopen
```

**Key Insight:** The server is a transparent nREPL proxy. All ops (eval, load-file,
describe, clone, etc.) are forwarded to jank's native C++ nREPL. The server only
adds hot-reload by intercepting defn evaluations to also generate WASM patches.

**Remaining Work:**
1. [ ] End-to-end browser testing with jank compiler
2. [ ] Add `loop`/`recur` support to wasm_patch_processor
3. [ ] Add multiple arity support
4. [ ] Add map literal support (`{}`)
5. [ ] Performance optimization (caching, incremental compilation)

**Quick Test Commands:**
```bash
# Start jank nREPL server (compiler backend)
cd /Users/pfeodrippe/dev/jank/compiler+runtime
./build/jank repl --server

# Start hot-reload server (in another terminal)
cd /Users/pfeodrippe/dev/jank/wasm-clang-interpreter-test/hot-reload-test
node hot_reload_server.cjs

# Open browser
open http://localhost:8080/eita_hot_reload.html

# Test with curl
curl -X POST -d "(defn ggg [v] (+ 49 v))" http://localhost:8080/eval

# Or connect from Emacs
# M-x cider-connect RET localhost RET 7889 RET
```

**Fallback Mode:**
If jank nREPL is not available, the server falls back to the bash script generator.

---

*Plan Created: November 27, 2025*
*Last Updated: November 28, 2025*
*Status: Phase 5 COMPLETE - jank compiler integration implemented!*
