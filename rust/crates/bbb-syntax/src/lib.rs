//! bbb-syntax — BiuBiuBiu 语法层（完全原生手写）。
//!
//! - `ast`：手写 AST（唯一事实源）
//! - `parser`：手写解析器（语法面覆盖 examples/01-17）
//! - `lexer`：手写词法器（零拷贝 token）

pub mod ast;
pub mod lexer;
pub mod parser;

pub use ast::*;
pub use lexer::{LexError, Span, Token, TokenKind, tokenize};
pub use parser::{ParseError, Parser, parse_source};
