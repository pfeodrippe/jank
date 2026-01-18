# jank-rs: core.jrs Implementation Complete

**Date**: 2026-01-14
**Status**: COMPLETE - 155 tests passing!

## Summary

Implemented `core.jrs` for jank-rs, providing Clojure-like core library functions using loop/recur instead of evaluator-context-dependent native functions.

## Key Changes

### 1. Created `test_resources/jank/core.jrs`

A comprehensive core library with:

**Control Flow Macros:**
- `when` - evaluates body when test is truthy
- `when-not` - evaluates body when test is falsy
- `cond` - conditional with multiple clauses

**Sequence Functions:**
- `second`, `ffirst`, `nfirst`, `fnext`, `nnext` - navigation
- `not-empty` - returns nil if coll is empty

**Higher-Order Functions (using loop/recur):**
- `map` - applies f to each item
- `filter` - returns items matching predicate
- `reduce` - reduces collection with function
- `some` - returns first truthy predicate result
- `every?` - checks all items match predicate
- `not-any?` - checks no items match predicate

**More Sequence Functions:**
- `take`, `drop` - take/drop n items
- `take-while`, `drop-while` - take/drop while predicate
- `interleave` - interleave two collections
- `interpose` - separate items with separator

**Collection Functions:**
- `select-keys` - select specific keys from map
- `zipmap` - create map from keys and vals

**Utility Functions:**
- `complement` - returns fn with opposite truth value
- `abs`, `sum`, `product` - numeric utilities
- `some?` - checks if x is not nil

### 2. Fixed Symbol Resolution Order (`src/runtime/eval.rs`)

**Problem:** Native functions in `global_env` were shadowing namespace-referred functions.

**Solution:** Changed resolution order to:
1. Check local bindings (let, fn params) via `lookup_local`
2. Check namespace registry (refers + current ns defs)
3. Fall back to `global_env` for native functions

```rust
// Before: natives shadow refers
if let Some(value) = env.lookup_symbol(sym) { return Ok(value); }
if let Some(value) = self.namespaces.resolve(sym) { return Ok(value); }

// After: refers shadow natives
if !Arc::ptr_eq(&env, &self.global_env) {
    if let Some(value) = env.lookup_local(sym) { return Ok(value); }
}
if let Some(value) = self.namespaces.resolve(sym) { return Ok(value); }
if let Some(value) = self.global_env.lookup_symbol(sym) { return Ok(value); }
```

### 3. Added `lookup_local` to Environment (`src/runtime/env.rs`)

New method that searches the local env chain but excludes the root (global_env):

```rust
pub fn lookup_local(&self, symbol: &Symbol) -> Option<Value> {
    // Check current bindings
    if let Some(value) = self.bindings.read().get(symbol.name()) {
        return Some(value.clone());
    }
    // Check parent if it has a parent (not root/global_env)
    if let Some(parent) = &self.parent {
        if parent.parent.is_some() {
            return parent.lookup_local(symbol);
        }
    }
    None
}
```

### 4. Added 8 core.jrs Tests

- `test_core_jrs_load` - verifies jank.core loads
- `test_core_jrs_when_macro` - tests when/when-not macros
- `test_core_jrs_sequence_functions` - tests second/ffirst/nnext
- `test_core_jrs_map_function` - tests map with function
- `test_core_jrs_filter_function` - tests filter with predicate
- `test_core_jrs_reduce_function` - tests reduce with +/*
- `test_core_jrs_every_and_some` - tests every?/some predicates
- `test_core_jrs_take_drop` - tests take/drop functions

## Test Results

```
running 155 tests
...
test_core_jrs_load ... ok
test_core_jrs_when_macro ... ok
test_core_jrs_sequence_functions ... ok
test_core_jrs_map_function ... ok
test_core_jrs_filter_function ... ok
test_core_jrs_reduce_function ... ok
test_core_jrs_every_and_some ... ok
test_core_jrs_take_drop ... ok
...
test result: ok. 155 passed; 0 failed
```

## Architecture Notes

**Why loop/recur instead of native implementations:**

The native `map`/`filter`/`reduce` functions need evaluator context to call the passed function. Rather than threading the evaluator through every native function (complex, breaks abstraction), we implement these in pure jank using `loop`/`recur` which the evaluator already supports.

This mirrors how Clojure's core is mostly written in Clojure itself, with only fundamental primitives in Java.

**Resolution Priority:**
1. Local bindings (let, fn params) - highest priority
2. Namespace refers - shadow global natives
3. Namespace defs - current namespace definitions
4. Global natives - fallback for primitives like `+`, `first`, etc.

## Next Steps

- Add threading macros (`->`, `->>`)
- Add `if-let`, `when-let`, `if-some`, `when-some`
- Add `doseq`, `dotimes`
- Create `jank.string.jrs` for string utilities
- Create `jank.set.jrs` for set operations
