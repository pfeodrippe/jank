//! Reader module for jank-rs
//!
//! This module handles lexing and parsing of Clojure source code.

pub mod lexer;
pub mod parser;

pub use lexer::{Lexer, Token, TokenWithLocation, Location};
pub use parser::Parser;

use crate::types::Value;
use crate::error::JankResult;

/// Read a string and return the parsed value
pub fn read_string(input: &str) -> JankResult<Value> {
    let mut parser = Parser::new(input);
    parser.read()
}

/// Read all forms from a string
pub fn read_all(input: &str) -> JankResult<Vec<Value>> {
    let mut parser = Parser::new(input);
    parser.read_all()
}
