//! bio-rs — BioLang Rust 实现 CLI（M1 里程碑：词法层可用）。
//!
//! 里程碑（见仓库根 RUST-PLAN.md）：
//! - M1 ✅ 生成 AST 骨架 + 手写词法器 + 内存核心（arena/value）
//! - M2 ⏳ 手写解析器 → AST（MPS 生成结构）
//! - M3 ⏳ 解释器 + 内置流（Unistream/CIO/FIO/Array/Threads/Taskm...）
//! - M4 ⏳ LLVM IR 文本后端（对标旧 src/llvm.c，零依赖）
//! - M5 ⏳ examples/01-17 全量回归

use std::io::Read;
use std::process::ExitCode;

use bio_core::arena::{BumpArena, StrArena};
use bio_core::value::Value;
use bio_syntax::lexer::{self, TokenKind};

const USAGE: &str = "\
bio-rs — BioLang Rust 实现（M1：词法 + 内存核心）

用法：
  bio-rs lexer <file>    打印 Token 流（词法器验证）
  bio-rs arena <n>       arena/字符串池压力测试（分配 n 个值）
  bio-rs --version       版本号
";

fn cmd_lexer(path: &str) -> Result<(), String> {
    let mut buf = Vec::new();
    std::fs::File::open(path)
        .map_err(|e| format!("无法打开 {path}: {e}"))?
        .read_to_end(&mut buf)
        .map_err(|e| format!("读取失败: {e}"))?;
    let src = String::from_utf8(buf).map_err(|_| "源文件不是 UTF-8".to_string())?;
    let mut toks = Vec::new();
    lexer::tokenize(&src, &mut toks).map_err(|e| format!("词法错误: {e}"))?;
    for t in &toks {
        let kind = match t.kind {
            TokenKind::Ident => "ident",
            TokenKind::Keyword => "keyword",
            TokenKind::Int => "int",
            TokenKind::Float => "float",
            TokenKind::Str => "string",
            TokenKind::Char => "char",
            TokenKind::Op => "op",
            TokenKind::Eof => "eof",
        };
        println!("{:>5}:{:<3} {:<8} {:?}", t.span.line, t.span.col, kind, t.text);
    }
    Ok(())
}

fn cmd_arena(n: u32) {
    // 压力测试：句柄不因扩容失效；字符串池跨页正确。
    let mut arena = BumpArena::<u64>::new();
    let mut last = 0;
    for i in 1..=n {
        last = arena.alloc(i as u64);
    }
    assert_eq!(*arena.get(last), n as u64);
    println!("BumpArena<u64>: {} 槽, {} 页, 句柄 {} 有效", n, arena.pages(), last);

    let mut strs = StrArena::new();
    let mut handle = None;
    for i in 0..n {
        let s = format!("value-{i}-{}", "x".repeat((i % 97) as usize));
        handle = Some(strs.push(&s));
    }
    let h = handle.unwrap();
    assert!(strs.get(h).starts_with("value-"));
    println!("StrArena: {} 次写入, 末串长度 {}（跨页完整）", n, strs.get(h).len());

    let v = Value::int(42).with_refused();
    println!("Value: size={}B, refused int 显示为 {v}", std::mem::size_of::<Value>());
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(|s| s.as_str()) {
        Some("lexer") => match args.get(2) {
            Some(path) => match cmd_lexer(path) {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("{e}");
                    ExitCode::FAILURE
                }
            },
            None => {
                eprintln!("用法：bio-rs lexer <file>");
                ExitCode::FAILURE
            }
        },
        Some("arena") => {
            let n: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(10_000);
            cmd_arena(n);
            ExitCode::SUCCESS
        }
        Some("--version" | "-V") => {
            println!("bio-rs {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        _ => {
            print!("{USAGE}");
            ExitCode::SUCCESS
        }
    }
}
