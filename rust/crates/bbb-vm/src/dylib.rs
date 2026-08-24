//! dylib — 跨平台动态库加载（二进制库流底层）。
//!
//! 平台适配：
//! - Linux / Android：`dlopen` / `dlsym`（.so）
//! - macOS / iOS：`dlopen` / `dlsym`（.dylib；dlopen 可用）
//! - Windows：`LoadLibraryA` / `GetProcAddress`（.dll）
//!
//! 库名归一化：声明 `Stream m & "libm.so"` 时，按当前平台尝试
//! 候选文件名（libm.so → libm.dylib → libm.dll / m.dll），
//! 并保留原始名兜底。这样同一份 .bio 源码可以跨系统运行。

use std::ffi::CString;
use std::os::raw::c_char;

/// 打开的库句柄（平台特定的不透明指针）。
#[cfg(unix)]
pub type LibHandle = *mut std::ffi::c_void;
#[cfg(windows)]
pub type LibHandle = *mut std::ffi::c_void;

/// 平台库名候选：把声明的名字转成当前平台可能的真实文件名。
/// 策略：原样优先 + 平台等价后缀 + 版本化后缀剥离（libm.so.6 → libm.so）。
pub fn candidate_names(declared: &str) -> Vec<String> {
    let mut out = Vec::new();
    let base = declared.trim();
    if base.is_empty() {
        return out;
    }
    // 1. 原样
    out.push(base.to_string());

    // 2. 剥离版本后缀：libm.so.6 → libm.so；foo.1.2 → foo（仅 .so/.dylib/.dll 前的版本号）
    if let Some((stem, ver)) = split_version(base) {
        if !ver.is_empty() && out.iter().all(|x| x != &stem) {
            out.push(stem.clone());
        }
    }

    // 3. 平台等价后缀
    let (stem, ext) = split_ext(base);
    // ext 为纯数字版本号（libm.so.6 的 "6"）时视为版本化，不按后缀处理
    let ext_is_ver = !ext.is_empty() && ext.chars().all(|c| c.is_ascii_digit());
    #[cfg(target_os = "windows")]
    {
        if ext != "dll" && !ext_is_ver {
            out.push(format!("{stem}.dll"));
        }
        if let Some(rest) = stem.strip_prefix("lib") {
            if !rest.is_empty() {
                out.push(format!("{rest}.dll"));
            }
        }
        // .so.6 → .dll 也试：libm → m.dll
        if let Some(rest) = base.split('.').next().and_then(|s| s.strip_prefix("lib")) {
            if !rest.is_empty() {
                out.push(format!("{rest}.dll"));
            }
        }
    }
    #[cfg(target_os = "macos")]
    {
        if ext != "dylib" && !ext_is_ver {
            out.push(format!("{stem}.dylib"));
        }
        // libfoo.so.6 → libfoo.dylib
        if let Some((s, _)) = split_version(base) {
            let (s2, _) = split_ext(&s);
            if s2 != stem && s2 != "" {
                out.push(format!("{s2}.dylib"));
            }
        }
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if ext != "so" && !ext_is_ver {
            out.push(format!("{stem}.so"));
        }
        // libm.so 可能是链接脚本 → 常见版本化真实库兜底
        if (ext == "so" || ext == "") && !ext_is_ver {
            out.push(format!("{stem}.so.6"));
            out.push(format!("{stem}.so.1"));
        }
    }
    // 去重
    let mut seen = std::collections::HashSet::new();
    out.retain(|x| seen.insert(x.clone()));
    out
}

/// 剥离版本号：libm.so.6 → ("libm.so", "6")；foo.1.2 → ("foo", "1.2")。
/// 仅当最后一段是数字或 .so.X/.dylib.X 形态。
fn split_version(name: &str) -> Option<(String, String)> {
    let (stem, last) = name.rsplit_once('.')?;
    if last.is_empty() || !last.chars().all(|c| c.is_ascii_digit()) {
        return None;
    }
    Some((stem.to_string(), last.to_string()))
}

fn split_ext(name: &str) -> (String, String) {
    match name.rsplit_once('.') {
        Some((s, e)) if !s.is_empty() => (s.to_string(), e.to_string()),
        _ => (name.to_string(), String::new()),
    }
}

/// 打开动态库，返回句柄；失败返回 None。
pub fn open(path: &str) -> Option<LibHandle> {
    let c = CString::new(path).ok()?;
    #[cfg(unix)]
    {
        // RTLD_LAZY = 1
        let h = unsafe { dlopen(c.as_ptr() as *const c_char, 1) };
        if h.is_null() { None } else { Some(h) }
    }
    #[cfg(windows)]
    {
        let h = unsafe { LoadLibraryA(c.as_ptr() as *const c_char) };
        if h.is_null() { None } else { Some(h) }
    }
}

/// 按声明名尝试打开库：遍历候选名，第一个成功的返回。
pub fn open_any(declared: &str) -> Option<LibHandle> {
    for name in candidate_names(declared) {
        if let Some(h) = open(&name) {
            return Some(h);
        }
    }
    None
}

/// 查找符号，返回裸指针；失败返回 None。
pub fn symbol(h: LibHandle, name: &str) -> Option<*mut std::ffi::c_void> {
    let c = CString::new(name).ok()?;
    #[cfg(unix)]
    {
        let p = unsafe { dlsym(h, c.as_ptr() as *const c_char) };
        if p.is_null() { None } else { Some(p) }
    }
    #[cfg(windows)]
    {
        let p = unsafe { GetProcAddress(h as *mut _, c.as_ptr() as *const c_char) };
        if p.is_null() { None } else { Some(p.cast()) }
    }
}

/// 关闭动态库。
pub fn close(h: LibHandle) {
    #[cfg(unix)]
    unsafe {
        dlclose(h);
    }
    #[cfg(windows)]
    unsafe {
        FreeLibrary(h as *mut _);
    }
}

// ---- FFI 声明 ----

#[cfg(unix)]
extern "C" {
    fn dlopen(filename: *const c_char, flags: i32) -> *mut std::ffi::c_void;
    fn dlsym(handle: *mut std::ffi::c_void, symbol: *const c_char) -> *mut std::ffi::c_void;
    fn dlclose(handle: *mut std::ffi::c_void) -> i32;
}

#[cfg(windows)]
extern "system" {
    fn LoadLibraryA(lpFileName: *const c_char) -> *mut std::ffi::c_void;
    fn GetProcAddress(hModule: *mut std::ffi::c_void, lpProcName: *const c_char) -> *mut std::ffi::c_void;
    fn FreeLibrary(hLibModule: *mut std::ffi::c_void) -> i32;
}
