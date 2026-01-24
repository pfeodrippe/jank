//! Error types for jank-rs
//!
//! This module defines all error types used throughout the jank runtime.

use std::fmt;
use thiserror::Error;

use crate::types::{Value, Symbol};

/// The main error type for jank operations
#[derive(Error, Debug)]
pub enum JankError {
    /// Error during lexical analysis
    #[error("Lexer error at position {position}: {message}")]
    LexerError {
        message: String,
        position: usize,
        line: usize,
        column: usize,
    },

    /// Error during parsing
    #[error("Parse error at line {line}: {message}")]
    ParseError {
        message: String,
        line: usize,
        column: usize,
    },

    /// Unexpected end of input
    #[error("Unexpected end of input: {0}")]
    UnexpectedEof(String),

    /// Unmatched delimiter
    #[error("Unmatched delimiter '{0}' at line {1}")]
    UnmatchedDelimiter(char, usize),

    /// Error during evaluation
    #[error("Eval error: {0}")]
    EvalError(String),

    /// Undefined symbol
    #[error("Unable to resolve symbol: {0}")]
    UndefinedSymbol(String),

    /// Wrong number of arguments
    #[error("Wrong number of arguments ({actual}) passed to {function}, expected {expected}")]
    ArityError {
        function: String,
        expected: String,
        actual: usize,
    },

    /// Type error
    #[error("Type error: expected {expected}, got {actual}")]
    TypeError {
        expected: String,
        actual: String,
    },

    /// Division by zero
    #[error("Division by zero")]
    DivisionByZero,

    /// Index out of bounds
    #[error("Index {index} out of bounds for collection of size {size}")]
    IndexOutOfBounds {
        index: i64,
        size: usize,
    },

    /// Key not found in map
    #[error("Key not found: {0}")]
    KeyNotFound(String),

    /// Invalid cast/coercion
    #[error("Cannot cast {from} to {to}")]
    CastError {
        from: String,
        to: String,
    },

    /// IO error
    #[error("IO error: {0}")]
    IoError(#[from] std::io::Error),

    /// Assertion error
    #[error("Assertion failed: {0}")]
    AssertionError(String),

    /// Stack overflow (recursion limit)
    #[error("Stack overflow - recursion limit exceeded")]
    StackOverflow,

    /// Compilation error
    #[error("Compilation error: {0}")]
    CompilationError(String),

    /// JIT compilation error
    #[error("JIT error: {0}")]
    JitError(String),

    /// Reader error
    #[error("Reader error: {0}")]
    ReaderError(String),

    /// Invalid argument
    #[error("Invalid argument to {function}: {message}")]
    InvalidArgument {
        function: String,
        message: String,
    },

    /// Not implemented
    #[error("Not implemented: {0}")]
    NotImplemented(String),

    /// Custom error (for user-defined exceptions)
    #[error("{0}")]
    Custom(String),
}

impl JankError {
    /// Create a lexer error
    pub fn lexer(message: impl Into<String>, position: usize, line: usize, column: usize) -> Self {
        JankError::LexerError {
            message: message.into(),
            position,
            line,
            column,
        }
    }

    /// Create a parse error
    pub fn parse(message: impl Into<String>, line: usize, column: usize) -> Self {
        JankError::ParseError {
            message: message.into(),
            line,
            column,
        }
    }

    /// Create an eval error
    pub fn eval(message: impl Into<String>) -> Self {
        JankError::EvalError(message.into())
    }

    /// Create an undefined symbol error
    pub fn undefined_symbol(name: impl Into<String>) -> Self {
        JankError::UndefinedSymbol(name.into())
    }

    /// Create an arity error
    pub fn arity(function: impl Into<String>, expected: impl Into<String>, actual: usize) -> Self {
        JankError::ArityError {
            function: function.into(),
            expected: expected.into(),
            actual,
        }
    }

    /// Create a type error
    pub fn type_error(expected: impl Into<String>, actual: impl Into<String>) -> Self {
        JankError::TypeError {
            expected: expected.into(),
            actual: actual.into(),
        }
    }

    /// Create an invalid argument error
    pub fn invalid_arg(function: impl Into<String>, message: impl Into<String>) -> Self {
        JankError::InvalidArgument {
            function: function.into(),
            message: message.into(),
        }
    }

    /// Create a not implemented error
    pub fn not_implemented(feature: impl Into<String>) -> Self {
        JankError::NotImplemented(feature.into())
    }
}

/// Result type for jank operations
pub type JankResult<T> = Result<T, JankError>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_error_display() {
        let err = JankError::undefined_symbol("foo");
        assert_eq!(err.to_string(), "Unable to resolve symbol: foo");

        let err = JankError::arity("+", "2", 3);
        assert_eq!(
            err.to_string(),
            "Wrong number of arguments (3) passed to +, expected 2"
        );
    }
}
