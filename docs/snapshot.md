# jank WASM AOT Development Snapshot

**Date:** November 25, 2025  
**Branch:** nrepl  
**Focus:** WebAssembly AOT (Ahead-of-Time) Compilation

---

## Current State

### What Works ✅

1. **C++ Codegen (--codegen cpp)**: The jank compiler can generate C++ code instead of LLVM IR
2. **Loop/Recur Fixed**: Applied fixes from PR #598 to properly handle `loop*` and `recur`:
   - `recur` now checks `loop_target` to differentiate loop vs function recursion
   - `let` with `is_loop=true` generates `while(true){...}` wrapper
   - Loop bindings use boxed values and generate `__boxed` aliases
3. **WASM Runtime Basics**: 
   - `libjank.a`, `libjankzip.a`, and `libgc.a` compile for WASM
   - Runtime initializes, GC works
   - `clojure.core-native` loads
   - Lexer/Parser work (can read jank source)
4. **All Tests Pass**: 578 jank tests + 153 C++ tests pass

### What's Missing ❌

1. **No Full Evaluation in WASM**: `jank_eval_string_c` only parses, doesn't evaluate
   - WASM has no JIT - can't generate/execute code at runtime
   - Need to pre-compile all jank code to C++ on the host
2. **clojure.core not compiled**: Only native functions available in WASM
3. **No CLI flag for temp files**: Currently uses `/tmp` directly

---

## The Problem

WASM cannot JIT compile code. The current flow:
```
jank source → analyze → LLVM IR → JIT execute
```

For WASM we need:
```
jank source → analyze → C++ code → em++ compile → WASM binary
```

The C++ codegen (`--codegen cpp`) exists but:
1. It's designed for JIT evaluation, not standalone compilation
2. Generated code expects runtime structures that may not be available
3. No integration with emscripten build pipeline

---

## The Solution: AOT C++ Compilation

### Architecture

```
Host (macOS/Linux):
┌─────────────────────────────────────────────────────────────┐
│  jank source files                                          │
│       ↓                                                     │
│  jank compiler (native)                                     │
│       ↓                                                     │
│  C++ source files (AOT)                                     │
│       ↓                                                     │
│  em++ (emscripten)                                          │
│       ↓                                                     │
│  .wasm + .js                                                │
└─────────────────────────────────────────────────────────────┘

Browser/Node.js:
┌─────────────────────────────────────────────────────────────┐
│  Load .wasm                                                 │
│       ↓                                                     │
│  Initialize jank runtime                                    │
│       ↓                                                     │
│  Execute pre-compiled functions                             │
└─────────────────────────────────────────────────────────────┘
```

### Key Changes Needed

1. **CLI Enhancement**: Add `--output-dir` or `--temp-dir` flag
2. **C++ Codegen for WASM**: 
   - Generate standalone C++ that doesn't need JIT
   - Include proper headers
   - Generate module initialization code
3. **emscripten-bundle Integration**:
   - Compile jank → C++ using native jank
   - Compile C++ → WASM using em++
   - Link with libjank.a

---

## Implementation Plan

### Phase 1: Infrastructure
- [x] Fix loop/recur codegen (done!)
- [ ] Add /tmp to .gitignore
- [ ] Add --output-dir CLI flag

### Phase 2: WASM AOT Codegen
- [ ] Create `codegen/wasm_processor.cpp` or extend existing processor
- [ ] Generate standalone C++ modules
- [ ] Generate module registration code

### Phase 3: Integration
- [ ] Update emscripten-bundle to use AOT codegen
- [ ] Compile clojure.core to C++
- [ ] Link everything into WASM

### Phase 4: Testing
- [ ] Test simple expressions
- [ ] Test loop/recur
- [ ] Test clojure.core functions

---

## Code Locations

- **C++ Codegen**: `src/cpp/jank/codegen/processor.cpp`
- **CLI Options**: `include/cpp/jank/util/cli.hpp`, `src/cpp/jank/util/cli.cpp`
- **WASM C API**: `src/cpp/jank/c_api_wasm.cpp`
- **Build Script**: `bin/emscripten-bundle`
- **WASM Examples**: `wasm-examples/`

---

## Generated C++ Structure

Current codegen produces:
```cpp
namespace user {
  struct user_my_fn : jank::runtime::obj::jit_function {
    // lifted constants
    jank::runtime::obj::integer_ref const user_const_1;
    
    user_my_fn()
      : jank::runtime::obj::jit_function{ /* metadata */ }
      , user_const_1{ make_box<integer>(42) }
    {}
    
    object_ref call() final {
      // function body
    }
  };
}
```

For WASM AOT, we need:
```cpp
// Standalone header
#include <jank/runtime/prelude.hpp>

namespace mymodule {
  // Module-level vars
  static var_ref my_var;
  
  // Function structs (same as JIT)
  struct my_fn : jank::runtime::obj::jit_function { ... };
  
  // Module initialization
  void init() {
    my_var = __rt_ctx->intern_var("mymodule", "my-var").expect_ok();
    my_var->bind_root(make_box<my_fn>());
  }
}

// Registration for WASM runtime
extern "C" void jank_register_mymodule() {
  mymodule::init();
}
```

---

## Test Cases

### Simple Expression
```clojure
;; test.jank
42
```
Should generate C++ that evaluates to integer 42.

### Loop/Recur
```clojure
(loop [i 0 sum 0]
  (if (< i 5)
    (recur (+ i 1) (+ sum i))
    sum))
```
Should generate C++ with proper `while(true)` and `continue`.

### Function Definition
```clojure
(defn add [a b] (+ a b))
(add 1 2)
```
Should generate C++ with function struct and call.

---

## Notes

- The `jit_function` base class exists in libjank.a - reuse it for AOT
- Runtime context (`__rt_ctx`) is initialized by `jank_init()`
- Need to handle module dependencies (require/use)
- Consider source maps for debugging

---

## References

- PR #598: Loop/recur codegen fixes
- `docs/wasm-status.md`: Overall WASM progress
- `bin/emscripten-bundle`: Current build script
