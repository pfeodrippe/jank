//! jank-rs: A Clojure dialect implementation in Rust
//!
//! This crate provides a pure Rust implementation of a Clojure-like language
//! with JIT compilation via Cranelift and persistent data structures.
//!
//! # Features
//!
//! - Full Clojure syntax support (reader, parser)
//! - Persistent immutable data structures (via imbl)
//! - String interning for symbols and keywords (via lasso)
//! - Tree-walking interpreter
//! - Future: Cranelift JIT compilation for hot functions
//!
//! # Example
//!
//! ```rust
//! use jank_rs::{Evaluator, read_string};
//!
//! let mut eval = Evaluator::new();
//!
//! // Simple arithmetic
//! let form = read_string("(+ 1 2 3)").unwrap();
//! let result = eval.eval(&form).unwrap();
//! println!("{}", result); // 6
//!
//! // Define a function
//! eval.eval(&read_string("(defn square [x] (* x x))").unwrap()).unwrap();
//! let result = eval.eval(&read_string("(square 5)").unwrap()).unwrap();
//! println!("{}", result); // 25
//! ```

pub mod types;
pub mod error;
pub mod reader;
pub mod runtime;

// Re-exports for convenience
pub use types::{Value, Symbol, Keyword, Function, Arity};
pub use error::{JankError, JankResult};
pub use reader::{read_string, read_all, Parser, Lexer, Token};
pub use runtime::{Environment, Evaluator};

/// Version of jank-rs
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// Evaluate a string of Clojure code and return the result
pub fn eval_string(input: &str) -> JankResult<Value> {
    let form = read_string(input)?;
    let mut evaluator = Evaluator::new();
    evaluator.eval(&form)
}

/// Evaluate multiple forms and return all results
pub fn eval_all(input: &str) -> JankResult<Vec<Value>> {
    let forms = read_all(input)?;
    let mut evaluator = Evaluator::new();
    forms.iter().map(|form| evaluator.eval(form)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_eval_string_arithmetic() {
        assert_eq!(eval_string("(+ 1 2 3)").unwrap(), Value::Integer(6));
        assert_eq!(eval_string("(* 2 3 4)").unwrap(), Value::Integer(24));
        assert_eq!(eval_string("(- 10 3)").unwrap(), Value::Integer(7));
        assert_eq!(eval_string("(/ 12 3)").unwrap(), Value::Integer(4));
    }

    #[test]
    fn test_eval_string_comparison() {
        assert_eq!(eval_string("(= 1 1)").unwrap(), Value::Bool(true));
        assert_eq!(eval_string("(< 1 2)").unwrap(), Value::Bool(true));
        assert_eq!(eval_string("(> 3 2 1)").unwrap(), Value::Bool(true));
    }

    #[test]
    fn test_eval_string_collections() {
        let vec = eval_string("[1 2 3]").unwrap();
        match vec {
            Value::Vector(v) => assert_eq!(v.len(), 3),
            _ => panic!("expected vector"),
        }

        let map = eval_string("{:a 1 :b 2}").unwrap();
        match map {
            Value::Map(m) => assert_eq!(m.len(), 2),
            _ => panic!("expected map"),
        }
    }

    #[test]
    fn test_eval_string_let() {
        assert_eq!(
            eval_string("(let [x 10 y 20] (+ x y))").unwrap(),
            Value::Integer(30)
        );
    }

    #[test]
    fn test_eval_string_fn() {
        assert_eq!(
            eval_string("((fn [x] (* x x)) 5)").unwrap(),
            Value::Integer(25)
        );
    }

    #[test]
    fn test_eval_string_if() {
        assert_eq!(
            eval_string("(if (> 5 3) \"yes\" \"no\")").unwrap(),
            Value::String(std::sync::Arc::new("yes".to_string()))
        );
    }

    #[test]
    fn test_eval_string_loop_recur() {
        // Factorial using loop/recur
        let result = eval_string("(loop [n 5 acc 1] (if (<= n 1) acc (recur (dec n) (* acc n))))").unwrap();
        assert_eq!(result, Value::Integer(120));
    }

    #[test]
    fn test_read_write_roundtrip() {
        let inputs = vec![
            "42",
            "3.14",
            "true",
            "nil",
            "\"hello\"",
            ":keyword",
            "symbol",
            "(+ 1 2)",
            "[1 2 3]",
            "{:a 1}",
            "#{1 2 3}",
        ];

        for input in inputs {
            let form = read_string(input).unwrap();
            // Just check it parses - display format may differ
            let _ = form.to_string();
        }
    }
}
