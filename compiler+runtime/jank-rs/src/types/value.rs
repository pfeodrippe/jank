//! Core Clojure value types for jank-rs
//!
//! This module defines the fundamental value representation used throughout
//! the jank runtime. All Clojure values are represented as `Value` enums.

use std::fmt;
use std::hash::{Hash, Hasher};
use std::sync::Arc;

use imbl::{HashMap as ImHashMap, Vector as ImVector};
use lasso::Spur;

use crate::types::symbol::Symbol;
use crate::types::keyword::Keyword;
use crate::types::function::Function;

/// The core value type representing all Clojure values.
///
/// This enum uses `Arc` for reference counting on heap-allocated values,
/// enabling efficient structural sharing for persistent data structures.
#[derive(Clone)]
pub enum Value {
    /// nil - the absence of a value
    Nil,

    /// Boolean true or false
    Bool(bool),

    /// 64-bit signed integer
    Integer(i64),

    /// 64-bit floating point
    Float(f64),

    /// Unicode string
    String(Arc<String>),

    /// Symbol (interned identifier)
    Symbol(Symbol),

    /// Keyword (interned, self-evaluating identifier)
    Keyword(Keyword),

    /// Persistent vector (array-like, indexed access)
    Vector(Arc<ImVector<Value>>),

    /// Persistent list (linked list, efficient prepend)
    List(Arc<List>),

    /// Persistent hash map
    Map(Arc<ImHashMap<Value, Value>>),

    /// Persistent hash set
    Set(Arc<imbl::HashSet<Value>>),

    /// Function (both native Rust functions and Clojure-defined)
    Function(Arc<Function>),

    /// Special form marker (for quote, if, fn, etc.)
    SpecialForm(&'static str),

    /// Atom (mutable reference for concurrency)
    Atom(Arc<parking_lot::RwLock<Value>>),

    /// Character
    Char(char),

    /// Ratio (for exact arithmetic) - numerator/denominator
    Ratio(i64, i64),

    /// Big integer (arbitrary precision) - stored as string for now
    BigInt(Arc<String>),

    /// Regex pattern
    Regex(Arc<String>),

    /// Recur signal (for loop/recur - not a user-visible value)
    Recur(Vec<Value>),
}

/// A persistent list (cons cell)
#[derive(Clone)]
pub struct List {
    pub first: Value,
    pub rest: Option<Arc<List>>,
    pub count: usize,
}

impl List {
    /// Create an empty list
    pub fn empty() -> Arc<List> {
        Arc::new(List {
            first: Value::Nil,
            rest: None,
            count: 0,
        })
    }

    /// Create a list with one element
    pub fn new(first: Value) -> Arc<List> {
        Arc::new(List {
            first,
            rest: None,
            count: 1,
        })
    }

    /// Cons (prepend) a value to the front
    pub fn cons(first: Value, rest: Arc<List>) -> Arc<List> {
        let count = rest.count + 1;
        Arc::new(List {
            first,
            rest: Some(rest),
            count,
        })
    }

    /// Check if list is empty
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Get the length
    pub fn len(&self) -> usize {
        self.count
    }

    /// Get the first element
    pub fn first(&self) -> &Value {
        &self.first
    }

    /// Get the first element if list is non-empty (alias for clarity)
    pub fn head(&self) -> Option<Value> {
        if self.count == 0 {
            None
        } else {
            Some(self.first.clone())
        }
    }

    /// Get the rest of the list
    pub fn rest(&self) -> Option<&Arc<List>> {
        self.rest.as_ref()
    }

    /// Convert to an iterator
    pub fn iter(&self) -> ListIter {
        ListIter { current: Some(self) }
    }
}

/// Iterator over a list
pub struct ListIter<'a> {
    current: Option<&'a List>,
}

impl<'a> Iterator for ListIter<'a> {
    type Item = &'a Value;

    fn next(&mut self) -> Option<Self::Item> {
        match self.current {
            Some(list) if list.count > 0 => {
                let value = &list.first;
                self.current = list.rest.as_ref().map(|l| l.as_ref());
                Some(value)
            }
            _ => None,
        }
    }
}

impl Value {
    // ============ Constructors ============

    /// Create a nil value
    pub fn nil() -> Self {
        Value::Nil
    }

    /// Create a boolean value
    pub fn bool(b: bool) -> Self {
        Value::Bool(b)
    }

    /// Create an integer value
    pub fn integer(n: i64) -> Self {
        Value::Integer(n)
    }

    /// Create a float value
    pub fn float(n: f64) -> Self {
        Value::Float(n)
    }

    /// Create a string value
    pub fn string(s: impl Into<String>) -> Self {
        Value::String(Arc::new(s.into()))
    }

    /// Create an empty vector
    pub fn empty_vector() -> Self {
        Value::Vector(Arc::new(ImVector::new()))
    }

    /// Create a vector from values
    pub fn vector(values: impl IntoIterator<Item = Value>) -> Self {
        Value::Vector(Arc::new(values.into_iter().collect()))
    }

    /// Create an empty list
    pub fn empty_list() -> Self {
        Value::List(List::empty())
    }

    /// Create a list from values (first value is first in list)
    pub fn list(values: impl IntoIterator<Item = Value>) -> Self {
        let values: Vec<_> = values.into_iter().collect();
        if values.is_empty() {
            return Value::List(List::empty());
        }
        let mut list = List::new(values[values.len() - 1].clone());
        for i in (0..values.len() - 1).rev() {
            list = List::cons(values[i].clone(), list);
        }
        Value::List(list)
    }

    /// Create an empty map
    pub fn empty_map() -> Self {
        Value::Map(Arc::new(ImHashMap::new()))
    }

    /// Create a map from key-value pairs
    pub fn map(pairs: impl IntoIterator<Item = (Value, Value)>) -> Self {
        Value::Map(Arc::new(pairs.into_iter().collect()))
    }

    /// Create an empty set
    pub fn empty_set() -> Self {
        Value::Set(Arc::new(imbl::HashSet::new()))
    }

    /// Create a set from values
    pub fn set(values: impl IntoIterator<Item = Value>) -> Self {
        Value::Set(Arc::new(values.into_iter().collect()))
    }

    // ============ Type predicates ============

    /// Check if value is nil
    pub fn is_nil(&self) -> bool {
        matches!(self, Value::Nil)
    }

    /// Check if value is truthy (not nil and not false)
    pub fn is_truthy(&self) -> bool {
        !matches!(self, Value::Nil | Value::Bool(false))
    }

    /// Check if value is falsy (nil or false)
    pub fn is_falsy(&self) -> bool {
        matches!(self, Value::Nil | Value::Bool(false))
    }

    /// Check if value is a boolean
    pub fn is_bool(&self) -> bool {
        matches!(self, Value::Bool(_))
    }

    /// Check if value is an integer
    pub fn is_integer(&self) -> bool {
        matches!(self, Value::Integer(_))
    }

    /// Check if value is a float
    pub fn is_float(&self) -> bool {
        matches!(self, Value::Float(_))
    }

    /// Check if value is a number (integer or float)
    pub fn is_number(&self) -> bool {
        matches!(self, Value::Integer(_) | Value::Float(_))
    }

    /// Check if value is a string
    pub fn is_string(&self) -> bool {
        matches!(self, Value::String(_))
    }

    /// Check if value is a symbol
    pub fn is_symbol(&self) -> bool {
        matches!(self, Value::Symbol(_))
    }

    /// Check if value is a keyword
    pub fn is_keyword(&self) -> bool {
        matches!(self, Value::Keyword(_))
    }

    /// Check if value is a vector
    pub fn is_vector(&self) -> bool {
        matches!(self, Value::Vector(_))
    }

    /// Check if value is a list
    pub fn is_list(&self) -> bool {
        matches!(self, Value::List(_))
    }

    /// Check if value is a map
    pub fn is_map(&self) -> bool {
        matches!(self, Value::Map(_))
    }

    /// Check if value is a set
    pub fn is_set(&self) -> bool {
        matches!(self, Value::Set(_))
    }

    /// Check if value is a function
    pub fn is_function(&self) -> bool {
        matches!(self, Value::Function(_))
    }

    /// Check if value is a collection (vector, list, map, or set)
    pub fn is_coll(&self) -> bool {
        matches!(
            self,
            Value::Vector(_) | Value::List(_) | Value::Map(_) | Value::Set(_)
        )
    }

    /// Check if value is sequential (vector or list)
    pub fn is_sequential(&self) -> bool {
        matches!(self, Value::Vector(_) | Value::List(_))
    }

    // ============ Type coercion ============

    /// Try to get as integer
    pub fn as_integer(&self) -> Option<i64> {
        match self {
            Value::Integer(n) => Some(*n),
            _ => None,
        }
    }

    /// Try to get as float
    pub fn as_float(&self) -> Option<f64> {
        match self {
            Value::Float(n) => Some(*n),
            Value::Integer(n) => Some(*n as f64),
            _ => None,
        }
    }

    /// Try to get as string
    pub fn as_string(&self) -> Option<&str> {
        match self {
            Value::String(s) => Some(s.as_str()),
            _ => None,
        }
    }

    /// Try to get as symbol
    pub fn as_symbol(&self) -> Option<&Symbol> {
        match self {
            Value::Symbol(s) => Some(s),
            _ => None,
        }
    }

    /// Try to get as keyword
    pub fn as_keyword(&self) -> Option<&Keyword> {
        match self {
            Value::Keyword(k) => Some(k),
            _ => None,
        }
    }

    /// Try to get as vector
    pub fn as_vector(&self) -> Option<&Arc<ImVector<Value>>> {
        match self {
            Value::Vector(v) => Some(v),
            _ => None,
        }
    }

    /// Try to get as list
    pub fn as_list(&self) -> Option<&Arc<List>> {
        match self {
            Value::List(l) => Some(l),
            _ => None,
        }
    }

    /// Try to get as map
    pub fn as_map(&self) -> Option<&Arc<ImHashMap<Value, Value>>> {
        match self {
            Value::Map(m) => Some(m),
            _ => None,
        }
    }

    /// Try to get as function
    pub fn as_function(&self) -> Option<&Arc<Function>> {
        match self {
            Value::Function(f) => Some(f),
            _ => None,
        }
    }

    // ============ Collection operations ============

    /// Get the count of elements (works for all collections)
    pub fn count(&self) -> Option<usize> {
        match self {
            Value::Nil => Some(0),
            Value::String(s) => Some(s.len()),
            Value::Vector(v) => Some(v.len()),
            Value::List(l) => Some(l.count),
            Value::Map(m) => Some(m.len()),
            Value::Set(s) => Some(s.len()),
            _ => None,
        }
    }

    /// Check if collection is empty
    pub fn is_empty_coll(&self) -> Option<bool> {
        self.count().map(|c| c == 0)
    }

    /// Get the type name as a string
    pub fn type_name(&self) -> &'static str {
        match self {
            Value::Nil => "nil",
            Value::Bool(_) => "boolean",
            Value::Integer(_) => "integer",
            Value::Float(_) => "float",
            Value::String(_) => "string",
            Value::Symbol(_) => "symbol",
            Value::Keyword(_) => "keyword",
            Value::Vector(_) => "vector",
            Value::List(_) => "list",
            Value::Map(_) => "map",
            Value::Set(_) => "set",
            Value::Function(_) => "function",
            Value::SpecialForm(_) => "special-form",
            Value::Atom(_) => "atom",
            Value::Char(_) => "char",
            Value::Ratio(_, _) => "ratio",
            Value::BigInt(_) => "bigint",
            Value::Regex(_) => "regex",
            Value::Recur(_) => "recur",
        }
    }
}

// ============ Display implementation ============

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Nil => write!(f, "nil"),
            Value::Bool(b) => write!(f, "{}", b),
            Value::Integer(n) => write!(f, "{}", n),
            Value::Float(n) => {
                if n.fract() == 0.0 {
                    write!(f, "{}.0", n)
                } else {
                    write!(f, "{}", n)
                }
            }
            Value::String(s) => write!(f, "\"{}\"", s.escape_default()),
            Value::Symbol(s) => write!(f, "{}", s),
            Value::Keyword(k) => write!(f, "{}", k),
            Value::Vector(v) => {
                write!(f, "[")?;
                for (i, item) in v.iter().enumerate() {
                    if i > 0 {
                        write!(f, " ")?;
                    }
                    write!(f, "{}", item)?;
                }
                write!(f, "]")
            }
            Value::List(l) => {
                write!(f, "(")?;
                for (i, item) in l.iter().enumerate() {
                    if i > 0 {
                        write!(f, " ")?;
                    }
                    write!(f, "{}", item)?;
                }
                write!(f, ")")
            }
            Value::Map(m) => {
                write!(f, "{{")?;
                for (i, (k, v)) in m.iter().enumerate() {
                    if i > 0 {
                        write!(f, ", ")?;
                    }
                    write!(f, "{} {}", k, v)?;
                }
                write!(f, "}}")
            }
            Value::Set(s) => {
                write!(f, "#{{")?;
                for (i, item) in s.iter().enumerate() {
                    if i > 0 {
                        write!(f, " ")?;
                    }
                    write!(f, "{}", item)?;
                }
                write!(f, "}}")
            }
            Value::Function(func) => write!(f, "{}", func),
            Value::SpecialForm(name) => write!(f, "#<special-form {}>", name),
            Value::Atom(a) => write!(f, "#<atom {}>", a.read()),
            Value::Char(c) => write!(f, "\\{}", c),
            Value::Ratio(num, denom) => write!(f, "{}/{}", num, denom),
            Value::BigInt(n) => write!(f, "{}N", n),
            Value::Regex(r) => write!(f, "#\"{}\"", r),
            Value::Recur(args) => {
                write!(f, "#<recur")?;
                for arg in args {
                    write!(f, " {}", arg)?;
                }
                write!(f, ">")
            }
        }
    }
}

impl fmt::Debug for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self)
    }
}

// ============ Equality implementation ============

impl PartialEq for Value {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Value::Nil, Value::Nil) => true,
            (Value::Bool(a), Value::Bool(b)) => a == b,
            (Value::Integer(a), Value::Integer(b)) => a == b,
            (Value::Float(a), Value::Float(b)) => a == b,
            (Value::Integer(a), Value::Float(b)) => (*a as f64) == *b,
            (Value::Float(a), Value::Integer(b)) => *a == (*b as f64),
            (Value::String(a), Value::String(b)) => a == b,
            (Value::Symbol(a), Value::Symbol(b)) => a == b,
            (Value::Keyword(a), Value::Keyword(b)) => a == b,
            (Value::Vector(a), Value::Vector(b)) => a == b,
            (Value::List(a), Value::List(b)) => {
                if a.count != b.count {
                    return false;
                }
                a.iter().zip(b.iter()).all(|(x, y)| x == y)
            }
            (Value::Map(a), Value::Map(b)) => a == b,
            (Value::Set(a), Value::Set(b)) => a == b,
            (Value::Char(a), Value::Char(b)) => a == b,
            (Value::Ratio(an, ad), Value::Ratio(bn, bd)) => an * bd == bn * ad,
            (Value::BigInt(a), Value::BigInt(b)) => a == b,
            (Value::Regex(a), Value::Regex(b)) => a == b,
            // Functions are only equal if they're the same object
            (Value::Function(a), Value::Function(b)) => Arc::ptr_eq(a, b),
            (Value::Atom(a), Value::Atom(b)) => Arc::ptr_eq(a, b),
            // Recur values are equal if their args are equal
            (Value::Recur(a), Value::Recur(b)) => a == b,
            _ => false,
        }
    }
}

impl Eq for Value {}

// ============ Hash implementation ============

impl Hash for Value {
    fn hash<H: Hasher>(&self, state: &mut H) {
        std::mem::discriminant(self).hash(state);
        match self {
            Value::Nil => {}
            Value::Bool(b) => b.hash(state),
            Value::Integer(n) => n.hash(state),
            Value::Float(n) => n.to_bits().hash(state),
            Value::String(s) => s.hash(state),
            Value::Symbol(s) => s.hash(state),
            Value::Keyword(k) => k.hash(state),
            Value::Vector(v) => {
                v.len().hash(state);
                for item in v.iter() {
                    item.hash(state);
                }
            }
            Value::List(l) => {
                l.count.hash(state);
                for item in l.iter() {
                    item.hash(state);
                }
            }
            Value::Map(m) => {
                m.len().hash(state);
                // Maps hash by their entries (order-independent)
                let mut hash_sum: u64 = 0;
                for (k, v) in m.iter() {
                    let mut h = std::collections::hash_map::DefaultHasher::new();
                    k.hash(&mut h);
                    v.hash(&mut h);
                    hash_sum = hash_sum.wrapping_add(h.finish());
                }
                hash_sum.hash(state);
            }
            Value::Set(s) => {
                s.len().hash(state);
                let mut hash_sum: u64 = 0;
                for item in s.iter() {
                    let mut h = std::collections::hash_map::DefaultHasher::new();
                    item.hash(&mut h);
                    hash_sum = hash_sum.wrapping_add(h.finish());
                }
                hash_sum.hash(state);
            }
            Value::Function(f) => Arc::as_ptr(f).hash(state),
            Value::SpecialForm(name) => name.hash(state),
            Value::Atom(a) => Arc::as_ptr(a).hash(state),
            Value::Char(c) => c.hash(state),
            Value::Ratio(n, d) => {
                n.hash(state);
                d.hash(state);
            }
            Value::BigInt(n) => n.hash(state),
            Value::Regex(r) => r.hash(state),
            Value::Recur(args) => {
                args.len().hash(state);
                for arg in args {
                    arg.hash(state);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_value_creation() {
        let nil = Value::nil();
        assert!(nil.is_nil());

        let n = Value::integer(42);
        assert_eq!(n.as_integer(), Some(42));

        let s = Value::string("hello");
        assert_eq!(s.as_string(), Some("hello"));

        let v = Value::vector(vec![Value::integer(1), Value::integer(2)]);
        assert!(v.is_vector());
        assert_eq!(v.count(), Some(2));
    }

    #[test]
    fn test_truthiness() {
        assert!(!Value::Nil.is_truthy());
        assert!(!Value::Bool(false).is_truthy());
        assert!(Value::Bool(true).is_truthy());
        assert!(Value::integer(0).is_truthy());
        assert!(Value::string("").is_truthy());
        assert!(Value::empty_vector().is_truthy());
    }

    #[test]
    fn test_list_operations() {
        let list = Value::list(vec![Value::integer(1), Value::integer(2), Value::integer(3)]);
        if let Value::List(l) = &list {
            assert_eq!(l.count, 3);
            assert_eq!(l.first().as_integer(), Some(1));
        }
    }

    #[test]
    fn test_equality() {
        assert_eq!(Value::integer(42), Value::integer(42));
        assert_eq!(Value::integer(42), Value::Float(42.0));
        assert_ne!(Value::integer(42), Value::integer(43));

        let v1 = Value::vector(vec![Value::integer(1), Value::integer(2)]);
        let v2 = Value::vector(vec![Value::integer(1), Value::integer(2)]);
        assert_eq!(v1, v2);
    }
}
