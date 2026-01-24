# jank-rs Bootstrap and JIT Fix

## Summary
Fixed the jank-rs bootstrap to follow jank's pattern and fixed a JIT compilation bug with boolean returns.

## Key Changes

### 1. Namespaced Symbol Resolution for Native Functions
The evaluator now correctly resolves namespaced symbols like `native/string?` by falling back to `global_env.lookup_symbol()` when the namespace registry doesn't have the symbol.

**File:** `src/runtime/eval.rs` (lines 267-277)
```rust
if sym.namespace().is_some() {
    // Qualified symbol - resolve through namespace registry
    if let Some(value) = self.namespaces.resolve(sym) {
        return Ok(value);
    }
    // Also check global_env for native/ prefixed functions
    if let Some(value) = self.global_env.lookup_symbol(sym) {
        return Ok(value);
    }
    return Err(JankError::undefined_symbol(&format!("{}/{}",
        sym.namespace().unwrap(), sym.name())));
}
```

### 2. JIT Eligibility Check for Namespaced Symbols
Functions with namespaced symbols in their body (like `native/zero?`) were incorrectly being JIT compiled because `sym.name()` returns just the name part without namespace, matching the supported operations list.

**Bug:** `(fn* zero? [x] (native/zero? x))` was being JIT compiled because `native/zero?`.name() returns `"zero?"` which matched the supported list.

**Fix:** Added check for `sym.has_namespace()` in `is_jit_eligible()`:
```rust
if let Value::Symbol(sym) = &head {
    // Only bare symbols are supported (no namespace)
    // This prevents native/zero? from matching "zero?"
    if sym.has_namespace() {
        return false;
    }
    // ... rest of checks
}
```

### 3. Bootstrap Phase 0 in core.jrs
Added type predicates using `fn*` before `defmacro` is defined:
```clojure
;; BOOTSTRAP PHASE 0: Type Predicates (using fn* to wrap native functions)
(def ^{:doc "Returns true if x is nil, false otherwise."}
  nil?
  (fn* nil? [x] (native/nil? x)))

(def ^{:doc "Returns true if x is a String."}
  string?
  (fn* string? [x] (native/string? x)))
;; ... more predicates
```

### 4. Metadata Support in `def`
The parser converts `^{:doc "..."}` to `(with-meta sym {:doc "..."})`, so `eval_def` was updated to extract the symbol from this pattern.

## Root Cause of Take/Drop Bug
The `take` function uses `(zero? count)` in its loop. When `zero?` was incorrectly JIT compiled:
1. JIT stores booleans as integers (1 for true, 0 for false)
2. `call_compiled()` always returns `Value::Integer(result)`
3. So `(zero? 0)` returned `Value::Integer(1)` instead of `Value::Bool(true)`
4. In Clojure semantics, only `nil` and `false` are falsy - `0` is truthy!
5. So `(if (zero? count) ...)` with count=3 evaluated the truthy branch (0 is truthy)
6. This caused `take` to return empty result immediately

## Tests
All 155 tests pass after these fixes.
