//! 标准层回归：examples/01-17 + project 全部必须 parse 成功。
//!
//! 这是"标准层不改变"的硬验证：任何解析器改动导致 examples 解析失败
//! 即回归失败。examples 目录位于仓库根（相对本 crate 的 ../examples）。

use std::path::PathBuf;

use bbb_syntax::parser::parse_source;

fn examples_dir() -> PathBuf {
    // CARGO_MANIFEST_DIR = rust/crates/bbb-syntax → 上溯 3 级到仓库根
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("examples")
}

fn collect_bio_files() -> Vec<PathBuf> {
    let mut out = Vec::new();
    let ex = examples_dir();
    let mut dirs = vec![ex.clone()];
    dirs.push(ex.join("project").join("src"));
    dirs.push(ex.join("project").join("utils"));
    for d in dirs {
        if let Ok(entries) = std::fs::read_dir(&d) {
            for e in entries.flatten() {
                let p = e.path();
                if p.extension().map(|x| x == "bio").unwrap_or(false) {
                    out.push(p);
                }
            }
        }
    }
    out.sort();
    out
}

#[test]
fn all_examples_parse() {
    let files = collect_bio_files();
    assert!(!files.is_empty(), "examples 目录为空？");
    let mut failed = Vec::new();
    for f in &files {
        let src = std::fs::read_to_string(f).expect("read");
        let (prog, errs) = parse_source(&src);
        if !errs.is_empty() {
            failed.push((f.display().to_string(), errs.clone()));
        }
        // 主程序必须声明了 kind
        if prog.kind.is_empty() && !errs.is_empty() {
            failed.push((f.display().to_string(), errs));
        }
    }
    assert!(failed.is_empty(), "解析失败：\n{}",
            failed.iter().map(|(f, es)| format!(
                "{f}: {}", es.iter().map(|e| e.to_string()).collect::<Vec<_>>().join("; ")))
                .collect::<Vec<_>>().join("\n"));
}

/// 每个示例必须声明 program main/utils（标准层契约）。
#[test]
fn examples_declare_program_kind() {
    let files = collect_bio_files();
    let mut bad = Vec::new();
    for f in &files {
        let src = std::fs::read_to_string(f).expect("read");
        let (prog, errs) = parse_source(&src);
        if errs.is_empty() && prog.kind.is_empty() {
            bad.push(f.display().to_string());
        }
    }
    assert!(bad.is_empty(), "缺少 program 声明：{bad:?}");
}
