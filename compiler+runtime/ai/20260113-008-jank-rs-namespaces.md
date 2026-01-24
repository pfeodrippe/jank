# jank-rs: Namespace System (.jrs files)

**Date**: 2026-01-13
**Status**: IMPLEMENTING

## Goal

Create a namespace system for jank-rs that works like Clojure/jank:
- `.jrs` files contain Clojure-like code
- `(ns ...)` declares a namespace
- `(:require ...)` loads dependencies
- Symbols resolve across namespaces

## Example Usage

```clojure
;; src/myapp/core.jrs
(ns myapp.core
  (:require [myapp.math :as math]))

(defn main []
  (println (math/distance 0 0 3 4)))
```

```clojure
;; src/myapp/math.jrs
(ns myapp.math)

(defn distance [x1 y1 x2 y2]
  (sqrt (+ (* (- x2 x1) (- x2 x1))
           (* (- y2 y1) (- y2 y1)))))
```

## Architecture

### 1. Namespace Struct

```rust
pub struct Namespace {
    /// Fully qualified name (e.g., "myapp.core")
    name: String,
    /// Interned symbols defined in this namespace
    defs: HashMap<String, Value>,
    /// Aliases (:as) mapping
    aliases: HashMap<String, String>,
    /// Referred symbols (:refer)
    refers: HashMap<String, (String, String)>, // symbol -> (ns, original-name)
}
```

### 2. Namespace Registry

```rust
pub struct NamespaceRegistry {
    /// All loaded namespaces
    namespaces: HashMap<String, Namespace>,
    /// Current namespace
    current: String,
    /// Source paths for loading .jrs files
    source_paths: Vec<PathBuf>,
}
```

### 3. File Loading

```rust
impl NamespaceRegistry {
    /// Load a namespace from a .jrs file
    pub fn require(&mut self, ns_name: &str) -> JankResult<()> {
        // Convert ns name to path: myapp.math -> myapp/math.jrs
        let path = ns_name.replace('.', "/") + ".jrs";

        // Search source paths
        for source_path in &self.source_paths {
            let full_path = source_path.join(&path);
            if full_path.exists() {
                return self.load_file(&full_path, ns_name);
            }
        }

        Err(JankError::eval(format!("Namespace not found: {}", ns_name)))
    }

    fn load_file(&mut self, path: &Path, expected_ns: &str) -> JankResult<()> {
        let source = std::fs::read_to_string(path)?;
        // Parse and evaluate the file
        // The (ns ...) form must come first
        ...
    }
}
```

### 4. Symbol Resolution

```rust
fn resolve_symbol(&self, sym: &Symbol) -> JankResult<Value> {
    // Qualified: myapp.math/distance
    if let Some(ns_name) = sym.namespace() {
        let resolved_ns = self.aliases.get(ns_name)
            .unwrap_or(&ns_name.to_string());
        return self.namespaces.get(resolved_ns)
            .and_then(|ns| ns.defs.get(sym.name()))
            .cloned()
            .ok_or_else(|| JankError::undefined(sym.name()));
    }

    // Check refers
    if let Some((ns, name)) = self.refers.get(sym.name()) {
        return self.namespaces.get(ns)
            .and_then(|ns| ns.defs.get(name))
            .cloned()
            .ok_or_else(|| JankError::undefined(sym.name()));
    }

    // Check current namespace
    let current = self.namespaces.get(&self.current).unwrap();
    current.defs.get(sym.name())
        .cloned()
        .ok_or_else(|| JankError::undefined(sym.name()))
}
```

## Implementation Steps

### Step 1: Create Namespace struct
- `src/runtime/namespace.rs`
- Store defs, aliases, refers

### Step 2: Create NamespaceRegistry
- Track all loaded namespaces
- Manage current namespace

### Step 3: Implement (ns ...) form
- Parse namespace declaration
- Handle (:require ...)
- Handle (:use ...) - maybe later

### Step 4: Implement (require ...) function
- Load .jrs files
- Parse and evaluate
- Handle circular dependencies

### Step 5: Update Evaluator
- Use namespace registry for symbol resolution
- Update defn, def to use current namespace

## Files

- `src/runtime/namespace.rs` (NEW) - Namespace and registry
- `src/runtime/eval.rs` - Update to use namespaces
- `src/runtime/mod.rs` - Export namespace module

## Success Criteria

1. Can load .jrs files with `(ns ...)` declarations
2. `(:require [other.ns :as alias])` works
3. Qualified symbols `ns/name` resolve correctly
4. `(:refer ...)` imports specific symbols
