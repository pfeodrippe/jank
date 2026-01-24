//! Namespace system for jank-rs
//!
//! Provides Clojure-like namespace support:
//! - (ns myapp.core (:require [other.ns :as alias]))
//! - Qualified symbols: ns/name
//! - Loading .jrs files

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::error::{JankError, JankResult};
use crate::types::{Value, Symbol, Keyword};

/// A namespace containing definitions
#[derive(Debug, Clone)]
pub struct Namespace {
    /// Fully qualified namespace name (e.g., "myapp.core")
    name: String,
    /// Definitions in this namespace (symbol name -> value)
    defs: HashMap<String, Value>,
    /// Aliases from :as (alias -> full ns name)
    aliases: HashMap<String, String>,
    /// Referred symbols from :refer (local name -> (namespace, original name))
    refers: HashMap<String, (String, String)>,
}

impl Namespace {
    /// Create a new namespace
    pub fn new(name: impl Into<String>) -> Self {
        Namespace {
            name: name.into(),
            defs: HashMap::new(),
            aliases: HashMap::new(),
            refers: HashMap::new(),
        }
    }

    /// Get the namespace name
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Define a value in this namespace
    pub fn define(&mut self, name: impl Into<String>, value: Value) {
        self.defs.insert(name.into(), value);
    }

    /// Get a definition from this namespace
    pub fn get(&self, name: &str) -> Option<&Value> {
        self.defs.get(name)
    }

    /// Check if a symbol is defined in this namespace
    pub fn contains(&self, name: &str) -> bool {
        self.defs.contains_key(name)
    }

    /// Add an alias for another namespace
    pub fn add_alias(&mut self, alias: impl Into<String>, ns_name: impl Into<String>) {
        self.aliases.insert(alias.into(), ns_name.into());
    }

    /// Get the namespace name for an alias
    pub fn resolve_alias(&self, alias: &str) -> Option<&String> {
        self.aliases.get(alias)
    }

    /// Add a referred symbol from another namespace
    pub fn add_refer(&mut self, local_name: impl Into<String>, ns_name: impl Into<String>, original_name: impl Into<String>) {
        self.refers.insert(local_name.into(), (ns_name.into(), original_name.into()));
    }

    /// Get the source of a referred symbol
    pub fn resolve_refer(&self, name: &str) -> Option<&(String, String)> {
        self.refers.get(name)
    }

    /// Iterate over all definitions
    pub fn iter_defs(&self) -> impl Iterator<Item = (&String, &Value)> {
        self.defs.iter()
    }
}

/// Registry of all loaded namespaces
#[derive(Debug)]
pub struct NamespaceRegistry {
    /// All loaded namespaces
    namespaces: HashMap<String, Namespace>,
    /// The current namespace name
    current: String,
    /// Source paths for loading .jrs files
    source_paths: Vec<PathBuf>,
    /// Namespaces currently being loaded (for cycle detection)
    loading: Vec<String>,
}

impl NamespaceRegistry {
    /// Create a new namespace registry with a default "user" namespace
    pub fn new() -> Self {
        let mut registry = NamespaceRegistry {
            namespaces: HashMap::new(),
            current: "user".to_string(),
            source_paths: vec![PathBuf::from("."), PathBuf::from("src")],
            loading: Vec::new(),
        };

        // Create the default "user" namespace
        registry.namespaces.insert("user".to_string(), Namespace::new("user"));

        // Note: clojure.core is NOT pre-created here - it will be loaded from
        // clojure/core.jrs by Evaluator::load_clojure_core()

        registry
    }

    /// Add a source path for loading .jrs files
    pub fn add_source_path(&mut self, path: impl Into<PathBuf>) {
        self.source_paths.push(path.into());
    }

    /// Get the current namespace
    pub fn current(&self) -> &Namespace {
        self.namespaces.get(&self.current).expect("Current namespace must exist")
    }

    /// Get the current namespace mutably
    pub fn current_mut(&mut self) -> &mut Namespace {
        self.namespaces.get_mut(&self.current).expect("Current namespace must exist")
    }

    /// Get the current namespace name
    pub fn current_name(&self) -> &str {
        &self.current
    }

    /// Switch to a namespace (creating it if it doesn't exist)
    pub fn switch_to(&mut self, name: impl Into<String>) {
        let name = name.into();
        if !self.namespaces.contains_key(&name) {
            self.namespaces.insert(name.clone(), Namespace::new(&name));
        }
        self.current = name;
    }

    /// Get a namespace by name
    pub fn get(&self, name: &str) -> Option<&Namespace> {
        self.namespaces.get(name)
    }

    /// Get a namespace mutably
    pub fn get_mut(&mut self, name: &str) -> Option<&mut Namespace> {
        self.namespaces.get_mut(name)
    }

    /// Define a value in the current namespace
    pub fn define(&mut self, name: impl Into<String>, value: Value) {
        self.current_mut().define(name, value);
    }

    /// Resolve a symbol to its value
    ///
    /// Resolution order:
    /// 1. Qualified symbol (ns/name) - look up in that namespace
    /// 2. Aliased qualified (alias/name) - resolve alias, then look up
    /// 3. Referred symbol - look up in source namespace
    /// 4. Current namespace - look up directly
    /// 5. clojure.core - look up in core
    pub fn resolve(&self, sym: &Symbol) -> Option<Value> {
        // Check for qualified symbol
        if let Some(ns_name) = sym.namespace() {
            // Try as direct namespace
            if let Some(ns) = self.namespaces.get(ns_name) {
                return ns.get(sym.name()).cloned();
            }

            // Try as alias in current namespace
            let current = self.current();
            if let Some(resolved_ns) = current.resolve_alias(ns_name) {
                if let Some(ns) = self.namespaces.get(resolved_ns) {
                    return ns.get(sym.name()).cloned();
                }
            }

            return None;
        }

        // Check referred symbols in current namespace
        let current = self.current();
        if let Some((ns_name, original_name)) = current.resolve_refer(sym.name()) {
            if let Some(ns) = self.namespaces.get(ns_name) {
                return ns.get(original_name).cloned();
            }
        }

        // Check current namespace
        if let Some(val) = current.get(sym.name()) {
            return Some(val.clone());
        }

        // Check clojure.core (if not the current namespace)
        if self.current != "clojure.core" {
            if let Some(core) = self.namespaces.get("clojure.core") {
                if let Some(val) = core.get(sym.name()) {
                    return Some(val.clone());
                }
            }
        }

        None
    }

    /// Convert a namespace name to a file path
    /// myapp.core -> myapp/core.jrs
    pub fn ns_to_path(&self, ns_name: &str) -> PathBuf {
        let path = ns_name.replace('.', "/") + ".jrs";
        PathBuf::from(path)
    }

    /// Find a .jrs file for a namespace
    pub fn find_ns_file(&self, ns_name: &str) -> Option<PathBuf> {
        let relative_path = self.ns_to_path(ns_name);

        for source_path in &self.source_paths {
            let full_path = source_path.join(&relative_path);
            if full_path.exists() {
                return Some(full_path);
            }
        }

        None
    }

    /// Check if a namespace is currently being loaded (cycle detection)
    pub fn is_loading(&self, ns_name: &str) -> bool {
        self.loading.contains(&ns_name.to_string())
    }

    /// Mark a namespace as being loaded
    pub fn start_loading(&mut self, ns_name: &str) {
        self.loading.push(ns_name.to_string());
    }

    /// Mark a namespace as finished loading
    pub fn finish_loading(&mut self, ns_name: &str) {
        self.loading.retain(|n| n != ns_name);
    }

    /// Check if a namespace is loaded
    pub fn is_loaded(&self, ns_name: &str) -> bool {
        self.namespaces.contains_key(ns_name)
    }
}

impl Default for NamespaceRegistry {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_namespace_basic() {
        let mut ns = Namespace::new("test.ns");
        assert_eq!(ns.name(), "test.ns");

        ns.define("foo", Value::Integer(42));
        assert_eq!(ns.get("foo"), Some(&Value::Integer(42)));
        assert_eq!(ns.get("bar"), None);
        assert!(ns.contains("foo"));
        assert!(!ns.contains("bar"));
    }

    #[test]
    fn test_namespace_aliases() {
        let mut ns = Namespace::new("test.ns");
        ns.add_alias("m", "myapp.math");
        assert_eq!(ns.resolve_alias("m"), Some(&"myapp.math".to_string()));
        assert_eq!(ns.resolve_alias("x"), None);
    }

    #[test]
    fn test_namespace_refers() {
        let mut ns = Namespace::new("test.ns");
        ns.add_refer("sqrt", "myapp.math", "sqrt");
        assert_eq!(ns.resolve_refer("sqrt"), Some(&("myapp.math".to_string(), "sqrt".to_string())));
        assert_eq!(ns.resolve_refer("abs"), None);
    }

    #[test]
    fn test_registry_basic() {
        let mut registry = NamespaceRegistry::new();

        // Default namespace is "user"
        assert_eq!(registry.current_name(), "user");

        // Define in current namespace
        registry.define("x", Value::Integer(10));
        assert_eq!(registry.current().get("x"), Some(&Value::Integer(10)));

        // Switch to a new namespace
        registry.switch_to("test.ns");
        assert_eq!(registry.current_name(), "test.ns");

        // Define in new namespace
        registry.define("y", Value::Integer(20));
        assert_eq!(registry.current().get("y"), Some(&Value::Integer(20)));
        assert_eq!(registry.current().get("x"), None); // x is in user, not here
    }

    #[test]
    fn test_registry_resolve_simple() {
        let mut registry = NamespaceRegistry::new();
        registry.define("foo", Value::Integer(42));

        let sym = Symbol::new("foo");
        assert_eq!(registry.resolve(&sym), Some(Value::Integer(42)));

        let unknown = Symbol::new("unknown");
        assert_eq!(registry.resolve(&unknown), None);
    }

    #[test]
    fn test_registry_resolve_qualified() {
        let mut registry = NamespaceRegistry::new();

        // Create another namespace with a definition
        registry.switch_to("other.ns");
        registry.define("bar", Value::Integer(100));

        // Switch back to user
        registry.switch_to("user");

        // Resolve qualified symbol
        let sym = Symbol::with_namespace("other.ns", "bar");
        assert_eq!(registry.resolve(&sym), Some(Value::Integer(100)));
    }

    #[test]
    fn test_registry_resolve_alias() {
        let mut registry = NamespaceRegistry::new();

        // Create another namespace with a definition
        registry.switch_to("myapp.math");
        registry.define("sqrt", Value::Integer(999)); // Placeholder

        // Switch to user and add alias
        registry.switch_to("user");
        registry.current_mut().add_alias("math", "myapp.math");

        // Resolve through alias
        let sym = Symbol::with_namespace("math", "sqrt");
        assert_eq!(registry.resolve(&sym), Some(Value::Integer(999)));
    }

    #[test]
    fn test_registry_resolve_refer() {
        let mut registry = NamespaceRegistry::new();

        // Create another namespace with a definition
        registry.switch_to("myapp.math");
        registry.define("sqrt", Value::Integer(999));

        // Switch to user and add refer
        registry.switch_to("user");
        registry.current_mut().add_refer("sqrt", "myapp.math", "sqrt");

        // Resolve referred symbol (unqualified)
        let sym = Symbol::new("sqrt");
        assert_eq!(registry.resolve(&sym), Some(Value::Integer(999)));
    }

    #[test]
    fn test_ns_to_path() {
        let registry = NamespaceRegistry::new();
        assert_eq!(registry.ns_to_path("myapp.core"), PathBuf::from("myapp/core.jrs"));
        assert_eq!(registry.ns_to_path("myapp.util.math"), PathBuf::from("myapp/util/math.jrs"));
        assert_eq!(registry.ns_to_path("simple"), PathBuf::from("simple.jrs"));
    }

    #[test]
    fn test_loading_cycle_detection() {
        let mut registry = NamespaceRegistry::new();

        assert!(!registry.is_loading("myapp.core"));

        registry.start_loading("myapp.core");
        assert!(registry.is_loading("myapp.core"));

        registry.finish_loading("myapp.core");
        assert!(!registry.is_loading("myapp.core"));
    }
}
