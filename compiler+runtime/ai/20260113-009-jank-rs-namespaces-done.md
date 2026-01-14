# jank-rs: Namespace System Complete

**Date**: 2026-01-13
**Status**: COMPLETE - 142 tests passing!

## Summary

Implemented a full namespace system for jank-rs that works like Clojure:
- `(ns myapp.core (:require ...))` - declare namespaces
- `(in-ns 'myapp.core)` - switch namespaces (REPL usage)
- `(require [other.ns :as alias])` - load dependencies
- `ns/name` qualified symbols - cross-namespace references

## Key Changes

### 1. `src/runtime/namespace.rs` (NEW)

```rust
// Namespace with definitions, aliases, and refers
pub struct Namespace {
    name: String,
    defs: HashMap<String, Value>,
    aliases: HashMap<String, String>,
    refers: HashMap<String, (String, String)>,
}

// Registry manages all namespaces
pub struct NamespaceRegistry {
    namespaces: HashMap<String, Namespace>,
    current: String,
    source_paths: Vec<PathBuf>,
    loading: Vec<String>,  // Cycle detection
}
```

### 2. `src/reader/parser.rs` (FIX)

```rust
// Before (BUG): Symbol::new(&name) - didn't parse ns/name
// After (FIX): Symbol::parse(&name) - properly handles qualified symbols
Token::Symbol(name) => {
    self.advance()?;
    Ok(Value::Symbol(Symbol::parse(&name)))  // <-- FIX
}
```

### 3. `src/runtime/eval.rs` (UPDATED)

**Symbol Resolution** - Now checks namespaces:
```rust
Value::Symbol(sym) => {
    // Qualified symbol (ns/name)
    if sym.namespace().is_some() {
        if let Some(value) = self.namespaces.resolve(sym) {
            return Ok(value);
        }
        return Err(JankError::undefined_symbol(...));
    }

    // Try local env, then namespace
    if let Some(value) = env.lookup_symbol(sym) {
        return Ok(value);
    }
    if let Some(value) = self.namespaces.resolve(sym) {
        return Ok(value);
    }
    Err(JankError::undefined_symbol(sym.name()))
}
```

**def and defn** - Now define in namespace (not global_env):
```rust
fn eval_def(...) {
    // Define in namespace for proper isolation
    self.namespaces.define(&name, value.clone());
    Ok(Value::Symbol(Symbol::new(&name)))
}
```

**New Special Forms**:
- `eval_ns` - Process `(ns myapp.core (:require ...))`
- `eval_require` - Load namespaces
- `eval_in_ns` - Switch current namespace

### 4. File Loading Infrastructure

```rust
fn load_namespace(&mut self, ns_name: &str) -> JankResult<()> {
    // Cycle detection
    if self.namespaces.is_loading(ns_name) {
        return Err(JankError::eval("circular dependency"));
    }

    // Find .jrs file
    let file_path = self.namespaces.find_ns_file(ns_name)?;

    // Parse and eval
    let source = std::fs::read_to_string(&file_path)?;
    self.eval_source(&source)?;
}
```

### 5. Alias Resolution in Nested Namespaces

Functions capture their defining namespace in `defining_ns` field:

```rust
// Function variants now include defining_ns
Function::Interpreted {
    ...
    defining_ns: Option<String>,  // NEW
}

// When calling a function, switch to its defining namespace
let prev_ns = self.namespaces.current_name().to_string();
if let Some(def_ns) = defining_ns {
    self.namespaces.switch_to(def_ns);
}
let result = self.eval_in_env(body, local_env);
self.namespaces.switch_to(&prev_ns);  // Restore
```

## Test .jrs Files

Created in `test_resources/`:

**`simple.jrs`**:
```clojure
(ns simple)
(def answer 42)
(defn greet [name] name)
```

**`myapp/math.jrs`**:
```clojure
(ns myapp.math)
(defn square [x] (* x x))
(defn cube [x] (* x x x))
```

**`myapp/core.jrs`**:
```clojure
(ns myapp.core
  (:require [myapp.math :as math]))
(defn area [side] (math/square side))  ;; Uses alias!
(defn volume [side] (math/cube side))
```

## Test Results

```
running 142 tests
...
test_load_simple_jrs_file ... ok
test_load_jrs_with_functions ... ok
test_load_jrs_with_alias ... ok
test_ns_with_require ... ok
test_ns_form_with_require ... ok
...
test result: ok. 142 passed; 0 failed
```

## Next: Phase 6 - Memory Control

The user requested custom allocators (arena, etc.) for complete memory control.
