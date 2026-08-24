//! bbb-vm — BioLang 解释器（M3）。
//!
//! 设计（对齐旧 C interp.c + examples 语义）：
//! - 流注册表：签名流/分叉/类/Main 统一登记，方法名解析（qual::name →
//!   流方法 → 签名回退分叉 → 对象方法；裸调用 → 全局方法名扫描）；
//! - 值语义：Value 16B（bbb-core），调用结果带 REFUSED 位（cause 为
//!   字符串句柄），`get`/`cause` 是位测试；
//! - 对象：arena 句柄（ObjData = 类定义 + 字段 + 动态属性），
//!   Array/Vector 是注入的 Bio 类源码（底层 Solid 流，Rust 实现）；
//! - 控制流：Flow 枚举（Next/Ret/Break/Continue）驱动语句块。

pub mod builtin;
pub mod dylib;
pub mod interp;
pub mod project;
pub mod registry;

pub use interp::{Interp, RunOutcome};
pub use project::load_project_sources;
pub use registry::Registry;

/// 预置的 Array/Vector 类源码（Bio 语言编写，与旧 C 注入的类一致）。
pub const BUILTIN_CLASS_SRC: &str = r#"
Class Array {
    void __init__(n int) {
        this::data = Solid::new().res;
        ALL i = 0;
        while (i < n) { Solid::push(this::data, 0); i = i + 1; }
        Arrays::add(this);
    }
    int len() { res Solid::len(this::data); }
    void set(i int, v) { Solid::set(this::data, i, v); }
    int get(i int) { res Solid::get(this::data, i); }
    void push(v) { Solid::push(this::data, v); }
    string join(sep string) { res Solid::join(this::data, sep); }
    void sort() { __sort__(); }
}
Class Vector {
    void __init__() { this::data = Solid::new().res; Arrays::add(this); }
    int len() { res Solid::len(this::data); }
    void set(i int, v) { Solid::set(this::data, i, v); }
    int get(i int) { res Solid::get(this::data, i); }
    void push(v) { Solid::push(this::data, v); }
    void sort() { __sort__(); }
}
"#;
