//! Keyword type with interning
//!
//! Keywords are self-evaluating identifiers that start with a colon.
//! They are commonly used as map keys.

use std::fmt;
use std::hash::{Hash, Hasher};

use lasso::Spur;

use crate::types::symbol::INTERNER;

/// A Clojure keyword with optional namespace.
///
/// Keywords are interned for efficient comparison and memory usage.
/// Example keywords: `:foo`, `:bar/baz`, `::local` (namespace-qualified)
#[derive(Clone, Copy)]
pub struct Keyword {
    /// Optional namespace (interned)
    namespace: Option<Spur>,
    /// Keyword name (interned)
    name: Spur,
}

impl Keyword {
    /// Create a new keyword without namespace
    pub fn new(name: &str) -> Self {
        // Remove leading colon if present
        let name = name.strip_prefix(':').unwrap_or(name);
        Keyword {
            namespace: None,
            name: INTERNER.get_or_intern(name),
        }
    }

    /// Create a new keyword with namespace
    pub fn with_namespace(namespace: &str, name: &str) -> Self {
        // Remove leading colons if present
        let namespace = namespace.strip_prefix(':').unwrap_or(namespace);
        let name = name.strip_prefix(':').unwrap_or(name);
        Keyword {
            namespace: Some(INTERNER.get_or_intern(namespace)),
            name: INTERNER.get_or_intern(name),
        }
    }

    /// Parse a keyword from a string (handles :namespace/name format)
    pub fn parse(s: &str) -> Self {
        // Remove leading colon
        let s = s.strip_prefix(':').unwrap_or(s);

        if let Some(idx) = s.find('/') {
            if idx > 0 && idx < s.len() - 1 {
                let ns = &s[..idx];
                let name = &s[idx + 1..];
                return Keyword::with_namespace(ns, name);
            }
        }
        Keyword::new(s)
    }

    /// Get the keyword's name
    pub fn name(&self) -> &str {
        INTERNER.resolve(&self.name)
    }

    /// Get the keyword's namespace (if any)
    pub fn namespace(&self) -> Option<&str> {
        self.namespace.map(|ns| INTERNER.resolve(&ns))
    }

    /// Check if the keyword has a namespace
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

impl fmt::Display for Keyword {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.namespace {
            Some(ns) => write!(f, ":{}/{}", INTERNER.resolve(&ns), INTERNER.resolve(&self.name)),
            None => write!(f, ":{}", INTERNER.resolve(&self.name)),
        }
    }
}

impl fmt::Debug for Keyword {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Keyword({})", self)
    }
}

impl PartialEq for Keyword {
    fn eq(&self, other: &Self) -> bool {
        // Fast path: compare interned keys directly
        self.name == other.name && self.namespace == other.namespace
    }
}

impl Eq for Keyword {}

impl Hash for Keyword {
    fn hash<H: Hasher>(&self, state: &mut H) {
        // Hash the interned keys directly
        self.namespace.hash(state);
        self.name.hash(state);
    }
}

impl From<&str> for Keyword {
    fn from(s: &str) -> Self {
        Keyword::parse(s)
    }
}

impl From<String> for Keyword {
    fn from(s: String) -> Self {
        Keyword::parse(&s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_keyword() {
        let kw = Keyword::new("foo");
        assert_eq!(kw.name(), "foo");
        assert!(kw.namespace().is_none());
        assert_eq!(kw.to_string(), ":foo");
    }

    #[test]
    fn test_namespaced_keyword() {
        let kw = Keyword::with_namespace("ns", "key");
        assert_eq!(kw.name(), "key");
        assert_eq!(kw.namespace(), Some("ns"));
        assert_eq!(kw.to_string(), ":ns/key");
    }

    #[test]
    fn test_parse_keyword() {
        let kw1 = Keyword::parse(":foo");
        assert_eq!(kw1.name(), "foo");
        assert!(kw1.namespace().is_none());

        let kw2 = Keyword::parse(":bar/baz");
        assert_eq!(kw2.name(), "baz");
        assert_eq!(kw2.namespace(), Some("bar"));

        let kw3 = Keyword::parse("qux");
        assert_eq!(kw3.name(), "qux");
    }

    #[test]
    fn test_keyword_equality() {
        let kw1 = Keyword::new("foo");
        let kw2 = Keyword::new("foo");
        let kw3 = Keyword::new("bar");

        assert_eq!(kw1, kw2);
        assert_ne!(kw1, kw3);

        let kw4 = Keyword::with_namespace("ns", "foo");
        assert_ne!(kw1, kw4);
    }
}
