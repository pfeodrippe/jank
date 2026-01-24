//! Parser for Clojure syntax
//!
//! Converts token stream into jank Values (AST).

use std::sync::Arc;

use imbl::{Vector as ImVector, HashMap as ImHashMap, HashSet as ImHashSet};

use crate::types::{Value, Symbol, Keyword};
use crate::error::{JankError, JankResult};
use super::lexer::{Lexer, Token, TokenWithLocation};

/// Parser for Clojure source code
pub struct Parser<'a> {
    lexer: Lexer<'a>,
    /// Current token (lookahead)
    current: TokenWithLocation,
}

impl<'a> Parser<'a> {
    /// Create a new parser for the given input
    pub fn new(input: &'a str) -> Self {
        let mut lexer = Lexer::new(input);
        let current = lexer.next_token().unwrap_or_else(|_| TokenWithLocation {
            token: Token::Eof,
            location: Default::default(),
        });
        Parser { lexer, current }
    }

    /// Advance to the next token
    fn advance(&mut self) -> JankResult<()> {
        self.current = self.lexer.next_token()?;
        Ok(())
    }

    /// Check if we're at end of input
    fn is_eof(&self) -> bool {
        matches!(self.current.token, Token::Eof)
    }

    /// Get the current token
    fn current_token(&self) -> &Token {
        &self.current.token
    }

    /// Get current location
    fn current_line(&self) -> usize {
        self.current.location.line
    }

    fn current_column(&self) -> usize {
        self.current.location.column
    }

    /// Read a single form
    pub fn read(&mut self) -> JankResult<Value> {
        if self.is_eof() {
            return Err(JankError::UnexpectedEof("expected a form".to_string()));
        }
        self.read_form()
    }

    /// Read all forms until EOF
    pub fn read_all(&mut self) -> JankResult<Vec<Value>> {
        let mut forms = Vec::new();
        while !self.is_eof() {
            forms.push(self.read_form()?);
        }
        Ok(forms)
    }

    /// Read a single form
    fn read_form(&mut self) -> JankResult<Value> {
        let token = self.current.token.clone();
        let line = self.current_line();

        match token {
            Token::LeftParen => self.read_list(),
            Token::LeftBracket => self.read_vector(),
            Token::LeftBrace => self.read_map(),
            Token::Quote => self.read_quoted("quote"),
            Token::SyntaxQuote => self.read_quoted("syntax-quote"),
            Token::Unquote => self.read_quoted("unquote"),
            Token::UnquoteSplice => self.read_quoted("unquote-splicing"),
            Token::Deref => self.read_quoted("deref"),
            Token::Meta => self.read_with_meta(),
            Token::Dispatch | Token::Var => self.read_dispatch(),
            Token::Nil => {
                self.advance()?;
                Ok(Value::Nil)
            }
            Token::True => {
                self.advance()?;
                Ok(Value::Bool(true))
            }
            Token::False => {
                self.advance()?;
                Ok(Value::Bool(false))
            }
            Token::Integer(n) => {
                self.advance()?;
                Ok(Value::Integer(n))
            }
            Token::Float(f) => {
                self.advance()?;
                Ok(Value::Float(f))
            }
            Token::String(s) => {
                self.advance()?;
                Ok(Value::String(Arc::new(s)))
            }
            Token::Symbol(name) => {
                self.advance()?;
                // Use parse to handle qualified symbols like ns/name
                Ok(Value::Symbol(Symbol::parse(&name)))
            }
            Token::Keyword(name) => {
                self.advance()?;
                Ok(Value::Keyword(Keyword::new(&name)))
            }
            Token::Char(c) => {
                self.advance()?;
                Ok(Value::Char(c))
            }
            Token::RightParen | Token::RightBracket | Token::RightBrace => {
                Err(JankError::UnmatchedDelimiter(
                    match token {
                        Token::RightParen => ')',
                        Token::RightBracket => ']',
                        Token::RightBrace => '}',
                        _ => unreachable!(),
                    },
                    line,
                ))
            }
            Token::Eof => {
                Err(JankError::UnexpectedEof("expected a form".to_string()))
            }
        }
    }

    /// Read a list (...)
    fn read_list(&mut self) -> JankResult<Value> {
        self.advance()?; // consume '('
        let mut elements = Vec::new();

        loop {
            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected ')'".to_string()));
            }
            if matches!(self.current_token(), Token::RightParen) {
                self.advance()?; // consume ')'
                return Ok(Value::list(elements));
            }
            elements.push(self.read_form()?);
        }
    }

    /// Read a vector [...]
    fn read_vector(&mut self) -> JankResult<Value> {
        self.advance()?; // consume '['
        let mut elements = ImVector::new();

        loop {
            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected ']'".to_string()));
            }
            if matches!(self.current_token(), Token::RightBracket) {
                self.advance()?; // consume ']'
                return Ok(Value::Vector(Arc::new(elements)));
            }
            elements.push_back(self.read_form()?);
        }
    }

    /// Read a map {...}
    fn read_map(&mut self) -> JankResult<Value> {
        self.advance()?; // consume '{'
        let mut map = ImHashMap::new();

        loop {
            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected '}'".to_string()));
            }
            if matches!(self.current_token(), Token::RightBrace) {
                self.advance()?; // consume '}'
                return Ok(Value::Map(Arc::new(map)));
            }

            let key = self.read_form()?;

            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected map value".to_string()));
            }
            if matches!(self.current_token(), Token::RightBrace) {
                return Err(JankError::ParseError {
                    message: "map literal must have even number of elements".to_string(),
                    line: self.current_line(),
                    column: self.current_column(),
                });
            }

            let value = self.read_form()?;
            map.insert(key, value);
        }
    }

    /// Read a quoted form 'x -> (quote x)
    fn read_quoted(&mut self, quote_type: &str) -> JankResult<Value> {
        self.advance()?; // consume quote character
        if self.is_eof() {
            return Err(JankError::UnexpectedEof(format!("expected form after {}", quote_type)));
        }
        let form = self.read_form()?;
        Ok(Value::list(vec![
            Value::Symbol(Symbol::new(quote_type)),
            form,
        ]))
    }

    /// Read form with metadata ^meta form
    fn read_with_meta(&mut self) -> JankResult<Value> {
        self.advance()?; // consume '^'
        if self.is_eof() {
            return Err(JankError::UnexpectedEof("expected metadata".to_string()));
        }

        let meta = self.read_form()?;
        if self.is_eof() {
            return Err(JankError::UnexpectedEof("expected form after metadata".to_string()));
        }
        let form = self.read_form()?;

        // Convert to (with-meta form meta)
        Ok(Value::list(vec![
            Value::Symbol(Symbol::new("with-meta")),
            form,
            meta,
        ]))
    }

    /// Read dispatch forms #...
    fn read_dispatch(&mut self) -> JankResult<Value> {
        // Check if this is #' (var quote)
        if matches!(self.current_token(), Token::Var) {
            self.advance()?;
            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected symbol after #'".to_string()));
            }
            let form = self.read_form()?;
            return Ok(Value::list(vec![
                Value::Symbol(Symbol::new("var")),
                form,
            ]));
        }

        self.advance()?; // consume '#'

        if self.is_eof() {
            return Err(JankError::UnexpectedEof("expected dispatch form".to_string()));
        }

        match self.current_token().clone() {
            Token::LeftBrace => self.read_set(),
            Token::LeftParen => self.read_anon_fn(),
            Token::String(s) => {
                // Regex literal #"pattern"
                self.advance()?;
                Ok(Value::Regex(Arc::new(s)))
            }
            Token::Symbol(s) if s == "_" => {
                // Discard form #_
                self.advance()?;
                if self.is_eof() {
                    return Err(JankError::UnexpectedEof("expected form to discard".to_string()));
                }
                // Read and discard the next form
                let _ = self.read_form()?;
                // Return the next form after the discarded one
                if self.is_eof() {
                    return Err(JankError::UnexpectedEof("expected form after discard".to_string()));
                }
                self.read_form()
            }
            _ => {
                // Tagged literal #tag value
                let tag = self.read_form()?;
                if self.is_eof() {
                    return Err(JankError::UnexpectedEof("expected value for tagged literal".to_string()));
                }
                let value = self.read_form()?;
                // For now, return as a list - proper tagged literal handling would need special support
                Ok(Value::list(vec![
                    Value::Symbol(Symbol::new("tagged-literal")),
                    tag,
                    value,
                ]))
            }
        }
    }

    /// Read a set #{...}
    fn read_set(&mut self) -> JankResult<Value> {
        self.advance()?; // consume '{'
        let mut set = ImHashSet::new();

        loop {
            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected '}'".to_string()));
            }
            if matches!(self.current_token(), Token::RightBrace) {
                self.advance()?; // consume '}'
                return Ok(Value::Set(Arc::new(set)));
            }
            set.insert(self.read_form()?);
        }
    }

    /// Read anonymous function #(...)
    fn read_anon_fn(&mut self) -> JankResult<Value> {
        // #(+ %1 %2) -> (fn [arg1 arg2] (+ arg1 arg2))
        self.advance()?; // consume '('
        let mut body = Vec::new();
        let mut max_arg = 0i32;
        let mut has_rest = false;

        loop {
            if self.is_eof() {
                return Err(JankError::UnexpectedEof("expected ')'".to_string()));
            }
            if matches!(self.current_token(), Token::RightParen) {
                self.advance()?; // consume ')'
                break;
            }
            let form = self.read_form()?;
            // Track argument usage
            self.find_arg_refs(&form, &mut max_arg, &mut has_rest);
            body.push(form);
        }

        // Build parameter vector
        let mut params = Vec::new();
        for i in 1..=max_arg {
            params.push(Value::Symbol(Symbol::new(&format!("arg{}", i))));
        }
        if has_rest {
            params.push(Value::Symbol(Symbol::new("&")));
            params.push(Value::Symbol(Symbol::new("rest-args")));
        }

        // Replace % placeholders in body
        let body = self.replace_arg_refs(Value::list(body));

        Ok(Value::list(vec![
            Value::Symbol(Symbol::new("fn")),
            Value::vector(params),
            body,
        ]))
    }

    /// Find argument references in a form (%1, %2, %, %&)
    fn find_arg_refs(&self, form: &Value, max_arg: &mut i32, has_rest: &mut bool) {
        match form {
            Value::Symbol(s) => {
                let name = s.name();
                if name == "%" || name == "%1" {
                    *max_arg = (*max_arg).max(1);
                } else if name.starts_with('%') {
                    if name == "%&" {
                        *has_rest = true;
                    } else if let Ok(n) = name[1..].parse::<i32>() {
                        *max_arg = (*max_arg).max(n);
                    }
                }
            }
            Value::List(list) => {
                for item in list.iter() {
                    self.find_arg_refs(item, max_arg, has_rest);
                }
            }
            Value::Vector(v) => {
                for item in v.iter() {
                    self.find_arg_refs(item, max_arg, has_rest);
                }
            }
            Value::Map(m) => {
                for (k, v) in m.iter() {
                    self.find_arg_refs(k, max_arg, has_rest);
                    self.find_arg_refs(v, max_arg, has_rest);
                }
            }
            Value::Set(s) => {
                for item in s.iter() {
                    self.find_arg_refs(item, max_arg, has_rest);
                }
            }
            _ => {}
        }
    }

    /// Replace argument references with actual parameter names
    fn replace_arg_refs(&self, form: Value) -> Value {
        match form {
            Value::Symbol(s) => {
                let name = s.name();
                if name == "%" || name == "%1" {
                    Value::Symbol(Symbol::new("arg1"))
                } else if name == "%&" {
                    Value::Symbol(Symbol::new("rest-args"))
                } else if name.starts_with('%') {
                    if let Ok(n) = name[1..].parse::<i32>() {
                        Value::Symbol(Symbol::new(&format!("arg{}", n)))
                    } else {
                        Value::Symbol(s)
                    }
                } else {
                    Value::Symbol(s)
                }
            }
            Value::List(list) => {
                let items: Vec<Value> = list.iter()
                    .map(|item| self.replace_arg_refs(item.clone()))
                    .collect();
                Value::list(items)
            }
            Value::Vector(v) => {
                let items: Vec<Value> = v.iter()
                    .map(|item| self.replace_arg_refs(item.clone()))
                    .collect();
                Value::vector(items)
            }
            Value::Map(m) => {
                let mut new_map = ImHashMap::new();
                for (k, v) in m.iter() {
                    new_map.insert(
                        self.replace_arg_refs(k.clone()),
                        self.replace_arg_refs(v.clone()),
                    );
                }
                Value::Map(Arc::new(new_map))
            }
            Value::Set(s) => {
                let mut new_set = ImHashSet::new();
                for item in s.iter() {
                    new_set.insert(self.replace_arg_refs(item.clone()));
                }
                Value::Set(Arc::new(new_set))
            }
            other => other,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(input: &str) -> JankResult<Value> {
        let mut parser = Parser::new(input);
        parser.read()
    }

    fn parse_all(input: &str) -> JankResult<Vec<Value>> {
        let mut parser = Parser::new(input);
        parser.read_all()
    }

    #[test]
    fn test_parse_nil() {
        assert_eq!(parse("nil").unwrap(), Value::Nil);
    }

    #[test]
    fn test_parse_booleans() {
        assert_eq!(parse("true").unwrap(), Value::Bool(true));
        assert_eq!(parse("false").unwrap(), Value::Bool(false));
    }

    #[test]
    fn test_parse_numbers() {
        assert_eq!(parse("42").unwrap(), Value::Integer(42));
        assert_eq!(parse("-17").unwrap(), Value::Integer(-17));
        assert_eq!(parse("3.14").unwrap(), Value::Float(3.14));
    }

    #[test]
    fn test_parse_strings() {
        assert_eq!(parse(r#""hello""#).unwrap(), Value::String(Arc::new("hello".to_string())));
    }

    #[test]
    fn test_parse_symbols() {
        let sym = parse("foo").unwrap();
        match sym {
            Value::Symbol(s) => assert_eq!(s.name(), "foo"),
            _ => panic!("expected symbol"),
        }
    }

    #[test]
    fn test_parse_keywords() {
        let kw = parse(":foo").unwrap();
        match kw {
            Value::Keyword(k) => assert_eq!(k.name(), "foo"),
            _ => panic!("expected keyword"),
        }
    }

    #[test]
    fn test_parse_list() {
        let list = parse("(+ 1 2)").unwrap();
        match list {
            Value::List(l) => assert_eq!(l.len(), 3),
            _ => panic!("expected list"),
        }
    }

    #[test]
    fn test_parse_vector() {
        let vec = parse("[1 2 3]").unwrap();
        match vec {
            Value::Vector(v) => assert_eq!(v.len(), 3),
            _ => panic!("expected vector"),
        }
    }

    #[test]
    fn test_parse_map() {
        let map = parse("{:a 1 :b 2}").unwrap();
        match map {
            Value::Map(m) => assert_eq!(m.len(), 2),
            _ => panic!("expected map"),
        }
    }

    #[test]
    fn test_parse_set() {
        let set = parse("#{1 2 3}").unwrap();
        match set {
            Value::Set(s) => assert_eq!(s.len(), 3),
            _ => panic!("expected set"),
        }
    }

    #[test]
    fn test_parse_quote() {
        let quoted = parse("'foo").unwrap();
        match quoted {
            Value::List(l) => {
                assert_eq!(l.len(), 2);
            }
            _ => panic!("expected list"),
        }
    }

    #[test]
    fn test_parse_nested() {
        let nested = parse("(defn foo [x] (+ x 1))").unwrap();
        match nested {
            Value::List(l) => assert_eq!(l.len(), 4),
            _ => panic!("expected list"),
        }
    }

    #[test]
    fn test_parse_multiple_forms() {
        let forms = parse_all("1 2 3").unwrap();
        assert_eq!(forms.len(), 3);
        assert_eq!(forms[0], Value::Integer(1));
        assert_eq!(forms[1], Value::Integer(2));
        assert_eq!(forms[2], Value::Integer(3));
    }

    #[test]
    fn test_parse_empty_collections() {
        assert!(matches!(parse("()").unwrap(), Value::List(_)));
        assert!(matches!(parse("[]").unwrap(), Value::Vector(_)));
        assert!(matches!(parse("{}").unwrap(), Value::Map(_)));
        assert!(matches!(parse("#{}").unwrap(), Value::Set(_)));
    }
}
