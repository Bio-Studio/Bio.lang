//! M3 解释器回归：examples 01-08 + 12 + 13 输出断言。
//! 完整期望值以旧 C 解释器（bin/bio）实测输出为准（标准层）。

use std::path::PathBuf;

use bbb_syntax::parser::parse_source;
use bbb_vm::interp::Interp;

fn examples_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent().unwrap().parent().unwrap().parent().unwrap()
        .join("examples")
}

fn run_file(name: &str) -> String {
    let path = examples_dir().join(name);
    let src = std::fs::read_to_string(&path).unwrap();
    let (prog, errs) = parse_source(&src);
    assert!(errs.is_empty(), "{name} parse errors: {errs:?}");
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert!(out.unmet_needs.is_empty(), "{name} unmet needs: {:?}", out.unmet_needs);
    out.stdout
}

#[test]
fn ex01_hello() {
    let out = run_file("01-hello.bio");
    assert_eq!(out, "Hello, BioLang!\nEvery call in BioLang is a request.\nThis one was just a request to print a line.\n");
}

#[test]
fn ex02_requests() {
    let out = run_file("02-requests.bio");
    assert!(out.contains("3 + 4 = 7"));
    assert!(out.contains("div cause: division by zero"));
    assert!(out.contains("missing method cause: stream MyCalc refuses: no method sqrt"));
    assert!(out.contains("default return cause: nothing"));
    assert!(out.contains("if is false (default ref nothing)"));
}

#[test]
fn ex03_control_flow() {
    let out = run_file("03-control-flow.bio");
    assert!(out.contains("1..10 sum (while) = 55"));
    assert!(out.contains("5! (for) = 120"));
    assert!(out.contains("1 2 3 5 6 7"));
    assert!(out.contains("for(;;) counted: 3"));
}

#[test]
fn ex04_streams_fork() {
    let out = run_file("04-streams-fork.bio");
    assert!(out.contains("Calc::add(2,3) = 5"));
    assert!(out.contains("bare add(10,20) = 30"));
    assert!(out.contains("count = 3  doubleGet = 6"));
    assert!(out.contains("hello from a passed stream!"));
}

#[test]
fn ex05_io() {
    let out = run_file("05-io-substreams.bio");
    assert!(out.contains("SIO::format → 2 + 3 = 5"));
    assert!(out.contains("SIO::upper → HELLO"));
    assert!(out.contains("SIO::getln → line one"));
    assert!(out.contains("FIO read back: Hello from BioLang! Appended line"));
}

#[test]
fn ex06_classes() {
    let out = run_file("06-classes-objects.bio");
    assert!(out.contains("object: <object Hero {name: TAK, hp: 88}>"));
    assert!(out.contains("name = TAK  hp = 88"));
    assert!(out.contains("Obj::call getHp() = 100"));
    assert!(out.contains("h::getName() = TAK"));
}

#[test]
fn ex07_arrays() {
    let out = run_file("07-arrays.bio");
    assert!(out.contains("array: [10, 20, 30]"));
    assert!(out.contains("after push: [10, 20, 30, 40]  len: 4"));
    assert!(out.contains("join(-): 10-20-30-40"));
    assert!(out.contains("a[1] = 20"));
    assert!(out.contains("after a[1] = 99: [10, 99, 30, 40]"));
    assert!(out.contains("new int[4] squares: [0, 1, 4, 9]"));
    assert!(out.contains("vector: [10, 20, 30]  len: 3"));
    assert!(out.contains("Arrays count: 3"));
    assert!(out.contains("after forget: 2"));
}

#[test]
fn ex08_multi_return() {
    let out = run_file("08-multi-return.bio");
    assert!(out.contains("triple(10) = [10, 20, 30]"));
    assert!(out.contains("arr = [10, 20, 30]  arr[1] = 20  len = 3"));
}

#[test]
fn ex12_computation() {
    let out = run_file("12-computation.bio");
    assert!(out.contains("Com::abs(0-5) = 5   Com::sqrt(9) = 3"));
    assert!(out.contains("Com::pow(2,10) = 1024"));
    assert!(out.contains("Time::reset(forked) ok"));
}

#[test]
fn ex13_need() {
    let out = run_file("13-need.bio");
    assert!(out.contains("PI = 3"));
    assert!(out.contains("hello, TAK"));
    assert!(out.contains("writing via a needed stream"));
    assert!(out.contains("hero created, hp = 0"));
}

#[test]
fn project_multi_file() {
    let root = examples_dir().join("project");
    let prog = bbb_vm::load_project_sources(&root).unwrap();
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert!(out.unmet_needs.is_empty(), "unmet: {:?}", out.unmet_needs);
    assert_eq!(out.stdout, "main entry — Hello from utils/\nhello, project\n");
}

#[test]
fn need_unmet_is_error() {
    let src = r#"
program main;
need value MISSING;
Main { void exec() { CIO::println("x"); } }
"#;
    let (prog, errs) = parse_source(src);
    assert!(errs.is_empty());
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert_eq!(out.unmet_needs, vec![("value".to_string(), "MISSING".to_string())]);
}

#[test]
fn ex09_threads() {
    let out = run_file("09-threads.bio");
    assert!(out.contains("live threads: 2"));
    assert!(out.contains("thread 1  10! = 3628800"));
    assert!(out.contains("thread 2  countUp = 5"));
}

#[test]
fn ex10_taskm() {
    let out = run_file("10-taskm.bio");
    assert!(out.contains("tasks: 2"));
    assert!(out.contains("tasks done: 0"));
    assert!(out.contains("jobA sum = 15"));
    assert!(out.contains("jobB 2^4 = 16"));
}

#[test]
fn ex11_smart_refs() {
    let out = run_file("11-smart-refs.bio");
    assert!(out.contains("u read counter = 10"));
    assert!(out.contains("read-only write → Ref refused: reference is read-only, cannot write"));
    assert!(out.contains("rw read = 10"));
    assert!(out.contains("counter after rw = 5 → 5"));
    assert!(out.contains("thread 2  a-layer = 44"));
    assert!(out.contains("thread 1  a-layer = 42"));
    assert!(out.contains("t1 = 42  t2 = 44"));
    assert!(out.contains("at [1] = refused: refused: reference is write-only, cannot read"));
    assert!(out.contains("⛔ main stream refused: refused: reference is read-only, cannot write"));
}

#[test]
fn ex14_binary_lib() {
    let out = run_file("14-binary-lib.bio");
    assert!(out.contains("m::sin(0) = 0"));
    assert!(out.contains("m::cos(0) = 1"));
    assert!(out.contains("m::pow(2,10) = 1024"));
    assert!(out.contains("m::doubleIt(21) = 42"));
}

#[test]
fn ex15_annotations() {
    let out = run_file("15-annotations.bio");
    assert!(out.contains("refused: stream Sealed is @unfork, cannot fork"));
    assert!(out.contains("new @unfork class → Obj refused: class Frozen is @unfork, cannot fork"));
    assert!(out.contains("onlyread get = 0"));
    assert!(out.contains("onlyread bump → refused: stream RO is @onlyread — bump() is a write method"));
    assert!(out.contains("alias get = 0"));
    assert!(out.contains("alias touch → refused: stream RA is @onlyread — touch() is a write method"));
    assert!(out.contains("marked write → refused: stream G is @onlyread — markedWrite() is a write method"));
    assert!(out.contains("marked read = 1"));
    assert!(out.contains("safe read = 1"));
}

#[test]
fn ex16_phonebooth() {
    let out = run_file("16-phonebooth.bio");
    assert!(out.contains("t1 sum = 5050  t2 sum = 20100"));
    assert!(out.contains("global 5! = 120"));
    assert!(out.contains("global 6! = 720"));
    assert!(out.contains("direct recursion → refused: phone-booth method down does not support recursion"));
    assert!(out.contains("indirect recursion → refused: phone-booth method down2 does not support recursion"));
    assert!(out.contains("ucall recursion → refused: phone-booth method uDown does not support recursion"));
    assert!(out.contains("plain fact(10) = 3628800"));
}

#[test]
fn new_classname_decl() {
    // 类名 = new 类()：宏展开语法（parser 层）
    let src = r#"
program main;
Class Box {
    void __init__(v int) { this::val = v; }
    int val;
}
Main {
    void exec() {
        Box b = new Box(42);
        CIO::println("val=", get b::val);
    }
}
"#;
    let (prog, errs) = parse_source(src);
    assert!(errs.is_empty(), "parse errors: {errs:?}");
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert!(out.unmet_needs.is_empty());
    assert!(out.stdout.contains("val= 42"), "got: {}", out.stdout);
}

#[test]
fn arrays_sort_inplace() {
    // Arrays::sort(arr) 原地排序 + arr::sort() → 内部 __sort__()
    let src = r#"
program main;
Main {
    void exec() {
        Array a = new Array(4);
        a::set(0, 30); a::set(1, 10); a::set(2, 50); a::set(3, 20);
        Arrays::sort(a);
        CIO::println("s1=", a);
        a::sort();
        CIO::println("s2=", a);
    }
}
"#;
    let (prog, errs) = parse_source(src);
    assert!(errs.is_empty(), "parse errors: {errs:?}");
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert!(out.unmet_needs.is_empty());
    assert!(out.stdout.contains("s1= [10, 20, 30, 50]"), "got: {}", out.stdout);
    assert!(out.stdout.contains("s2= [10, 20, 30, 50]"), "got: {}", out.stdout);
}

#[test]
fn interface_basic_and_polymorphism() {
    let src = r#"
program main;
Interface Shape {
    double area();
}
Class Circle implements Shape {
    double area() { res 3.14 * this::r * this::r; }
    double r;
}
Class Square implements Shape {
    double area() { res this::side * this::side; }
    double side;
}
Main {
    void exec() {
        Shape s = new Circle();
        s::r = 2.0;
        CIO::println("circle=", get s::area());
        Shape t = new Square();
        t::side = 3.0;
        CIO::println("square=", get t::area());
    }
}
"#;
    let (prog, errs) = parse_source(src);
    assert!(errs.is_empty(), "parse errors: {errs:?}");
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert!(out.unmet_needs.is_empty());
    assert!(out.stdout.contains("circle= 12.56"), "got: {}", out.stdout);
    assert!(out.stdout.contains("square= 9"), "got: {}", out.stdout);
}

#[test]
fn interface_missing_method_rejected() {
    let src = r#"
program main;
Interface Shape {
    double area();
    void draw();
}
Class Circle implements Shape {
    double area() { res 0.0; }
}
Main {
    void exec() {}
}
"#;
    let (prog, errs) = parse_source(src);
    assert!(errs.is_empty(), "parse errors: {errs:?}");
    let mut interp = Interp::new();
    let out = interp.run(&prog);
    assert!(
        out.unmet_needs.iter().any(|(k, _)| k.contains("draw")),
        "expected missing draw method, got: {:?}",
        out.unmet_needs
    );
}
