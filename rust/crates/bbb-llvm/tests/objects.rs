//! bbb-llvm 集成测试：编译 → clang → 运行 → 输出断言。
//! 需要系统 clang（与 CLI shell build 相同路径）。

use std::process::Command;

use bbb_llvm::compile;
use bbb_syntax::parser::parse_source;

fn build_and_run(src: &str) -> String {
    let (prog, errs) = parse_source(src);
    assert!(errs.is_empty(), "parse errors: {errs:?}");
    let ir = compile(&prog).expect("IR generation failed");

    // 目录含进程+时间戳，避免并行测试冲突
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.subsec_nanos())
        .unwrap_or(0);
    let dir = std::env::temp_dir().join(format!("bbb-llvm-test-{}-{nanos}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let ir_path = dir.join("out.ll");
    let bin_path = dir.join("a.out");
    std::fs::write(&ir_path, &ir).unwrap();

    let status = Command::new("clang")
        .arg(&ir_path)
        .arg("-o")
        .arg(&bin_path)
        .status()
        .expect("clang not found");
    assert!(status.success(), "clang failed");

    let out = Command::new(&bin_path).output().expect("run failed");
    let _ = std::fs::remove_dir_all(&dir);
    String::from_utf8_lossy(&out.stdout).to_string()
}

#[test]
fn llvm_class_new_fields_and_methods() {
    let src = r#"
program main;
Class Student {
    void __init__(age int, solve double) {
        this::age = age;
        this::solve = solve;
    }
    int getAge() { res this::age; }
    double getSolve() { res this::solve; }
    void bump() { this::age = this::age + 1; }
    int age;
    double solve;
}
Main {
    void exec() {
        Student s = new Student(12, 1.2);
        CIO::println("age:", get s::getAge());
        CIO::println("solve:", get s::getSolve());
        s::bump();
        CIO::println("after bump:", get s::getAge());
        s::age = 20;
        CIO::println("direct:", s::age);
    }
}
"#;
    let out = build_and_run(src);
    assert!(out.contains("age: 12"), "got: {out}");
    assert!(out.contains("solve: 1.200000"), "got: {out}");
    assert!(out.contains("after bump: 13"), "got: {out}");
    assert!(out.contains("direct: 20"), "got: {out}");
}

#[test]
fn llvm_class_method_call_with_args() {
    let src = r#"
program main;
Class Calc {
    int add(a int, b int) { res a + b; }
    int mul(a int, b int) { res a * b; }
}
Main {
    void exec() {
        Calc c = new Calc();
        CIO::println("sum:", get c::add(2, 3));
        CIO::println("prod:", get c::mul(4, 5));
    }
}
"#;
    let out = build_and_run(src);
    assert!(out.contains("sum: 5"), "got: {out}");
    assert!(out.contains("prod: 20"), "got: {out}");
}
