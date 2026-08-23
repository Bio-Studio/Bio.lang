//! M3 解释器回归：examples 01-08 + 12 + 13 输出断言。
//! 完整期望值以旧 C 解释器（bin/bio）实测输出为准（标准层）。

use std::path::PathBuf;

use bio_syntax::parser::parse_source;
use bio_vm::interp::Interp;

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
    let prog = bio_vm::load_project_sources(&root).unwrap();
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
