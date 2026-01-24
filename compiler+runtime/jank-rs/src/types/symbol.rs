//! Symbol type with interning
//!
//! Symbols are identifiers that can have an optional namespace.
//! They are interned for efficient comparison and memory usage.

use std::fmt;
use std::hash::{Hash, Hasher};

use lasso::{Spur, ThreadedRodeo};
use once_cell::sync::Lazy;

/// Global string interner for symbols and keywords
pub static INTERNER: Lazy<ThreadedRodeo> = Lazy::new(ThreadedRodeo::default);

/// A Clojure symbol with optional namespace.
///
/// Symbols are interned for efficient comparison and memory usage.
/// Example symbols: `foo`, `bar/baz`, `clojure.core/map`
#[derive(Clone, Copy)]
pub struct Symbol {
    /// Optional namespace (interned)
    namespace: Option<Spur>,
    /// Symbol name (interned)
    name: Spur,
}

impl Symbol {
    /// Create a new symbol without namespace
    pub fn new(name: &str) -> Self {
        Symbol {
            namespace: None,
            name: INTERNER.get_or_intern(name),
        }
    }

    /// Create a new symbol with namespace
    pub fn with_namespace(namespace: &str, name: &str) -> Self {
        Symbol {
            namespace: Some(INTERNER.get_or_intern(namespace)),
            name: INTERNER.get_or_intern(name),
        }
    }

    /// Parse a symbol from a string (handles namespace/name format)
    pub fn parse(s: &str) -> Self {
        if let Some(idx) = s.find('/') {
            if idx > 0 && idx < s.len() - 1 {
                let ns = &s[..idx];
                let name = &s[idx + 1..];
                return Symbol::with_namespace(ns, name);
            }
        }
        Symbol::new(s)
    }

    /// Get the symbol's name
    pub fn name(&self) -> &str {
        INTERNER.resolve(&self.name)
    }

    /// Get the symbol's namespace (if any)
    pub fn namespace(&self) -> Option<&str> {
        self.namespace.map(|ns| INTERNER.resolve(&ns))
    }

    /// Check if the symbol has a namespace
    pub fn has_namespace(&self) -> bool {
        self.namespace.is_some()
    }

    /// Get the full qualified name (namespace/name or just name)
    pub fn full_name(&self) -> String {
        match self.namespace {
            Some(ns) => format!("{}/{}", INTERNER.resolve(&ns), INTERNER.resolve(&self.name)),
            None => INTERNER.resolve(&self.name).to_string(),
        }
    }

    /// Get the interned name key (for efficient lookups)
    pub fn name_key(&self) -> Spur {
        self.name
    }

    /// Get the interned namespace key (for efficient lookups)
    pub fn namespace_key(&self) -> Option<Spur> {
        self.namespace
    }
}

impl fmt::Display for Symbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.namespace {
            Some(ns) => write!(f, "{}/{}", INTERNER.resolve(&ns), INTERNER.resolve(&self.name)),
            None => write!(f, "{}", INTERNER.resolve(&self.name)),
        }
    }
}

impl fmt::Debug for Symbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Symbol({})", self)
    }
}

impl PartialEq for Symbol {
    fn eq(&self, other: &Self) -> bool {
        // Fast path: compare interned keys directly
        self.name == other.name && self.namespace == other.namespace
    }
}

impl Eq for Symbol {}

impl Hash for Symbol {
    fn hash<H: Hasher>(&self, state: &mut H) {
        // Hash the interned keys directly
        self.namespace.hash(state);
        self.name.hash(state);
    }
}

impl From<&str> for Symbol {
    fn from(s: &str) -> Self {
        Symbol::parse(s)
    }
}

impl From<String> for Symbol {
    fn from(s: String) -> Self {
        Symbol::parse(&s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_symbol() {
        let sym = Symbol::new("foo");
        assert_eq!(sym.name(), "foo");
        assert!(sym.namespace().is_none());
        assert_eq!(sym.to_string(), "foo");
    }

    #[test]
    fn test_namespaced_symbol() {
        let sym = Symbol::with_namespace("clojure.core", "map");
        assert_eq!(sym.name(), "map");
        assert_eq!(sym.namespace(), Some("clojure.core"));
        assert_eq!(sym.to_string(), "clojure.core/map");
    }

    #[test]
    fn test_parse_symbol() {
        let sym1 = Symbol::parse("foo");
        assert_eq!(sym1.name(), "foo");
        assert!(sym1.namespace().is_none());

        let sym2 = Symbol::parse("bar/baz");
        assert_eq!(sym2.name(), "baz");
        assert_eq!(sym2.namespace(), Some("bar"));
    }

    #[test]
    fn test_symbol_equality() {
        let sym1 = Symbol::new("foo");
        let sym2 = Symbol::new("foo");
        let sym3 = Symbol::new("bar");

        assert_eq!(sym1, sym2);
        assert_ne!(sym1, sym3);

        let sym4 = Symbol::with_namespace("ns", "foo");
        assert_ne!(sym1, sym4);
    }

    #[test]
    fn test_interning() {
        let sym1 = Symbol::new("interned-symbol");
        let sym2 = Symbol::new("interned-symbol");

        // Same interned key
        assert_eq!(sym1.name_key(), sym2.name_key());
    }
}
