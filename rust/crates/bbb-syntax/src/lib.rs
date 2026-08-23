//! bbb-syntax — BioLang 语法层（完全原生手写）。
//!
//! - `ast`：手写 AST（2026-08-22 起为唯一事实源，替代 MPS 生成骨架）
//! - `parser`：手写解析器（语法面覆盖 examples/01-17）
//! - `lexer`：手写词法器（零拷贝 token）
//!
//! `ast_generated` / `concepts_generated`：保留自 MPS 生成管线
//! （tools/mps_gen_rust.py），仅作历史对照，不再参与实现。

pub mod ast;
pub mod ast_generated;
pub mod concepts_generated;
pub mod lexer;
pub mod parser;

pub use ast::*;
pub use lexer::{LexError, Span, Token, TokenKind, tokenize};
pub use parser::{ParseError, Parser, parse_source};
