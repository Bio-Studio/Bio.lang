//! bio-syntax — BioLang 语法层。
//!
//! - `ast_generated` / `concepts_generated`：由 tools/mps_gen_rust.py 从
//!   MPS 语言模型（languages/biolang/languageModels/structure.mps）生成，
//!   是语言结构的唯一事实源。**禁止手改**；手动优化复制为 `ast` 模块后再改。
//! - `lexer`：手写词法器（对标旧 C 实现的 token 规则）。

pub mod ast_generated;
pub mod concepts_generated;
pub mod lexer;

pub use ast_generated::*;
pub use lexer::{LexError, Span, Token, TokenKind, tokenize};
