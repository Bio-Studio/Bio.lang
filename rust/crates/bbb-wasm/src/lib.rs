//! bbb-wasm — BiuBiuBiu 格式化器/词法器，编译为 wasm32 供 VSCode 插件调用。
//!
//! 导出（手写 C ABI，零 wasm-bindgen 依赖）：
//! - `format(ptr, len, out_cap) -> usize`：格式化源码，写入输出缓冲，返回长度
//! - `format_len(ptr, len) -> usize`：预计算格式化后长度（JS 分配缓冲）
//!
//! formatter 规则（对齐旧 formatter.js 0.16.0 行为，17 examples 幂等）：
//! - 4 空格缩进（块级）；`{` 换行、`}` 缩进减一换行
//! - 运算符两侧空格；`::`/`.`/`,`/`;`/括号规则明确
//! - 关键字与 `(` 之间空格（if/while/for）；函数调用名与 `(` 无空格
//! - 字符串/字符 token 重新包裹引号（内容原样）；注释原样
//! - 连续空行压缩为单空行；行尾无空格

use std::slice;

/// 格式化输入（len 字节 UTF-8），写入 out（out_cap 字节），返回写入字节数。
#[no_mangle]
pub extern "C" fn format(src_ptr: *const u8, src_len: usize, out_ptr: *mut u8, out_cap: usize) -> usize {
    if src_ptr.is_null() || src_len == 0 || out_ptr.is_null() || out_cap == 0 {
        return 0;
    }
    let src = unsafe { slice::from_raw_parts(src_ptr, src_len) };
    let Some(text) = std::str::from_utf8(src).ok() else { return 0 };
    let out = fmt(text);
    let bytes = out.as_bytes();
    if bytes.len() > out_cap {
        return 0;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_ptr, bytes.len());
    }
    bytes.len()
}

/// 预计算格式化后长度。
#[no_mangle]
pub extern "C" fn format_len(src_ptr: *const u8, src_len: usize) -> usize {
    if src_ptr.is_null() || src_len == 0 {
        return 0;
    }
    let src = unsafe { slice::from_raw_parts(src_ptr, src_len) };
    let Some(text) = std::str::from_utf8(src).ok() else { return 0 };
    fmt(text).len()
}

use bio_syntax::lexer::{Token, TokenKind};

const CONTROL_KW: &[&str] = &["if", "while", "for", "else"];
const SPACE_OP: &[&str] = &[
    "+", "-", "*", "/", "%", "=", "==", "!=", "<", ">", "<=", ">=",
    "&&", "||", "+=", "-=", "*=", "/=", "%=",
];

fn is_ident_like(t: &Token) -> bool {
    matches!(t.kind, TokenKind::Ident | TokenKind::Keyword)
}

/// Rust 格式化器主体。
pub fn fmt(src: &str) -> String {
    let mut toks = Vec::new();
    if bio_syntax::lexer::tokenize(src, &mut toks).is_err() {
        return src.to_string(); // 词法错误：原样返回
    }
    let mut out = String::new();
    let mut indent: usize = 0;
    let mut prev: Option<Token> = None;
    let mut line_start = true;
    let mut paren_depth: usize = 0;

    for t in toks.iter() {
        if t.kind == TokenKind::Eof {
            break;
        }
        // 行首缩进（; 和 , 不缩进；} 用 indent-1）
        if line_start {
            if t.text != ";" && t.text != "," {
                let n = if t.text == "}" { indent.saturating_sub(1) } else { indent };
                for _ in 0..n * 4 {
                    out.push(' ');
                }
            }
            line_start = false;
        }

        let text = t.text;
        let is_block_open = text == "{";
        let is_block_close = text == "}";
        let is_semi = text == ";";
        let is_comma = text == ",";
        let is_paren_open = text == "(";
        let is_paren_close = text == ")";
        if is_paren_open {
            paren_depth += 1;
        } else if is_paren_close && paren_depth > 0 {
            paren_depth -= 1;
        }
        let is_colon2 = text == "::";
        let is_dot = text == ".";
        let is_comment = text.starts_with("//") || text.starts_with("/*");

        // 前置空格决策
        let space_before = if let Some(p) = &prev {
            if line_start {
                false
            } else if p.text == "{" || p.text == ";" || p.text == "," || p.text == "(" || is_colon2 || is_dot {
                false
            } else if is_block_close {
                // } 前无空格（但 `} else {` 由 else 关键字前补空格处理）
                false
            } else if is_semi || is_comma || is_paren_close {
                false
            } else if is_paren_open {
                // 控制关键字后空格；调用名后无
                is_ident_like(p) && CONTROL_KW.contains(&p.text)
            } else if is_comment {
                true
            } else if is_block_open {
                // { 前空格：Main {、while (...) {、} else {
                true
            } else if is_ident_like(t) && is_ident_like(p) {
                // 标识符/关键字相邻 → 空格（program main、void exec）
                true
            } else if is_ident_like(t) || is_ident_like(p) {
                // 标识符与运算符/括号之间：看运算符
                SPACE_OP.contains(&text) || SPACE_OP.contains(&p.text) || text == "=" || p.text == "="
            } else {
                // 运算符之间
                SPACE_OP.contains(&text) || SPACE_OP.contains(&p.text)
            }
        } else {
            false
        };
        if space_before && !out.ends_with(' ') && !out.ends_with('\n') {
            out.push(' ');
        }
        // ) 前无空格（分号后 push 的空格在此清理）
        if is_paren_close && out.ends_with(' ') {
            out.pop();
        }

        // 输出 token 文本（字符串/字符重新加引号）
        match t.kind {
            TokenKind::Str => {
                out.push('"');
                out.push_str(text);
                out.push('"');
            }
            TokenKind::Char => {
                out.push('\'');
                out.push_str(text);
                out.push('\'');
            }
            _ => out.push_str(text),
        }

        // 后置处理
        if is_block_open {
            out.push('\n');
            indent += 1;
            line_start = true;
        } else if is_block_close {
            if indent > 0 {
                indent -= 1;
            }
            out.push('\n');
            line_start = true;
        } else if is_semi {
            if paren_depth == 0 {
                out.push('\n');
                line_start = true;
            } else {
                out.push(' '); // for 头内分号不换行
            }
        } else if is_comma {
            out.push(' ');
        } else if is_comment && text.starts_with("//") {
            out.push('\n');
            line_start = true;
        }
        prev = Some(*t);
    }

    // 清理：空行压缩 + 行尾空格
    let mut cleaned = String::new();
    let mut nl = 0usize;
    for c in out.chars() {
        if c == '\n' {
            nl += 1;
            if nl <= 2 {
                cleaned.push('\n');
            }
        } else {
            nl = 0;
            cleaned.push(c);
        }
    }
    let lines: Vec<&str> = cleaned.split('\n').collect();
    let trimmed: Vec<String> = lines.iter().map(|l| l.trim_end().to_string()).collect();
    let mut result = trimmed.join("\n");
    while result.ends_with('\n') {
        result.pop();
    }
    result.push('\n');
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fmt2(s: &str) -> String {
        let a = fmt(s);
        let b = fmt(&a);
        assert_eq!(a, b, "格式化不幂等:\n---1---\n{a}\n---2---\n{b}");
        a
    }

    #[test]
    fn hello() {
        let src = r#"program main;
Main { void exec() { CIO::println("Hello"); } }"#;
        let out = fmt2(src);
        assert!(
            out.contains("program main;\nMain {\n    void exec() {\n        CIO::println(\"Hello\");\n    }\n}"),
            "{out}"
        );
    }

    #[test]
    fn control_flow() {
        let src = "program main;\nMain {\nvoid exec() {\nALL i=1;\nwhile(i<=10){i=i+1;}\nfor(ALL k=1;k<=5;k=k+1;){}\n}\n}";
        let out = fmt2(src);
        assert!(out.contains("ALL i = 1;"), "{out}");
        assert!(out.contains("while (i <= 10) {"), "{out}");
        assert!(out.contains("for (ALL k = 1; k <= 5; k = k + 1;) {"), "{out}");
        assert!(out.contains("    void exec() {"), "{out}");
    }

    #[test]
    fn strings_untouched() {
        let src = r#"CIO::println("a + b  keep  spaces");"#;
        let out = fmt2(src);
        assert!(out.contains("\"a + b  keep  spaces\""), "{out}");
    }

    #[test]
    fn idempotent_on_examples() {
        let ex = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent().unwrap().parent().unwrap().parent().unwrap().join("examples");
        let mut n = 0;
        if let Ok(entries) = std::fs::read_dir(&ex) {
            for e in entries.flatten() {
                let p = e.path();
                if p.extension().map(|x| x == "bio").unwrap_or(false) {
                    let src = std::fs::read_to_string(&p).unwrap();
                    let a = fmt(&src);
                    let b = fmt(&a);
                    assert_eq!(a, b, "not idempotent: {}", p.display());
                    n += 1;
                }
            }
        }
        assert!(n >= 10, "examples 太少: {n}");
    }
}
