//! Lexer for Clojure syntax
//!
//! This module tokenizes Clojure source code into tokens that the parser can consume.

use std::str::Chars;
use std::iter::Peekable;

use crate::error::{JankError, JankResult};

/// Token types produced by the lexer
#[derive(Debug, Clone, PartialEq)]
pub enum Token {
    // Delimiters
    LeftParen,      // (
    RightParen,     // )
    LeftBracket,    // [
    RightBracket,   // ]
    LeftBrace,      // {
    RightBrace,     // }

    // Literals
    Integer(i64),
    Float(f64),
    String(String),
    Char(char),

    // Identifiers
    Symbol(String),
    Keyword(String),

    // Special forms / reader macros
    Quote,          // '
    SyntaxQuote,    // `
    Unquote,        // ~
    UnquoteSplice,  // ~@
    Deref,          // @
    Meta,           // ^
    Dispatch,       // #
    Var,            // #'

    // Special
    Nil,
    True,
    False,

    // End of file
    Eof,
}

impl Token {
    /// Check if token is a value (not a delimiter or special)
    pub fn is_value(&self) -> bool {
        matches!(
            self,
            Token::Integer(_)
                | Token::Float(_)
                | Token::String(_)
                | Token::Char(_)
                | Token::Symbol(_)
                | Token::Keyword(_)
                | Token::Nil
                | Token::True
                | Token::False
        )
    }

    /// Check if token opens a collection
    pub fn opens_collection(&self) -> bool {
        matches!(
            self,
            Token::LeftParen | Token::LeftBracket | Token::LeftBrace
        )
    }

    /// Check if token closes a collection
    pub fn closes_collection(&self) -> bool {
        matches!(
            self,
            Token::RightParen | Token::RightBracket | Token::RightBrace
        )
    }
}

/// Location in source code
#[derive(Debug, Clone, Copy, Default)]
pub struct Location {
    pub position: usize,
    pub line: usize,
    pub column: usize,
}

/// Token with its source location
#[derive(Debug, Clone)]
pub struct TokenWithLocation {
    pub token: Token,
    pub location: Location,
}

/// The lexer for Clojure source code
pub struct Lexer<'a> {
    input: &'a str,
    chars: Peekable<Chars<'a>>,
    position: usize,
    line: usize,
    column: usize,
}

impl<'a> Lexer<'a> {
    /// Create a new lexer for the given input
    pub fn new(input: &'a str) -> Self {
        Lexer {
            input,
            chars: input.chars().peekable(),
            position: 0,
            line: 1,
            column: 1,
        }
    }

    /// Get the current location
    fn location(&self) -> Location {
        Location {
            position: self.position,
            line: self.line,
            column: self.column,
        }
    }

    /// Advance to the next character
    fn advance(&mut self) -> Option<char> {
        if let Some(c) = self.chars.next() {
            self.position += c.len_utf8();
            if c == '\n' {
                self.line += 1;
                self.column = 1;
            } else {
                self.column += 1;
            }
            Some(c)
        } else {
            None
        }
    }

    /// Peek at the next character without consuming it
    fn peek(&mut self) -> Option<char> {
        self.chars.peek().copied()
    }

    /// Peek at the character after next
    fn peek_next(&self) -> Option<char> {
        let mut chars = self.chars.clone();
        chars.next();
        chars.next()
    }

    /// Skip whitespace and comments
    fn skip_whitespace(&mut self) {
        while let Some(c) = self.peek() {
            if c.is_whitespace() || c == ',' {
                // Commas are whitespace in Clojure
                self.advance();
            } else if c == ';' {
                // Skip line comment
                while let Some(c) = self.peek() {
                    if c == '\n' {
                        break;
                    }
                    self.advance();
                }
            } else {
                break;
            }
        }
    }

    /// Read the next token
    pub fn next_token(&mut self) -> JankResult<TokenWithLocation> {
        self.skip_whitespace();

        let location = self.location();

        let token = match self.peek() {
            None => Token::Eof,
            Some(c) => match c {
                '(' => {
                    self.advance();
                    Token::LeftParen
                }
                ')' => {
                    self.advance();
                    Token::RightParen
                }
                '[' => {
                    self.advance();
                    Token::LeftBracket
                }
                ']' => {
                    self.advance();
                    Token::RightBracket
                }
                '{' => {
                    self.advance();
                    Token::LeftBrace
                }
                '}' => {
                    self.advance();
                    Token::RightBrace
                }
                '\'' => {
                    self.advance();
                    Token::Quote
                }
                '`' => {
                    self.advance();
                    Token::SyntaxQuote
                }
                '~' => {
                    self.advance();
                    if self.peek() == Some('@') {
                        self.advance();
                        Token::UnquoteSplice
                    } else {
                        Token::Unquote
                    }
                }
                '@' => {
                    self.advance();
                    Token::Deref
                }
                '^' => {
                    self.advance();
                    Token::Meta
                }
                '#' => {
                    self.advance();
                    match self.peek() {
                        Some('\'') => {
                            self.advance();
                            Token::Var
                        }
                        Some('{') => {
                            // Set literal - return dispatch and let parser handle it
                            Token::Dispatch
                        }
                        Some('(') => {
                            // Anonymous function shorthand
                            Token::Dispatch
                        }
                        Some('"') => {
                            // Regex literal
                            Token::Dispatch
                        }
                        Some('_') => {
                            // Discard reader macro
                            Token::Dispatch
                        }
                        _ => Token::Dispatch
                    }
                }
                '"' => self.read_string()?,
                ':' => self.read_keyword()?,
                '\\' => self.read_char()?,
                c if c.is_ascii_digit() => self.read_number()?,
                '-' | '+' => {
                    // Could be a number or symbol
                    if let Some(next) = self.peek_next() {
                        if next.is_ascii_digit() {
                            self.read_number()?
                        } else {
                            self.read_symbol()?
                        }
                    } else {
                        self.read_symbol()?
                    }
                }
                c if is_symbol_start(c) => self.read_symbol()?,
                _ => {
                    return Err(JankError::lexer(
                        format!("Unexpected character: '{}'", c),
                        self.position,
                        self.line,
                        self.column,
                    ));
                }
            },
        };

        Ok(TokenWithLocation { token, location })
    }

    /// Read a string literal
    fn read_string(&mut self) -> JankResult<Token> {
        let start = self.location();
        self.advance(); // Skip opening quote

        let mut string = String::new();

        loop {
            match self.advance() {
                None => {
                    return Err(JankError::lexer(
                        "Unterminated string",
                        start.position,
                        start.line,
                        start.column,
                    ));
                }
                Some('"') => break,
                Some('\\') => {
                    // Escape sequence
                    match self.advance() {
                        Some('n') => string.push('\n'),
                        Some('r') => string.push('\r'),
                        Some('t') => string.push('\t'),
                        Some('\\') => string.push('\\'),
                        Some('"') => string.push('"'),
                        Some('0') => string.push('\0'),
                        Some('u') => {
                            // Unicode escape \uXXXX
                            let mut code = String::new();
                            for _ in 0..4 {
                                if let Some(c) = self.advance() {
                                    code.push(c);
                                }
                            }
                            if let Ok(n) = u32::from_str_radix(&code, 16) {
                                if let Some(c) = char::from_u32(n) {
                                    string.push(c);
                                }
                            }
                        }
                        Some(c) => {
                            return Err(JankError::lexer(
                                format!("Invalid escape sequence: \\{}", c),
                                self.position,
                                self.line,
                                self.column,
                            ));
                        }
                        None => {
                            return Err(JankError::lexer(
                                "Unterminated escape sequence",
                                self.position,
                                self.line,
                                self.column,
                            ));
                        }
                    }
                }
                Some(c) => string.push(c),
            }
        }

        Ok(Token::String(string))
    }

    /// Read a keyword
    fn read_keyword(&mut self) -> JankResult<Token> {
        self.advance(); // Skip ':'

        // Handle :: for auto-resolved keywords
        let auto_resolve = if self.peek() == Some(':') {
            self.advance();
            true
        } else {
            false
        };

        let name = self.read_symbol_name();

        if name.is_empty() {
            return Err(JankError::lexer(
                "Invalid keyword: empty name",
                self.position,
                self.line,
                self.column,
            ));
        }

        let keyword = if auto_resolve {
            format!("::{}", name)
        } else {
            format!(":{}", name)
        };

        Ok(Token::Keyword(keyword))
    }

    /// Read a character literal
    fn read_char(&mut self) -> JankResult<Token> {
        self.advance(); // Skip '\'

        let c = match self.advance() {
            None => {
                return Err(JankError::lexer(
                    "Unterminated character literal",
                    self.position,
                    self.line,
                    self.column,
                ));
            }
            Some(c) => c,
        };

        // Check for named characters
        if c.is_alphabetic() {
            let mut name = String::from(c);
            while let Some(c) = self.peek() {
                if c.is_alphabetic() {
                    name.push(c);
                    self.advance();
                } else {
                    break;
                }
            }

            if name.len() > 1 {
                // Named character
                let ch = match name.as_str() {
                    "newline" => '\n',
                    "return" => '\r',
                    "space" => ' ',
                    "tab" => '\t',
                    "backspace" => '\x08',
                    "formfeed" => '\x0c',
                    _ => {
                        return Err(JankError::lexer(
                            format!("Unknown character name: {}", name),
                            self.position,
                            self.line,
                            self.column,
                        ));
                    }
                };
                return Ok(Token::Char(ch));
            }
        }

        Ok(Token::Char(c))
    }

    /// Read a number (integer or float)
    fn read_number(&mut self) -> JankResult<Token> {
        let start = self.position;
        let mut number = String::new();
        let mut is_float = false;
        let mut has_exponent = false;

        // Handle sign
        if let Some(c @ ('+' | '-')) = self.peek() {
            number.push(c);
            self.advance();
        }

        // Read digits before decimal
        while let Some(c) = self.peek() {
            if c.is_ascii_digit() {
                number.push(c);
                self.advance();
            } else if c == '_' {
                // Numeric separator (like 1_000_000)
                self.advance();
            } else {
                break;
            }
        }

        // Check for decimal point
        if self.peek() == Some('.') {
            if let Some(next) = self.peek_next() {
                if next.is_ascii_digit() {
                    is_float = true;
                    number.push('.');
                    self.advance();

                    // Read digits after decimal
                    while let Some(c) = self.peek() {
                        if c.is_ascii_digit() {
                            number.push(c);
                            self.advance();
                        } else if c == '_' {
                            self.advance();
                        } else {
                            break;
                        }
                    }
                }
            }
        }

        // Check for exponent
        if let Some(c @ ('e' | 'E')) = self.peek() {
            is_float = true;
            has_exponent = true;
            number.push(c);
            self.advance();

            // Optional sign
            if let Some(c @ ('+' | '-')) = self.peek() {
                number.push(c);
                self.advance();
            }

            // Read exponent digits
            while let Some(c) = self.peek() {
                if c.is_ascii_digit() {
                    number.push(c);
                    self.advance();
                } else {
                    break;
                }
            }
        }

        // Check for type suffixes
        match self.peek() {
            Some('N') => {
                // BigInt suffix - for now parse as integer
                self.advance();
                let n: i64 = number.parse().map_err(|_| {
                    JankError::lexer(
                        format!("Invalid integer: {}", number),
                        start,
                        self.line,
                        self.column,
                    )
                })?;
                return Ok(Token::Integer(n));
            }
            Some('M') => {
                // BigDecimal suffix - parse as float
                self.advance();
                let n: f64 = number.parse().map_err(|_| {
                    JankError::lexer(
                        format!("Invalid decimal: {}", number),
                        start,
                        self.line,
                        self.column,
                    )
                })?;
                return Ok(Token::Float(n));
            }
            _ => {}
        }

        if is_float {
            let n: f64 = number.parse().map_err(|_| {
                JankError::lexer(
                    format!("Invalid float: {}", number),
                    start,
                    self.line,
                    self.column,
                )
            })?;
            Ok(Token::Float(n))
        } else {
            let n: i64 = number.parse().map_err(|_| {
                JankError::lexer(
                    format!("Invalid integer: {}", number),
                    start,
                    self.line,
                    self.column,
                )
            })?;
            Ok(Token::Integer(n))
        }
    }

    /// Read a symbol
    fn read_symbol(&mut self) -> JankResult<Token> {
        let name = self.read_symbol_name();

        // Check for special symbols
        let token = match name.as_str() {
            "nil" => Token::Nil,
            "true" => Token::True,
            "false" => Token::False,
            _ => Token::Symbol(name),
        };

        Ok(token)
    }

    /// Read a symbol name (used by both symbols and keywords)
    fn read_symbol_name(&mut self) -> String {
        let mut name = String::new();

        while let Some(c) = self.peek() {
            if is_symbol_char(c) {
                name.push(c);
                self.advance();
            } else {
                break;
            }
        }

        name
    }

    /// Tokenize the entire input
    pub fn tokenize(&mut self) -> JankResult<Vec<TokenWithLocation>> {
        let mut tokens = Vec::new();

        loop {
            let token = self.next_token()?;
            let is_eof = token.token == Token::Eof;
            tokens.push(token);
            if is_eof {
                break;
            }
        }

        Ok(tokens)
    }
}

/// Check if a character can start a symbol
fn is_symbol_start(c: char) -> bool {
    c.is_alphabetic()
        || matches!(
            c,
            '*' | '+' | '!' | '-' | '_' | '?' | '<' | '>' | '=' | '&' | '/' | '.'
        )
}

/// Check if a character can be part of a symbol
fn is_symbol_char(c: char) -> bool {
    is_symbol_start(c) || c.is_ascii_digit() || c == '\'' || c == '#' || c == ':'
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tokenize(input: &str) -> Vec<Token> {
        let mut lexer = Lexer::new(input);
        lexer
            .tokenize()
            .unwrap()
            .into_iter()
            .map(|t| t.token)
            .collect()
    }

    #[test]
    fn test_delimiters() {
        assert_eq!(
            tokenize("()[]{}"),
            vec![
                Token::LeftParen,
                Token::RightParen,
                Token::LeftBracket,
                Token::RightBracket,
                Token::LeftBrace,
                Token::RightBrace,
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_numbers() {
        assert_eq!(
            tokenize("42 -17 3.14 -2.5 1e10"),
            vec![
                Token::Integer(42),
                Token::Integer(-17),
                Token::Float(3.14),
                Token::Float(-2.5),
                Token::Float(1e10),
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_strings() {
        assert_eq!(
            tokenize(r#""hello" "world\n""#),
            vec![
                Token::String("hello".to_string()),
                Token::String("world\n".to_string()),
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_symbols() {
        assert_eq!(
            tokenize("foo bar+ *baz* ns/name"),
            vec![
                Token::Symbol("foo".to_string()),
                Token::Symbol("bar+".to_string()),
                Token::Symbol("*baz*".to_string()),
                Token::Symbol("ns/name".to_string()),
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_keywords() {
        assert_eq!(
            tokenize(":foo :bar/baz"),
            vec![
                Token::Keyword(":foo".to_string()),
                Token::Keyword(":bar/baz".to_string()),
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_special_symbols() {
        assert_eq!(
            tokenize("nil true false"),
            vec![Token::Nil, Token::True, Token::False, Token::Eof]
        );
    }

    #[test]
    fn test_quote_and_friends() {
        assert_eq!(
            tokenize("'x `y ~z ~@w @a"),
            vec![
                Token::Quote,
                Token::Symbol("x".to_string()),
                Token::SyntaxQuote,
                Token::Symbol("y".to_string()),
                Token::Unquote,
                Token::Symbol("z".to_string()),
                Token::UnquoteSplice,
                Token::Symbol("w".to_string()),
                Token::Deref,
                Token::Symbol("a".to_string()),
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_comments() {
        assert_eq!(
            tokenize("foo ; comment\nbar"),
            vec![
                Token::Symbol("foo".to_string()),
                Token::Symbol("bar".to_string()),
                Token::Eof
            ]
        );
    }

    #[test]
    fn test_expression() {
        assert_eq!(
            tokenize("(+ 1 2)"),
            vec![
                Token::LeftParen,
                Token::Symbol("+".to_string()),
                Token::Integer(1),
                Token::Integer(2),
                Token::RightParen,
                Token::Eof
            ]
        );
    }
}
