//! dylib 模块测试：候选名生成 + 实际加载/符号查找（Linux 环境）。

use bbb_vm::dylib;

#[test]
fn candidate_names_keeps_original() {
    let c = dylib::candidate_names("libm.so");
    assert_eq!(c[0], "libm.so");
    assert!(c.contains(&"libm.so".to_string()));
}

#[test]
fn candidate_names_linux_so() {
    // Linux：libm.so → 版本化兜底 libm.so.6 / libm.so.1
    let c = dylib::candidate_names("libm.so");
    assert!(c.contains(&"libm.so.6".to_string()), "{c:?}");
}

#[test]
fn candidate_names_versioned_no_garbage() {
    // libm.so.6 不应生成 libm.so.so 之类的垃圾
    let c = dylib::candidate_names("libm.so.6");
    assert!(!c.iter().any(|x| x.contains(".so.so")), "{c:?}");
    assert!(c.contains(&"libm.so".to_string()), "{c:?}");
}

#[test]
fn candidate_names_bare_lib() {
    let c = dylib::candidate_names("libm");
    assert!(c.contains(&"libm.so".to_string()), "{c:?}");
    assert!(c.contains(&"libm.so.6".to_string()), "{c:?}");
}

#[cfg(unix)]
#[test]
fn open_and_symbol_libm() {
    // 平台真实库：声明 libm.so（链接脚本），应通过候选命中 libm.so.6
    let h = dylib::open_any("libm.so").expect("open libm");
    let s = dylib::symbol(h, "sin").expect("symbol sin");
    let f: fn(f64) -> f64 = unsafe { std::mem::transmute(s) };
    assert_eq!(f(0.0), 0.0);
    assert!((f(std::f64::consts::FRAC_PI_2) - 1.0).abs() < 1e-9);
}
