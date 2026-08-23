//! bbb — BiuBiuBiu 语言 CLI（Rust + LLVM 实现）。
//!
//! 标准 CLI（README 定义，与旧 C 实现一致）：
//!   bbb <file>                    解释运行脚本
//!   bbb shell run <file>          解释运行脚本（显式）
//!   bbb shell build <file> [-o out]  编译脚本 → 原生可执行
//!   bbb init <name>               创建项目骨架（package.toml + src/ + utils/）
//!   bbb build [dir] [-s|-m] [-o out]  构建项目
//!   bbb run [dir]                 运行项目（解释）
//!   bbb install [dir]             安装依赖
//!   bbb destroy [dir]             清理构建产物
//!   bbb pack <out.img|.zip> [--entry NAME] <files...>
//!   bbb unpack <pkg> [dir]
//!   bbb --tokens <file>           dump tokens（调试）
//!   bbb -e <bytes[K|M|G]>         解释器内存限制（0 = unlimited）
//!   bbb -h | --help               帮助

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use bbb_core::arena::{BumpArena, StrArena};
use bbb_core::value::Value;
use bbb_syntax::lexer::{self, TokenKind};
use bbb_syntax::parser::parse_source;
use bbb_vm::interp::Interp;

const USAGE: &str = "\
🧬 BiuBiuBiu (bbb) — interpret or compile .bio/.bl programs

Usage:
  bbb <file>                    run (interpret) a script
  bbb shell run <file>          run a script (interpret)
  bbb shell build <file> [-o out]  compile a script → standalone executable
  bbb -e <bytes[K|M|G]>         set interpreter memory limit (0 = unlimited, default 256M)
  bbb --tokens <file>           dump tokens (debug)
  bbb init <name>               create a project skeleton (src/ utils/ package.toml)
  bbb build [dir] [-o out]      build a project (bundle needs, compile)
  bbb build [dir] -s            build → standalone executable (default)
  bbb build [dir] -m [out.img|out.zip]  build → .img/.zip package
  bbb run [dir]                 run a project (bundle needs, interpret)
  bbb install [dir]             install deps from package.toml
  bbb destroy [dir]             remove build artifacts
  bbb pack <out.img|.zip> [--entry NAME] <files...>
                                package compiled products (raw .img / .zip)
  bbb unpack <pkg> [dir]        unpack a .img / .zip package
  bbb                           run built-in demos
  bbb -h | --help               show this help

  env: BIOLANG_CONFIG → global config file (TOML, may contain repo=)
";

fn read_source(path: &str) -> Result<String, String> {
    let mut buf = Vec::new();
    std::fs::File::open(path)
        .map_err(|e| format!("cannot open file: {path} ({e})"))?
        .read_to_end(&mut buf)
        .map_err(|e| format!("read failed: {e}"))?;
    String::from_utf8(buf).map_err(|_| "source is not UTF-8".to_string())
}

fn parse_or_err(src: &str) -> Result<bbb_syntax::ast::Program, String> {
    let (prog, errs) = parse_source(src);
    if !errs.is_empty() {
        return Err(errs.iter().map(|e| e.to_string()).collect::<Vec<_>>().join("\n"));
    }
    Ok(prog)
}

/// 运行单个脚本文件（解释）。
fn run_script_file(path: &str) -> Result<(), String> {
    let src = read_source(path)?;
    let prog = parse_or_err(&src)?;
    let mut interp = Interp::new();
    let unmet = interp.reg.register(&prog);
    let unmet = filter_value_needs(unmet, &prog);
    if !unmet.is_empty() {
        return Err(unmet
            .iter()
            .map(|(k, n)| format!("need {k} {n} has no provider"))
            .collect::<Vec<_>>()
            .join("\n"));
    }
    let out = interp.run(&prog);
    print!("{}", out.stdout);
    Ok(())
}

/// 运行项目目录（解释）：合并 src/ + utils/。
fn run_project_dir(root: &Path) -> Result<(), String> {
    let prog = bbb_vm::load_project_sources(&root.to_path_buf())?;
    let mut interp = Interp::new();
    let unmet = interp.reg.register(&prog);
    let unmet = filter_value_needs(unmet, &prog);
    if !unmet.is_empty() {
        return Err(unmet
            .iter()
            .map(|(k, n)| format!("need {k} {n} has no provider"))
            .collect::<Vec<_>>()
            .join("\n"));
    }
    let out = interp.run(&prog);
    print!("{}", out.stdout);
    Ok(())
}

fn filter_value_needs(
    unmet: Vec<(String, String)>,
    prog: &bbb_syntax::ast::Program,
) -> Vec<(String, String)> {
    unmet
        .into_iter()
        .filter(|(k, n)| {
            !(k == "value"
                && prog
                    .decls
                    .iter()
                    .any(|d| matches!(d, bbb_syntax::Decl::Const { name, .. } if name == n)))
        })
        .collect()
}

/// LLVM 编译：AST → IR → clang → 可执行文件（不运行）。
fn compile_to_executable(prog: &bbb_syntax::ast::Program, out: &str) -> Result<(), String> {
    let ir = bbb_llvm::compile(prog).map_err(|e| format!("IR generation failed: {e}"))?;
    let dir = std::env::temp_dir().join(format!("bbb-build-{}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let ir_path = dir.join("out.ll");
    std::fs::write(&ir_path, &ir).map_err(|e| e.to_string())?;
    // 确保输出目录存在
    if let Some(parent) = Path::new(out).parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
        }
    }
    let status = std::process::Command::new("clang")
        .arg(&ir_path)
        .arg("-o")
        .arg(out)
        .status()
        .map_err(|e| format!("clang invocation failed: {e}"))?;
    if !status.success() {
        return Err(format!("clang failed (IR kept at {})", ir_path.display()));
    }
    Ok(())
}

// ───────────────────────── 命令实现 ─────────────────────────

fn cmd_tokens(path: &str) -> Result<(), String> {
    let src = read_source(path)?;
    let mut toks = Vec::new();
    lexer::tokenize(&src, &mut toks).map_err(|e| format!("lex error: {e}"))?;
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

fn cmd_shell_run(file: &str) -> Result<(), String> {
    run_script_file(file)
}

fn cmd_shell_build(file: &str, out: Option<&str>) -> Result<(), String> {
    let src = read_source(file)?;
    let prog = parse_or_err(&src)?;
    // 默认输出：bin/<basename 去扩展名>
    let out_path = match out {
        Some(o) => o.to_string(),
        None => {
            let base = Path::new(file)
                .file_name()
                .map(|s| s.to_string_lossy().to_string())
                .unwrap_or_else(|| "a.out".to_string());
            let base = base
                .strip_suffix(".bio")
                .or_else(|| base.strip_suffix(".bl"))
                .unwrap_or(&base)
                .to_string();
            format!("bin/{base}")
        }
    };
    println!("compiling {file} → {out_path}");
    compile_to_executable(&prog, &out_path)?;
    println!("✔ compiled: {out_path}");
    Ok(())
}

fn cmd_run(target: &str) -> Result<(), String> {
    let path = PathBuf::from(target);
    if path.is_dir() {
        run_project_dir(&path)
    } else {
        run_script_file(target)
    }
}

fn cmd_init(name: &str) -> Result<(), String> {
    let root = PathBuf::from(name);
    if root.exists() {
        return Err(format!("{name}: already exists"));
    }
    std::fs::create_dir_all(root.join("src")).map_err(|e| e.to_string())?;
    std::fs::create_dir_all(root.join("utils")).map_err(|e| e.to_string())?;
    let toml = format!(
        "# {name} — BiuBiuBiu project manifest\n\
         # standard fields: name / version / repo (optional) + [dependencies]\n\
         name = \"{name}\"\n\
         version = \"0.1.0\"\n\
         \n\
         [dependencies]\n\
         # libfoo = {{ version = \"1.0.0\" }}\n\
         # libbar = {{ version = \"0.2.0\", repo = \"https://...\" }}\n"
    );
    std::fs::write(root.join("package.toml"), toml).map_err(|e| e.to_string())?;
    let main = format!(
        "program main;\n\
         \n\
         Main {{\n\
             void exec() {{\n\
                 CIO::println(\"Hello from {name}!\");\n\
             }}\n\
         }}\n"
    );
    std::fs::write(root.join("src").join("main.bio"), main).map_err(|e| e.to_string())?;
    println!("✔ project created: {name}");
    println!("  package.toml  — manifest (name/version/repo + deps)");
    println!("  src/main.bio  — entry");
    println!("  utils/        — libraries (need providers)");
    Ok(())
}

/// 项目构建：need bundling（load_project_sources 合并）+ LLVM 编译 → 可执行。
/// -s standalone（默认）；-m 打包 .img/.zip（v1 简化：先做 standalone 产物，包格式后续）。
fn cmd_build(dir: &str, _mode: &str, out: Option<&str>) -> Result<(), String> {
    let root = PathBuf::from(dir);
    let prog = bbb_vm::load_project_sources(&root)?;
    // 项目名：package.toml 的 name 字段（简单解析），默认目录名
    let pname = parse_package_name(&root).unwrap_or_else(|| {
        root.file_name()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| "app".to_string())
    });
    let out_path = match out {
        Some(o) => o.to_string(),
        None => {
            let dir_trim = dir.trim_end_matches('/');
            if dir_trim.is_empty() || dir_trim == "." {
                format!("bin/{pname}")
            } else {
                format!("{dir_trim}/bin/{pname}")
            }
        }
    };
    println!("building {dir} → {out_path}");
    compile_to_executable(&prog, &out_path)?;
    println!("✔ build ok: {out_path}");
    Ok(())
}

/// 简单解析 package.toml 的 name = "..."。
fn parse_package_name(root: &Path) -> Option<String> {
    let toml = std::fs::read_to_string(root.join("package.toml")).ok()?;
    for line in toml.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("name") {
            let rest = rest.trim_start();
            if let Some(rest) = rest.strip_prefix('=') {
                let rest = rest.trim().trim_matches('"');
                if !rest.is_empty() {
                    return Some(rest.to_string());
                }
            }
        }
    }
    None
}

fn cmd_install(dir: &str) -> Result<(), String> {
    let root = PathBuf::from(dir);
    let toml_path = root.join("package.toml");
    let toml = match std::fs::read_to_string(&toml_path) {
        Ok(t) => t,
        Err(_) => return Err(format!("no package.toml in {dir}")),
    };
    // 解析 [dependencies] 下的 name = { version, repo } 或 name = "version"
    let mut found = false;
    let mut installed = 0;
    let mut in_deps = false;
    for line in toml.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.starts_with('[') {
            in_deps = line.starts_with("[dependencies]");
            continue;
        }
        if !in_deps {
            continue;
        }
        let Some(eq) = line.find('=') else { continue };
        let dep = line[..eq].trim().to_string();
        let spec = line[eq + 1..].trim();
        if dep.is_empty() {
            continue;
        }
        found = true;
        // repo：spec 里的 repo = "..."；否则从 BIOLANG_CONFIG 全局配置读
        let repo = extract_repo(spec).or_else(global_repo);
        match repo {
            Some(r) => {
                let dest = root.join(".biolang").join("deps").join(&dep);
                let rc = fetch_dep(&r, &dest);
                if rc == 0 {
                    println!("✔ installed {dep}");
                    installed += 1;
                } else {
                    eprintln!("⛔ dep {dep}: fetch failed from {r}");
                }
            }
            None => {
                eprintln!("⛔ dep {dep}: no repo (set repo= or BIOLANG_CONFIG global repo)");
            }
        }
    }
    if !found {
        println!("ℹ️ no dependencies in {}", toml_path.display());
    }
    if installed > 0 {
        println!("✔ {installed} dependency(ies) installed → {}/.biolang/deps", root.display());
    }
    Ok(())
}

fn extract_repo(spec: &str) -> Option<String> {
    // 形如 { version = "1.0.0", repo = "https://..." } 或 "1.0.0"
    if let Some(inner) = spec.strip_prefix('{') {
        let inner = inner.trim_end_matches('}');
        for part in inner.split(',') {
            let part = part.trim();
            if let Some(rest) = part.strip_prefix("repo") {
                let rest = rest.trim_start();
                if let Some(rest) = rest.strip_prefix('=') {
                    return Some(rest.trim().trim_matches('"').to_string());
                }
            }
        }
        None
    } else {
        None
    }
}

fn global_repo() -> Option<String> {
    let path = std::env::var("BIOLANG_CONFIG")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
            PathBuf::from(home).join(".biolang").join("config.toml")
        });
    let text = std::fs::read_to_string(path).ok()?;
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("repo") {
            let rest = rest.trim_start();
            if let Some(rest) = rest.strip_prefix('=') {
                return Some(rest.trim().trim_matches('"').to_string());
            }
        }
    }
    None
}

/// 拉取依赖：git 仓库 → git clone；http → curl 下载 package.toml；本地路径 → 复制。
fn fetch_dep(repo: &str, dest: &Path) -> i32 {
    if let Some(parent) = dest.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if repo.starts_with("http") && repo.contains(".git") {
        let _ = std::fs::remove_dir_all(dest);
        std::process::Command::new("git")
            .args(["clone", "--depth", "1", repo])
            .arg(dest)
            .status()
            .map(|s| if s.success() { 0 } else { 1 })
            .unwrap_or(1)
    } else if repo.starts_with("http") {
        let _ = std::fs::create_dir_all(dest);
        let outfile = dest.join("package.toml");
        std::process::Command::new("curl")
            .args(["-fsSL", repo, "-o"])
            .arg(&outfile)
            .status()
            .map(|s| if s.success() { 0 } else { 1 })
            .unwrap_or(1)
    } else {
        copy_tree(Path::new(repo), dest)
    }
}

fn copy_tree(src: &Path, dest: &Path) -> i32 {
    if !src.is_dir() {
        return 1;
    }
    let _ = std::fs::remove_dir_all(dest);
    fn rec(src: &Path, dest: &Path) -> std::io::Result<()> {
        std::fs::create_dir_all(dest)?;
        for entry in std::fs::read_dir(src)? {
            let entry = entry?;
            let from = entry.path();
            let to = dest.join(entry.file_name());
            if from.is_dir() {
                rec(&from, &to)?;
            } else {
                std::fs::copy(&from, &to)?;
            }
        }
        Ok(())
    }
    match rec(src, dest) {
        Ok(()) => 0,
        Err(_) => 1,
    }
}

fn cmd_destroy(dir: &str) -> Result<(), String> {
    let root = PathBuf::from(dir);
    let biolang = root.join(".biolang");
    if biolang.exists() {
        std::fs::remove_dir_all(&biolang).map_err(|e| e.to_string())?;
    }
    let app = root.join("app");
    if app.exists() {
        let _ = std::fs::remove_dir_all(&app);
    }
    let bin_cache = root.join("bin").join(".cache");
    if bin_cache.exists() {
        let _ = std::fs::remove_dir_all(&bin_cache);
    }
    println!("✔ destroyed build artifacts: {}/.biolang, {}/app", dir, dir);
    Ok(())
}

// ── .img / .zip 打包（v1：基于系统 zip/unzip 的 .zip + 原始 .img） ──

const IMG_MAGIC: &[u8; 7] = b"BIOIMG1";
const IMG_VERSION: u32 = 2;

fn cmd_pack(out: &str, entry: Option<&str>, files: &[String]) -> Result<(), String> {
    if files.is_empty() {
        return Err("pack: no files".to_string());
    }
    if out.ends_with(".img") {
        img_create(out, entry, files)
    } else {
        zip_create(out, files)
    }
}

fn zip_create(out: &str, files: &[String]) -> Result<(), String> {
    // 用系统 zip（STORE 无压缩）；不存在则报错
    let status = std::process::Command::new("zip")
        .arg("-0")
        .arg("-j")
        .arg(out)
        .args(files)
        .status()
        .map_err(|e| format!("zip invocation failed: {e}"))?;
    if !status.success() {
        return Err(format!("pack failed: {out}"));
    }
    println!("packed {} file(s) → {out}", files.len());
    Ok(())
}

fn img_create(out: &str, entry: Option<&str>, files: &[String]) -> Result<(), String> {
    use std::io::Write;
    let mut data: Vec<u8> = Vec::new();
    // header 占位
    let entry_name = entry.unwrap_or(&files[0]);
    data.extend_from_slice(IMG_MAGIC);
    data.extend_from_slice(&IMG_VERSION.to_le_bytes());
    data.extend_from_slice(&0u32.to_le_bytes()); // flags
    data.extend_from_slice(&(entry_name.len() as u32).to_le_bytes());
    data.extend_from_slice(entry_name.as_bytes());
    data.extend_from_slice(&(files.len() as u32).to_le_bytes());
    // 目录记录区：先写 records（offset 暂填 0），再写 payload
    let header_size = 7 + 4 + 4 + 4 + entry_name.len() + 4; // magic(7) + ver + flags + entry_len + name + count
    // 每条记录 = name_len(4) + mode(4) + name(nl) + offset(8) + size(8)
    let mut offset = header_size as u64;
    for f in files {
        offset += (4 + 4 + f.len() + 8 + 8) as u64;
    }
    let mut records: Vec<(String, u32, u64, u64)> = Vec::new();
    for f in files {
        let bytes = std::fs::read(f).map_err(|e| format!("cannot read {f}: {e}"))?;
        let mode = file_mode(f);
        records.push((f.clone(), mode, offset, bytes.len() as u64));
        offset += bytes.len() as u64;
    }
    for (name, mode, off, size) in &records {
        data.extend_from_slice(&(name.len() as u32).to_le_bytes());
        data.extend_from_slice(&mode.to_le_bytes());
        data.extend_from_slice(name.as_bytes());
        data.extend_from_slice(&off.to_le_bytes());
        data.extend_from_slice(&size.to_le_bytes());
    }
    for f in files {
        let bytes = std::fs::read(f).map_err(|e| format!("cannot read {f}: {e}"))?;
        data.extend_from_slice(&bytes);
    }
    let mut f = std::fs::File::create(out).map_err(|e| e.to_string())?;
    f.write_all(&data).map_err(|e| e.to_string())?;
    println!("packed {} file(s) → {out}", files.len());
    Ok(())
}

fn file_mode(path: &str) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path)
        .map(|m| m.permissions().mode() & 0o777)
        .unwrap_or(0o644)
}

fn cmd_unpack(pkg: &str, dir: &str) -> Result<(), String> {
    if pkg.ends_with(".img") {
        img_unpack(pkg, dir)
    } else {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        let status = std::process::Command::new("unzip")
            .arg("-o")
            .arg(pkg)
            .arg("-d")
            .arg(dir)
            .status()
            .map_err(|e| format!("unzip invocation failed: {e}"))?;
        if !status.success() {
            return Err(format!("unpack failed: {pkg}"));
        }
        println!("unpacked {pkg} → {dir}");
        Ok(())
    }
}

fn img_unpack(pkg: &str, dir: &str) -> Result<(), String> {
    let data = std::fs::read(pkg).map_err(|e| format!("cannot read {pkg}: {e}"))?;
    if data.len() < 24 || &data[..7] != IMG_MAGIC {
        return Err(format!("{pkg}: not a BIOIMG1 image"));
    }
    let mut pos = 7usize;
    let _version = rd_u32(&data, &mut pos);
    let _flags = rd_u32(&data, &mut pos);
    let entry_len = rd_u32(&data, &mut pos) as usize;
    pos += entry_len; // entry name
    let count = rd_u32(&data, &mut pos) as usize;
    std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    for _ in 0..count {
        let name_len = rd_u32(&data, &mut pos) as usize;
        let mode = rd_u32(&data, &mut pos);
        let name = String::from_utf8_lossy(&data[pos..pos + name_len]).to_string();
        pos += name_len;
        let off = rd_u64(&data, &mut pos);
        let size = rd_u64(&data, &mut pos);
        let bytes = &data[off as usize..(off + size) as usize];
        let out_path = Path::new(dir).join(Path::new(&name).file_name().unwrap_or_default());
        std::fs::write(&out_path, bytes).map_err(|e| e.to_string())?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&out_path, std::fs::Permissions::from_mode(mode));
        }
    }
    println!("unpacked {pkg} → {dir}");
    Ok(())
}

fn rd_u32(data: &[u8], pos: &mut usize) -> u32 {
    let v = u32::from_le_bytes(data[*pos..*pos + 4].try_into().unwrap());
    *pos += 4;
    v
}

fn rd_u64(data: &[u8], pos: &mut usize) -> u64 {
    let v = u64::from_le_bytes(data[*pos..*pos + 8].try_into().unwrap());
    *pos += 8;
    v
}

// ───────────────────────── 内部调试命令（保留） ─────────────────────────

fn cmd_parse(path: &str) -> Result<(), String> {
    let src = read_source(path)?;
    let (prog, errs) = parse_source(&src);
    if !errs.is_empty() {
        for e in &errs {
            eprintln!("parse error: {e}");
        }
        return Err(format!("{} parse errors", errs.len()));
    }
    println!("kind: {}", if prog.kind.is_empty() { "<none>" } else { &prog.kind });
    println!("decls: {} | main: {} | methods: {}",
             prog.decls.len(),
             if prog.main.is_some() { "yes" } else { "no" },
             prog.main.as_ref().map(|m| m.methods.len()).unwrap_or(0));
    for d in &prog.decls {
        let name = match d {
            bbb_syntax::Decl::Const { name, .. } => format!("const {name}"),
            bbb_syntax::Decl::Need { kind, name } => format!("need {kind} {name}"),
            bbb_syntax::Decl::StreamSig { name, members, .. } =>
                format!("stream {name} ({} members)", members.len()),
            bbb_syntax::Decl::StreamBin { name, file, .. } =>
                format!("bin-stream {name} <- {file}"),
            bbb_syntax::Decl::Class { name, members, .. } =>
                format!("class {name} ({} members)", members.len()),
            bbb_syntax::Decl::Fork { sig, name, members, .. } =>
                format!("fork {sig} {name} ({} members)", members.len()),
        };
        println!("  {name}");
    }
    Ok(())
}

fn cmd_arena(n: u32) {
    let mut arena = BumpArena::<u64>::new();
    let mut last = 0;
    for i in 1..=n {
        last = arena.alloc(i as u64);
    }
    assert_eq!(*arena.get(last), n as u64);
    println!("BumpArena<u64>: {} slots, {} pages, handle {last} valid", n, arena.pages());

    let mut strs = StrArena::new();
    let mut handle = None;
    for i in 0..n {
        let s = format!("value-{i}-{}", "x".repeat((i % 97) as usize));
        handle = Some(strs.push(&s));
    }
    let h = handle.unwrap();
    assert!(strs.get(h).starts_with("value-"));
    println!("StrArena: {} writes, last len {} (cross-page intact)", n, strs.get(h).len());

    let v = Value::int(42).with_refused();
    println!("Value: size={}B, refused int displays as {v}", std::mem::size_of::<Value>());
}

// ───────────────────────── 入口 ─────────────────────────

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let argc = args.len();

    // 无参数：内置演示（v1：打印 usage 提示）
    if argc == 1 {
        print!("{USAGE}");
        return ExitCode::SUCCESS;
    }

    let a1 = args[1].as_str();
    // 全局选项
    if a1 == "-h" || a1 == "--help" {
        print!("{USAGE}");
        return ExitCode::SUCCESS;
    }
    if a1 == "--version" || a1 == "-V" {
        println!("bbb {}", env!("CARGO_PKG_VERSION"));
        return ExitCode::SUCCESS;
    }
    if a1 == "-e" {
        // 内存限制：v1 接受参数（0 = unlimited），解释器当前无硬限制
        if argc < 3 {
            eprintln!("usage: bbb -e <bytes[K|M|G]>");
            return ExitCode::FAILURE;
        }
        let _limit = parse_size(&args[2]);
        // 剩余参数按普通命令处理（-e 仅设置限制）
        let rest: Vec<String> = args[2..].to_vec();
        if rest.is_empty() {
            print!("{USAGE}");
            return ExitCode::SUCCESS;
        }
        return dispatch(&rest[0], &rest[1..]);
    }
    if a1 == "--tokens" {
        if argc < 3 {
            eprintln!("usage: bbb --tokens <file>");
            return ExitCode::FAILURE;
        }
        return finish(cmd_tokens(&args[2]));
    }

    dispatch(a1, &args[2..])
}

fn parse_size(s: &str) -> u64 {
    let (num, mult) = match s.chars().last() {
        Some('K' | 'k') => (&s[..s.len() - 1], 1024u64),
        Some('M' | 'm') => (&s[..s.len() - 1], 1024u64 * 1024),
        Some('G' | 'g') => (&s[..s.len() - 1], 1024u64 * 1024 * 1024),
        _ => (s, 1),
    };
    num.parse::<u64>().unwrap_or(0) * mult
}

fn dispatch(cmd: &str, rest: &[String]) -> ExitCode {
    match cmd {
        "shell" => {
            match rest.first().map(|s| s.as_str()) {
                Some("run") => {
                    if rest.len() < 2 {
                        eprintln!("usage: bbb shell run <file>");
                        return ExitCode::FAILURE;
                    }
                    finish(cmd_shell_run(&rest[1]))
                }
                Some("build") => {
                    if rest.len() < 2 {
                        eprintln!("usage: bbb shell build <file> [-o out]");
                        return ExitCode::FAILURE;
                    }
                    let file = &rest[1];
                    let mut out = None;
                    let mut i = 2;
                    while i < rest.len() {
                        if rest[i] == "-o" && i + 1 < rest.len() {
                            out = Some(rest[i + 1].clone());
                            i += 2;
                        } else {
                            eprintln!("usage: bbb shell build <file> [-o out]");
                            return ExitCode::FAILURE;
                        }
                    }
                    finish(cmd_shell_build(file, out.as_deref()))
                }
                _ => {
                    eprintln!("usage: bbb shell run <file> | bbb shell build <file> [-o out]");
                    ExitCode::FAILURE
                }
            }
        }
        "init" => {
            if rest.is_empty() {
                eprintln!("usage: bbb init <name>");
                return ExitCode::FAILURE;
            }
            finish(cmd_init(&rest[0]))
        }
        "build" => {
            // bbb build [dir] [-s|-m [out]] [-o out]
            let mut dir = ".".to_string();
            let mut mode = "s";
            let mut out = None;
            let mut i = 0;
            while i < rest.len() {
                match rest[i].as_str() {
                    "-s" => mode = "s",
                    "-m" => {
                        mode = "m";
                        if i + 1 < rest.len() && !rest[i + 1].starts_with('-') {
                            out = Some(rest[i + 1].clone());
                            i += 1;
                        }
                    }
                    "-o" => {
                        if i + 1 < rest.len() {
                            out = Some(rest[i + 1].clone());
                            i += 1;
                        }
                    }
                    other => dir = other.to_string(),
                }
                i += 1;
            }
            finish(cmd_build(&dir, mode, out.as_deref()))
        }
        "run" => {
            let target = rest.first().map(|s| s.as_str()).unwrap_or(".");
            finish(cmd_run(target))
        }
        "install" => {
            let dir = rest.first().map(|s| s.as_str()).unwrap_or(".");
            finish(cmd_install(dir))
        }
        "destroy" => {
            let dir = rest.first().map(|s| s.as_str()).unwrap_or(".");
            finish(cmd_destroy(dir))
        }
        "pack" => {
            if rest.len() < 2 {
                eprintln!("usage: bbb pack <out.img|.zip> [--entry NAME] <files...>");
                return ExitCode::FAILURE;
            }
            let out = &rest[0];
            let mut entry = None;
            let mut files = Vec::new();
            let mut i = 1;
            while i < rest.len() {
                if rest[i] == "--entry" && i + 1 < rest.len() {
                    entry = Some(rest[i + 1].clone());
                    i += 2;
                } else {
                    files.push(rest[i].clone());
                    i += 1;
                }
            }
            finish(cmd_pack(out, entry.as_deref(), &files))
        }
        "unpack" => {
            if rest.is_empty() {
                eprintln!("usage: bbb unpack <pkg> [dir]");
                return ExitCode::FAILURE;
            }
            let dir = rest.get(1).map(|s| s.as_str()).unwrap_or(".");
            finish(cmd_unpack(&rest[0], dir))
        }
        // 内部调试命令（保留）
        "lexer" => {
            if rest.is_empty() {
                eprintln!("usage: bbb lexer <file>");
                return ExitCode::FAILURE;
            }
            finish(cmd_tokens(&rest[0]))
        }
        "parse" => {
            if rest.is_empty() {
                eprintln!("usage: bbb parse <file>");
                return ExitCode::FAILURE;
            }
            finish(cmd_parse(&rest[0]))
        }
        "arena" => {
            let n: u32 = rest.first().and_then(|s| s.parse().ok()).unwrap_or(10_000);
            cmd_arena(n);
            ExitCode::SUCCESS
        }
        // 默认：`bbb <file>` 解释运行
        _ => finish(cmd_run(cmd)),
    }
}

fn finish(r: Result<(), String>) -> ExitCode {
    match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}
