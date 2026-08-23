//! bio-rs — BioLang Rust 实现 CLI（M3：解释器可用）。
//!
//! 里程碑（见仓库根 RUST-PLAN.md）：
//! - M1 ✅ 手写词法器 + 内存核心（arena/value）
//! - M2 ✅ 手写 AST + 完整解析器（examples/01-17 全量 parse 通过）
//! - M3 🔨 解释器 + 内置流（Unistream/CIO/SIO/FIO/Com/Time/Obj/Solid/Array…）
//! - M4 ⏳ LLVM IR 文本后端（对标旧 src/llvm.c，零依赖）
//! - M5 ⏳ examples/01-17 全量回归

use std::io::Read;
use std::path::PathBuf;
use std::process::ExitCode;

use bio_core::arena::{BumpArena, StrArena};
use bio_core::value::Value;
use bio_syntax::lexer::{self, TokenKind};
use bio_syntax::parser::parse_source;
use bio_vm::interp::Interp;

const USAGE: &str = "\
bio-rs — BioLang Rust 实现（M3：词法 + 解析 + 解释）

用法：
  bio-rs lexer <file>    打印 Token 流（词法器验证）
  bio-rs parse <file>    解析并打印 AST 摘要（解析器验证）
  bio-rs run <file|dir>  解释运行（目录=项目：src/ + utils/）
  bio-rs llvm <file>     LLVM 编译成原生可执行并运行（M4）
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

fn cmd_parse(path: &str) -> Result<(), String> {
    let mut buf = Vec::new();
    std::fs::File::open(path)
        .map_err(|e| format!("无法打开 {path}: {e}"))?
        .read_to_end(&mut buf)
        .map_err(|e| format!("读取失败: {e}"))?;
    let src = String::from_utf8(buf).map_err(|_| "源文件不是 UTF-8".to_string())?;
    let (prog, errs) = parse_source(&src);
    if !errs.is_empty() {
        for e in &errs {
            eprintln!("解析错误: {e}");
        }
        return Err(format!("{} 个解析错误", errs.len()));
    }
    println!("kind: {}", if prog.kind.is_empty() { "<none>" } else { &prog.kind });
    println!("decls: {} | main: {} | methods: {}",
             prog.decls.len(),
             if prog.main.is_some() { "yes" } else { "no" },
             prog.main.as_ref().map(|m| m.methods.len()).unwrap_or(0));
    for d in &prog.decls {
        let name = match d {
            bio_syntax::Decl::Const { name, .. } => format!("const {name}"),
            bio_syntax::Decl::Need { kind, name } => format!("need {kind} {name}"),
            bio_syntax::Decl::StreamSig { name, members, .. } =>
                format!("stream {name} ({} members)", members.len()),
            bio_syntax::Decl::StreamBin { name, file, .. } =>
                format!("bin-stream {name} <- {file}"),
            bio_syntax::Decl::Class { name, members, .. } =>
                format!("class {name} ({} members)", members.len()),
            bio_syntax::Decl::Fork { sig, name, members, .. } =>
                format!("fork {sig} {name} ({} members)", members.len()),
        };
        println!("  {name}");
    }
    Ok(())
}

fn cmd_run(arg: &str) -> Result<(), String> {
    let path = PathBuf::from(arg);
    let mut interp = Interp::new();
    let (prog, unmet) = if path.is_dir() {
        let prog = bio_vm::load_project_sources(&path)?;
        let unmet = interp.reg.register(&prog);
        (prog, unmet)
    } else {
        let mut buf = Vec::new();
        std::fs::File::open(&path)
            .map_err(|e| format!("无法打开 {}: {e}", path.display()))?
            .read_to_end(&mut buf)
            .map_err(|e| format!("读取失败: {e}"))?;
        let src = String::from_utf8(buf).map_err(|_| "源文件不是 UTF-8".to_string())?;
        let (prog, errs) = parse_source(&src);
        if !errs.is_empty() {
            return Err(errs.iter().map(|e| e.to_string()).collect::<Vec<_>>().join("\n"));
        }
        let unmet = interp.reg.register(&prog);
        (prog, unmet)
    };
    // need 校验（const 提供检查）
    let unmet = unmet
        .into_iter()
        .filter(|(k, n)| !(k == "value" && prog.decls.iter().any(|d| matches!(d, bio_syntax::Decl::Const { name, .. } if name == n))))
        .collect::<Vec<_>>();
    if !unmet.is_empty() {
        return Err(unmet
            .iter()
            .map(|(k, n)| format!("need {k} {n} 没有 provider"))
            .collect::<Vec<_>>()
            .join("\n"));
    }
    let out = interp.run(&prog);
    print!("{}", out.stdout);
    Ok(())
}

fn cmd_llvm(arg: &str) -> Result<(), String> {
    let path = std::path::PathBuf::from(arg);
    let mut buf = Vec::new();
    std::fs::File::open(&path)
        .map_err(|e| format!("无法打开 {}: {e}", path.display()))?
        .read_to_end(&mut buf)
        .map_err(|e| format!("读取失败: {e}"))?;
    let src = String::from_utf8(buf).map_err(|_| "源文件不是 UTF-8".to_string())?;
    let (prog, errs) = parse_source(&src);
    if !errs.is_empty() {
        return Err(errs.iter().map(|e| e.to_string()).collect::<Vec<_>>().join("\n"));
    }
    // 1. AST → IR
    let ir = bio_llvm::compile(&prog).map_err(|e| format!("IR 生成失败: {e}"))?;
    // 2. IR → clang → 可执行
    let dir = std::env::temp_dir().join(format!("bio-rs-llvm-{}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let ir_path = dir.join("out.ll");
    let bin_path = dir.join("a.out");
    std::fs::write(&ir_path, &ir).map_err(|e| e.to_string())?;
    let status = std::process::Command::new("clang")
        .arg(&ir_path)
        .arg("-o")
        .arg(&bin_path)
        .status()
        .map_err(|e| format!("clang 调用失败: {e}"))?;
    if !status.success() {
        return Err(format!("clang 编译失败（IR 已存 {})", ir_path.display()));
    }
    // 3. 运行
    let status = std::process::Command::new(&bin_path).status().map_err(|e| e.to_string())?;
    if !status.success() {
        return Err(format!("运行失败，退出码 {:?}", status.code()));
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
        Some("parse") => match args.get(2) {
            Some(path) => match cmd_parse(path) {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("{e}");
                    ExitCode::FAILURE
                }
            },
            None => {
                eprintln!("用法：bio-rs parse <file>");
                ExitCode::FAILURE
            }
        },
        Some("run") => match args.get(2) {
            Some(target) => match cmd_run(target) {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("{e}");
                    ExitCode::FAILURE
                }
            },
            None => {
                eprintln!("用法：bio-rs run <file|dir>");
                ExitCode::FAILURE
            }
        },
        Some("llvm") => match args.get(2) {
            Some(target) => match cmd_llvm(target) {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("{e}");
                    ExitCode::FAILURE
                }
            },
            None => {
                eprintln!("用法：bio-rs llvm <file>");
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
