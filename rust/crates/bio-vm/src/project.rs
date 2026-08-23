//! 项目加载：目录模式（package.toml + src/ + utils/ 合并解析）。

use std::path::PathBuf;

use bio_syntax::ast::{Decl, Program};
use bio_syntax::parser::parse_source;

/// 加载项目所有 .bio 文件并合并成一个 Program。
/// need 跨文件配对由 registry.register 统一校验。
pub fn load_project_sources(root: &PathBuf) -> Result<Program, String> {
    let mut files: Vec<PathBuf> = Vec::new();
    for base in ["src", "utils"] {
        let d = root.join(base);
        if !d.is_dir() {
            continue;
        }
        if let Ok(entries) = std::fs::read_dir(&d) {
            for e in entries.flatten() {
                let p = e.path();
                if p.extension().map(|x| x == "bio" || x == "bl").unwrap_or(false) {
                    files.push(p);
                }
            }
        }
    }
    files.sort();
    if files.is_empty() {
        return Err(format!("{}: 项目里没有 src/ 或 utils/ 下的 .bio 文件", root.display()));
    }

    let mut decls: Vec<Decl> = Vec::new();
    let mut main = None;
    let mut kind = String::new();
    for f in &files {
        let src = std::fs::read_to_string(f)
            .map_err(|e| format!("{}: 读取失败: {e}", f.display()))?;
        let (prog, errs) = parse_source(&src);
        if !errs.is_empty() {
            return Err(format!(
                "{}: {}",
                f.display(),
                errs.iter().map(|e| e.to_string()).collect::<Vec<_>>().join("; ")
            ));
        }
        if !prog.kind.is_empty() {
            kind = prog.kind.clone();
        }
        if prog.main.is_some() {
            if main.is_some() {
                return Err(format!("{}: 项目里有多个 Main 流定义", f.display()));
            }
            main = prog.main;
        }
        decls.extend(prog.decls);
    }
    Ok(Program { kind, decls, main })
}
