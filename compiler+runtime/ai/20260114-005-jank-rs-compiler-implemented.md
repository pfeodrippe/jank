# jank.compiler Namespace Implementation (COMPLETE)

## Summary
Implemented `jank.compiler` namespace for jank-rs, providing introspection and debugging tools similar to C++ jank's `jank.compiler`.

## Files Created/Modified

### New Files
1. **`src/runtime/rust_codegen.rs`** - Rust-like source code generator
   - `generate_rust_source(form)` - Convert Clojure form to Rust-like pseudocode
   - `generate_rust_source_formatted(form)` - Same with nice indentation
   - Handles all special forms: if, let, fn, def, defn, loop, recur, etc.
   - Converts operators to infix notation: `(+ 1 2)` → `(1 + 2)`

2. **`src/clojure/jank/compiler.jrs`** - The jank.compiler namespace
   - `rust-source` - Generate Rust-like code from a form
   - `rust-source-formatted` - Same with formatting
   - `jit-eligible?` - Check if form can be JIT compiled
   - `print-rust` - Convenience function to print Rust source
   - `analyze-form` - Return map with rust-source, jit-eligible, type

### Modified Files
1. **`src/runtime/mod.rs`** - Added `rust_codegen` module
2. **`src/runtime/core.rs`** - Added native functions:
   - `native/rust-source`
   - `native/rust-source-formatted`
   - `native/jit-eligible?`
   - Added `is_jit_eligible()` function (moved from eval.rs for reuse)
3. **`src/runtime/eval.rs`** - Added test for jank.compiler namespace

## Example Usage

```clojure
(require jank.compiler)

;; Generate Rust-like source for a simple expression
(jank.compiler/rust-source '(+ 1 2))
;; => "(1 + 2)"

;; Generate Rust-like source for an if expression
(jank.compiler/rust-source '(if (> x 0) "positive" "negative"))
;; => "if (x > 0) { \"positive\" } else { \"negative\" }"

;; Formatted output for complex functions
(jank.compiler/rust-source-formatted '(defn factorial [n]
                                        (loop [i n acc 1]
                                          (if (<= i 1)
                                            acc
                                            (recur (dec i) (* acc i))))))
;; Returns nicely indented Rust-like code

;; Check if a form is JIT eligible
(jank.compiler/jit-eligible? '(fn [x] (* x x)))
;; => true

(jank.compiler/jit-eligible? '(fn [x] (println x)))
;; => false

;; Get full analysis
(jank.compiler/analyze-form '(fn [x] (* x x)))
;; => {:rust-source "|x| { (x * x) }"
;;     :jit-eligible true
;;     :type "list"}
```

## Transformations

The Rust codegen transforms Clojure to Rust-like syntax:

| Clojure | Rust-like |
|---------|-----------|
| `(+ 1 2 3)` | `(1 + 2 + 3)` |
| `(if x 1 2)` | `if x { 1 } else { 2 }` |
| `(let [x 1] x)` | `{ let x = 1; x }` |
| `(fn [x] x)` | `\|x\| { x }` |
| `(def x 42)` | `static x: Value = 42;` |
| `(defn foo [x] x)` | `fn foo(x: Value) -> Value { x }` |
| `[1 2 3]` | `vec![1, 2, 3]` |
| `:keyword` | `:keyword` |
| `(loop [i 0] ...)` | `{ let mut i = 0; loop { ... } }` |

## Tests
- Added `test_jank_compiler_namespace` test
- All 166 tests pass

## Design Notes
- Unlike C++ jank which generates actual C++ code, jank-rs generates pseudocode
- This is because jank-rs uses Cranelift JIT, not Rust codegen
- The Rust-like output is for debugging/understanding, not compilation
- JIT eligibility check reuses the evaluator's logic
