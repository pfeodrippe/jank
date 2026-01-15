//! Environment for variable bindings
//!
//! This module implements the lexical environment used during evaluation.

use std::collections::HashMap;
use std::sync::Arc;

use parking_lot::RwLock;

use crate::types::{Value, Symbol};
use crate::error::{JankError, JankResult};

/// An environment frame containing bindings
#[derive(Debug)]
pub struct Environment {
    /// Local bindings in this frame
    bindings: RwLock<HashMap<String, Value>>,
    /// Parent environment (for lexical scoping)
    parent: Option<Arc<Environment>>,
}

impl Environment {
    /// Create a new root environment
    pub fn new() -> Self {
        Environment {
            bindings: RwLock::new(HashMap::new()),
            parent: None,
        }
    }

    /// Create a new child environment with the given parent
    pub fn with_parent(parent: Arc<Environment>) -> Self {
        Environment {
            bindings: RwLock::new(HashMap::new()),
            parent: Some(parent),
        }
    }

    /// Create a new child environment (convenience method)
    pub fn child(self: &Arc<Self>) -> Self {
        Environment::with_parent(Arc::clone(self))
    }

    /// Define a binding in the current environment
    pub fn define(&self, name: impl Into<String>, value: Value) {
        self.bindings.write().insert(name.into(), value);
    }

    /// Define multiple bindings at once
    pub fn define_all(&self, bindings: impl IntoIterator<Item = (String, Value)>) {
        let mut env = self.bindings.write();
        for (name, value) in bindings {
            env.insert(name, value);
        }
    }

    /// Look up a binding by name (searches parent scopes)
    pub fn lookup(&self, name: &str) -> Option<Value> {
        // Check local bindings first
        if let Some(value) = self.bindings.read().get(name) {
            return Some(value.clone());
        }

        // Check parent scope
        if let Some(parent) = &self.parent {
            return parent.lookup(name);
        }

        None
    }

    /// Look up a binding or return an error
    pub fn get(&self, name: &str) -> JankResult<Value> {
        self.lookup(name)
            .ok_or_else(|| JankError::undefined_symbol(name))
    }

    /// Look up a binding by Symbol
    /// Uses full_name() to handle namespaced symbols like native/nil?
    pub fn lookup_symbol(&self, symbol: &Symbol) -> Option<Value> {
        // For namespaced symbols, use the full name
        if symbol.has_namespace() {
            self.lookup(&symbol.full_name())
        } else {
            self.lookup(symbol.name())
        }
    }

    /// Look up a binding in local scope chain, excluding the root (global_env)
    /// This is used to find let bindings and fn params without finding global natives
    pub fn lookup_local(&self, symbol: &Symbol) -> Option<Value> {
        // Check current bindings
        if let Some(value) = self.bindings.read().get(symbol.name()) {
            return Some(value.clone());
        }

        // Check parent if it has a parent (meaning it's not the root/global_env)
        if let Some(parent) = &self.parent {
            if parent.parent.is_some() {
                return parent.lookup_local(symbol);
            }
        }

        None
    }

    /// Get a binding by Symbol or return an error
    /// Uses full_name() to handle namespaced symbols like native/nil?
    pub fn get_symbol(&self, symbol: &Symbol) -> JankResult<Value> {
        if symbol.has_namespace() {
            self.get(&symbol.full_name())
        } else {
            self.get(symbol.name())
        }
    }

    /// Set a binding (must already exist in some scope)
    pub fn set(&self, name: &str, value: Value) -> JankResult<()> {
        // Check local bindings first
        {
            let mut bindings = self.bindings.write();
            if bindings.contains_key(name) {
                bindings.insert(name.to_string(), value);
                return Ok(());
            }
        }

        // Check parent scope
        if let Some(parent) = &self.parent {
            return parent.set(name, value);
        }

        Err(JankError::undefined_symbol(name))
    }

    /// Check if a binding exists
    pub fn contains(&self, name: &str) -> bool {
        if self.bindings.read().contains_key(name) {
            return true;
        }

        if let Some(parent) = &self.parent {
            return parent.contains(name);
        }

        false
    }

    /// Get all bindings in the current frame only (not parents)
    pub fn local_bindings(&self) -> HashMap<String, Value> {
        self.bindings.read().clone()
    }

    /// Get all bindings including parent scopes
    pub fn all_bindings(&self) -> HashMap<String, Value> {
        let mut result = HashMap::new();

        // Start with parent bindings (so local can override)
        if let Some(parent) = &self.parent {
            result.extend(parent.all_bindings());
        }

        // Add local bindings
        result.extend(self.bindings.read().clone());

        result
    }

    /// Create a new environment with let bindings
    pub fn with_let_bindings(
        parent: Arc<Environment>,
        bindings: impl IntoIterator<Item = (String, Value)>,
    ) -> Self {
        let env = Environment::with_parent(parent);
        env.define_all(bindings);
        env
    }

    /// Create a new environment with function parameters bound to arguments
    pub fn with_fn_args(
        parent: Arc<Environment>,
        params: &[Symbol],
        args: &[Value],
        variadic_param: Option<&Symbol>,
    ) -> JankResult<Self> {
        let env = Environment::with_parent(parent);

        // Bind fixed parameters
        for (param, arg) in params.iter().zip(args.iter()) {
            env.define(param.name(), arg.clone());
        }

        // Handle variadic parameter
        if let Some(rest_param) = variadic_param {
            let rest_args: Vec<Value> = args.iter().skip(params.len()).cloned().collect();
            env.define(rest_param.name(), Value::list(rest_args));
        }

        Ok(env)
    }
}

impl Default for Environment {
    fn default() -> Self {
        Environment::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_define_and_lookup() {
        let env = Environment::new();
        env.define("x", Value::integer(42));

        assert_eq!(env.lookup("x"), Some(Value::integer(42)));
        assert_eq!(env.lookup("y"), None);
    }

    #[test]
    fn test_scoping() {
        let parent = Arc::new(Environment::new());
        parent.define("x", Value::integer(1));
        parent.define("y", Value::integer(2));

        let child = Environment::with_parent(parent);
        child.define("x", Value::integer(10)); // Shadow parent's x

        assert_eq!(child.lookup("x"), Some(Value::integer(10))); // Shadowed
        assert_eq!(child.lookup("y"), Some(Value::integer(2))); // From parent
        assert_eq!(child.lookup("z"), None);
    }

    #[test]
    fn test_set_in_parent() {
        let parent = Arc::new(Environment::new());
        parent.define("x", Value::integer(1));

        let child = Environment::with_parent(Arc::clone(&parent));

        // Setting in child should modify parent's binding
        child.set("x", Value::integer(10)).unwrap();

        // Both should see the change
        assert_eq!(parent.lookup("x"), Some(Value::integer(10)));
        assert_eq!(child.lookup("x"), Some(Value::integer(10)));
    }
}
