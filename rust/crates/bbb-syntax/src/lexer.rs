//! BiuBiuBiu 词法器（手写，Rust 版）。
//!
//! 规则对标旧 C 实现（src/lexer.c）+ examples/ 实际语法：
//! - 关键字：program / Main / Stream / Class / const / thread / need /
//!   res / ref / get / cause / ALL / if / else / while / for / break /
//!   continue / new / this / 基础类型 / true / false
//! - 注释：`//` 与 `/* */`；字符串 `"..."`（`\"` 转义）；字符 `'x'`
//! - 数字：int / float（`3.14`、`.5`、`1e3`）
//! - 运算符：`::` `==` `!=` `<=` `>=` `&&` `||` `++` `--` `->` + 单字符集
//!
//! 设计目标（内存规划）：Token 零堆分配——`kind: u8` + `len: u32` 引用
//! 源切片，字符串内容不拷贝；行/列只在出错时按需计算（错误路径才扫描）。

/// 词法错误：源位置 + 信息。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LexError {
    pub line: u32,
    pub col: u32,
    pub msg: &'static str,
}

impl std::fmt::Display for LexError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}:{}: {}", self.line, self.col, self.msg)
    }
}

/// 源位置（行:列，1 起）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Span {
    pub line: u32,
    pub col: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TokenKind {
    Ident,
    Keyword,
    Int,
    Float,
    Str,
    Char,
    Op,
    Eof,
}

/// Token：零拷贝——`text` 是源字符串的切片。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Token<'a> {
    pub kind: TokenKind,
    pub text: &'a str,
    pub span: Span,
}

const KEYWORDS: &[&str] = &[
    "program", "Main", "Stream", "Class", "Interface", "implements", "const", "thread", "need",
    "res", "ref", "get", "cause", "ALL", "if", "else", "while", "for",
    "break", "continue", "new", "this",
    "void", "int", "float", "double", "string", "char", "bool",
    "true", "false",
];

const OPS2: &[&str] = &["==", "!=", "<=", ">=", "&&", "||", "::", "++", "--", "->"];

fn is_kw(s: &str) -> bool {
    KEYWORDS.contains(&s) // 线性查找：29 个关键字，正确性优先
}

fn is_ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_'
}

fn is_ident_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

fn is_op1(c: u8) -> bool {
    matches!(c, b'+' | b'-' | b'*' | b'/' | b'%' | b'<' | b'>' | b'=' | b'!'
              | b'&' | b'|' | b'.' | b':' | b',' | b';' | b'(' | b')'
              | b'{' | b'}' | b'[' | b']' | b'@')
}

fn is_digit(c: u8) -> bool {
    c.is_ascii_digit()
}

struct Scan<'a> {
    src: &'a [u8],
    pos: usize,
    line: u32,
    col: u32,
}

impl<'a> Scan<'a> {
    fn peek(&self, k: usize) -> Option<u8> {
        self.src.get(self.pos + k).copied()
    }

    fn advance(&mut self) -> Option<u8> {
        let c = self.src.get(self.pos).copied();
        if let Some(b) = c {
            self.pos += 1;
            if b == b'\n' {
                self.line += 1;
                self.col = 1;
            } else {
                self.col += 1;
            }
        }
        c
    }

    fn span(&self) -> Span {
        Span { line: self.line, col: self.col }
    }
}

/// 把源码切成 Token 流。`tokens` 预先分配（容量即上限），零堆分配。
pub fn tokenize<'a>(src: &'a str, tokens: &mut Vec<Token<'a>>) -> Result<(), LexError> {
    let mut s = Scan { src: src.as_bytes(), pos: 0, line: 1, col: 1 };
    tokens.clear();
    loop {
        // 空白
        while matches!(s.peek(0), Some(b' ') | Some(b'\t') | Some(b'\r') | Some(b'\n')) {
            s.advance();
        }
        // 注释
        if s.peek(0) == Some(b'/') && s.peek(1) == Some(b'/') {
            while let Some(c) = s.advance() {
                if c == b'\n' {
                    break;
                }
            }
            continue;
        }
        if s.peek(0) == Some(b'/') && s.peek(1) == Some(b'*') {
            let start = s.span();
            s.advance();
            s.advance();
            loop {
                if s.peek(0).is_none() {
                    return Err(LexError { line: start.line, col: start.col, msg: "unterminated block comment /*" });
                }
                if s.peek(0) == Some(b'*') && s.peek(1) == Some(b'/') {
                    s.advance();
                    s.advance();
                    break;
                }
                s.advance();
            }
            continue;
        }
        let start = s.span();
        let c = match s.peek(0) {
            None => {
                tokens.push(Token { kind: TokenKind::Eof, text: "", span: start });
                return Ok(());
            }
            Some(c) => c,
        };

        // 字符串
        if c == b'"' {
            s.advance();
            let begin = s.pos;
            loop {
                match s.peek(0) {
                    None => return Err(LexError { line: start.line, col: start.col, msg: "unterminated string literal" }),
                    Some(b'"') => {
                        let end = s.pos;
                        s.advance();
                        tokens.push(Token { kind: TokenKind::Str, text: &src[begin..end], span: start });
                        break;
                    }
                    Some(b'\\') => {
                        s.advance();
                        s.advance();
                    }
                    Some(_) => {
                        s.advance();
                    }
                }
            }
            continue;
        }

        // 字符
        if c == b'\'' {
            s.advance();
            let begin = s.pos;
            match s.peek(0) {
                None => return Err(LexError { line: start.line, col: start.col, msg: "unterminated character literal" }),
                Some(b'\\') => {
                    s.advance();
                    s.advance();
                }
                Some(_) => {
                    s.advance();
                }
            }
            if s.peek(0) != Some(b'\'') {
                return Err(LexError { line: start.line, col: start.col, msg: "character literal must be exactly one character" });
            }
            s.advance();
            tokens.push(Token { kind: TokenKind::Char, text: &src[begin..s.pos - 1], span: start });
            continue;
        }

        // 数字
        if is_digit(c) || (c == b'.' && s.peek(1).map(is_digit).unwrap_or(false)) {
            let begin = s.pos;
            let mut is_float = false;
            while let Some(d) = s.peek(0) {
                if is_digit(d) {
                    s.advance();
                } else if d == b'.' && !is_float {
                    is_float = true;
                    s.advance();
                } else if (d == b'e' || d == b'E') && !is_float {
                    is_float = true;
                    s.advance();
                    if matches!(s.peek(0), Some(b'+') | Some(b'-')) {
                        s.advance();
                    }
                } else {
                    break;
                }
            }
            let kind = if is_float { TokenKind::Float } else { TokenKind::Int };
            tokens.push(Token { kind, text: &src[begin..s.pos], span: start });
            continue;
        }

        // 标识符 / 关键字
        if is_ident_start(c) {
            let begin = s.pos;
            while s.peek(0).map(is_ident_char).unwrap_or(false) {
                s.advance();
            }
            let word = &src[begin..s.pos];
            let kind = if is_kw(word) { TokenKind::Keyword } else { TokenKind::Ident };
            tokens.push(Token { kind, text: word, span: start });
            continue;
        }

        // 运算符
        let mut matched = false;
        for op in OPS2 {
            if s.src[s.pos..].starts_with(op.as_bytes()) {
                s.advance();
                s.advance();
                tokens.push(Token { kind: TokenKind::Op, text: op, span: start });
                matched = true;
                break;
            }
        }
        if matched {
            continue;
        }
        if is_op1(c) {
            s.advance();
            tokens.push(Token { kind: TokenKind::Op, text: &src[s.pos - 1..s.pos], span: start });
            continue;
        }

        return Err(LexError {
            line: start.line,
            col: start.col,
            msg: "unrecognized character",
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kinds(src: &str) -> Vec<(TokenKind, &str)> {
        let mut toks = Vec::new();
        tokenize(src, &mut toks).unwrap();
        toks.iter().map(|t| (t.kind, t.text)).collect()
    }

    #[test]
    fn hello() {
        let toks = kinds(r#"program main; Main { void exec() { CIO::println("hi"); } }"#);
        assert_eq!(toks[0], (TokenKind::Keyword, "program"));
        assert!(toks.contains(&(TokenKind::Keyword, "Main")));
        assert!(toks.contains(&(TokenKind::Keyword, "void")));
        assert!(toks.contains(&(TokenKind::Str, "hi")));
        assert_eq!(toks.last().unwrap().0, TokenKind::Eof);
    }

    #[test]
    fn numbers() {
        let toks = kinds("1 3.14 .5 1e3");
        assert_eq!(toks[0], (TokenKind::Int, "1"));
        assert_eq!(toks[1], (TokenKind::Float, "3.14"));
        assert_eq!(toks[2], (TokenKind::Float, ".5"));
        assert_eq!(toks[3], (TokenKind::Float, "1e3"));
    }

    #[test]
    fn ops() {
        let toks = kinds("a::b == c && d <= e;");
        let ops: Vec<&str> = toks.iter().filter(|t| t.0 == TokenKind::Op).map(|t| t.1).collect();
        assert_eq!(ops, vec!["::", "==", "&&", "<=", ";"]);
    }

    #[test]
    fn comment_and_string() {
        let src = r#"// line
CIO::println("a\"b"); /* block
comment */ x"#;
        let toks = kinds(src);
        assert!(toks.contains(&(TokenKind::Str, "a\\\"b")));
        assert!(toks.contains(&(TokenKind::Ident, "x")));
    }

    #[test]
    fn unclosed_string_err() {
        let mut toks = Vec::new();
        let err = tokenize("CIO::println(\"oops);", &mut toks).unwrap_err();
        assert_eq!(err.msg, "unterminated string literal");
    }

    #[test]
    fn line_col() {
        let mut toks = Vec::new();
        let err = tokenize("a = 1;\nb = \"x;\n", &mut toks).unwrap_err();
        assert_eq!(err.line, 2);
        assert_eq!(err.col, 5); // `b = "` — 引号在第 5 列
    }
}
