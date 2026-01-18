# Plan: jank.compiler Namespace for jank-rs (COMPLETED)

**Status: IMPLEMENTED**
- All 166 tests pass
- Created `jank.compiler` namespace with introspection tools

## Overview

Create a `jank.compiler` namespace for jank-rs that provides introspection and debugging capabilities similar to the C++ jank version, adapted for Rust.

## What C++ jank.compiler Provides

- `native-cpp-source` / `native-cpp-source-formatted` - Generated C++ source
- `native-analyzed-form` - Analyzer's representation
- `native-optimized-form` - Optimized analyzed form
- `native-llvm-ir` / `native-llvm-ir-optimized` - LLVM IR
- `native-aot-source-*` - AOT compilation sources

## What jank-rs Can Provide

Since jank-rs uses Cranelift JIT (not Rust codegen), we adapt:

### 1. Form Analysis Functions
- `read-form` - Parse and return the form (identity, but validates syntax)
- `macroexpand-1` - Expand one level of macros
- `macroexpand` - Fully expand macros

### 2. Rust-Like Source Generation (NEW!)
- `rust-source` - Generate Rust-like pseudocode showing how the form evaluates
- `rust-source-formatted` - Same but with nice formatting/indentation

This is the KEY feature - generate readable Rust code that shows:
- How special forms (if, let, fn, loop, etc.) map to Rust
- Function calls with their evaluated arguments
- The evaluation strategy used

### 3. JIT Introspection
- `jit-eligible?` - Check if a form can be JIT compiled
- `jit-info` - Return info about JIT compilation status
- `cranelift-ir` - Return Cranelift IR for JIT-compiled functions (if available)

### 4. Evaluation Tracing
- `trace-eval` - Evaluate with step-by-step trace output

## Implementation Plan

### Phase 1: Core Infrastructure
1. Create `src/clojure/jank/compiler.jrs` namespace file
2. Add native Rust functions in `src/runtime/core.rs`:
   - `native/rust-source` - Core source generation
   - `native/jit-eligible?` - JIT eligibility check
   - `native/macroexpand-1` - Single macro expansion
   - `native/macroexpand` - Full macro expansion

### Phase 2: Rust Source Generator
Create a new module `src/runtime/rust_codegen.rs`:
```rust
/// Generate Rust-like pseudocode from a Value (form)
pub fn generate_rust_source(form: &Value) -> String {
    // Recursively convert form to Rust-like syntax
}

/// Generate with formatting/indentation
pub fn generate_rust_source_formatted(form: &Value, indent: usize) -> String {
    // Same but with proper indentation
}
```

Example transformations:
```clojure
(if (> x 0) "positive" "non-positive")
```
becomes:
```rust
if x > 0 {
    "positive"
} else {
    "non-positive"
}
```

```clojure
(let [x 1 y 2] (+ x y))
```
becomes:
```rust
{
    let x = 1;
    let y = 2;
    x + y
}
```

```clojure
(fn [x] (* x x))
```
becomes:
```rust
|x| {
    x * x
}
```

```clojure
(defn square [x] (* x x))
```
becomes:
```rust
fn square(x: Value) -> Value {
    x * x
}
```

### Phase 3: JIT Introspection
Add functions to expose JIT info:
- Check `is_jit_eligible` from evaluator
- Return compilation status from `compiled` cache
- Optionally dump Cranelift IR (advanced)

### Phase 4: Macro Expansion
- `macroexpand-1`: Evaluate macro once, return expanded form
- `macroexpand`: Repeatedly expand until no more macros

## Files to Create/Modify

1. **NEW** `src/runtime/rust_codegen.rs` - Rust source generation
2. **NEW** `src/clojure/jank/compiler.jrs` - The namespace definition
3. **MODIFY** `src/runtime/core.rs` - Add native functions
4. **MODIFY** `src/runtime/mod.rs` - Export new module
5. **MODIFY** `src/runtime/eval.rs` - Expose JIT info functions

## Example Usage (Goal)

```clojure
(require 'jank.compiler)

;; See Rust-like source for any form
(jank.compiler/rust-source '(if (> x 0) "positive" "negative"))
;; => "if x > 0 { \"positive\" } else { \"negative\" }"

(jank.compiler/rust-source-formatted '(defn factorial [n]
                                        (loop [i n acc 1]
                                          (if (<= i 1)
                                            acc
                                            (recur (dec i) (* acc i))))))
;; => "fn factorial(n: Value) -> Value {
;;        let mut i = n;
;;        let mut acc = 1;
;;        loop {
;;            if i <= 1 {
;;                return acc;
;;            }
;;            let (new_i, new_acc) = (dec(i), acc * i);
;;            i = new_i;
;;            acc = new_acc;
;;        }
;;    }"

;; Check JIT eligibility
(jank.compiler/jit-eligible? '(fn [x] (* x x)))
;; => true

(jank.compiler/jit-eligible? '(fn [x] (println x)))
;; => false

;; Expand macros
(jank.compiler/macroexpand-1 '(defn foo [x] x))
;; => (def foo (fn foo [x] x))
```

## Priority Order

1. `rust-source` / `rust-source-formatted` - Most valuable for debugging
2. `jit-eligible?` - Simple and useful
3. `macroexpand-1` / `macroexpand` - Important for macro debugging
4. `jit-info` - Nice to have
5. `cranelift-ir` - Advanced, lower priority
