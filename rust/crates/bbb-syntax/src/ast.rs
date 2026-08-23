//! BiuBiuBiu AST（完全原生手写版）。
//!
//! 2026-08-22 起替代 MPS 生成骨架（ast_generated.rs 保留作对照，不再作为
//! 事实源）。设计贴合旧 C 实现（src/parser.c）的节点形态，并做了枚举化
//! 与所有权整理：
//! - 表达式/语句/声明全枚举，无 NULL 指针（Option 表达可选）；
//! - 字符串用 `String`（解析期零拷贝优化留给后续：arena 字符串池）；
//! - 语法面覆盖 examples/01-17：流签名/分叉/类/need/注解/智能引用/
//!   数组字面量/多返回值/二进制库流。

/// 程序 = 声明序列（Main 流单独存放，语义上总在最后执行）。
#[derive(Debug, Clone, PartialEq)]
pub struct Program {
    pub kind: String, // "main" | "utils"
    pub decls: Vec<Decl>,
    pub main: Option<MainDecl>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Decl {
    /// `const int x = 10;`（顶层 → Constantstream）
    Const { name: String, ty: String, init: Expr },
    /// `need value/function/stream/Class X;`
    Need { kind: String, name: String },
    /// `Stream Name { members }` — 签名流
    StreamSig { name: String, members: Vec<Member>, annos: Vec<String> },
    /// `Stream Name & "lib.so" { members }` — 二进制库流
    StreamBin { name: String, file: String, members: Vec<Member>, annos: Vec<String> },
    /// `Class Name { members }`
    Class { name: String, members: Vec<Member>, annos: Vec<String> },
    /// `Sig Name { members }` — 分叉实现
    Fork { sig: String, name: String, members: Vec<Member>, annos: Vec<String> },
}

/// Main 流：`Main { void exec() {...} ... }`，仅方法。
#[derive(Debug, Clone, PartialEq)]
pub struct MainDecl {
    pub methods: Vec<Method>,
}

/// 流/类成员：字段与方法可任意交错；字段支持逗号分隔 `int x, y;`。
#[derive(Debug, Clone, PartialEq)]
pub enum Member {
    Field { ty: String, names: Vec<String> },
    Method(Method),
}

#[derive(Debug, Clone, PartialEq)]
pub struct Method {
    pub ret: String, // void / int / int[] / Hero / T[]...
    pub name: String,
    pub params: Vec<Param>,
    pub body: Vec<Stmt>, // 空 = 签名（分号结尾）
    pub annos: Vec<String>, // @read/@write/@call/@ucall
}

#[derive(Debug, Clone, PartialEq)]
pub struct Param {
    pub name: String,
    pub ty: String, // 基类型（含流名/类名）
    pub is_arr: bool,
    /// 智能引用参数：`&<perm> <follow> name type`
    pub ref_perm: Option<String>,
    pub ref_follow: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Stmt {
    If { cond: Expr, then: Vec<Stmt>, els: Option<Vec<Stmt>> },
    While { cond: Expr, body: Vec<Stmt> },
    For { init: Option<Box<Stmt>>, cond: Option<Expr>, update: Option<Box<Stmt>>, body: Vec<Stmt> },
    Break,
    Continue,
    /// `res expr;` / `res a, b, c;`（多值 → 数组）/ `ref "reason";`
    Ret { kind: RetKind, values: Vec<Expr> },
    /// 变量声明与赋值：`int x = e;` / `ALL x = e;` / `x = e;` / `x += e;` /
    /// `const int x = e;` / `thread int x = e;` / `this::attr = e;` / `a[i] = e;`
    Assign {
        vtype: Option<String>, // Some = 声明（含 "ALL"/"const"/"thread" 变体）
        is_const: bool,
        is_thread: bool,
        target: AssignTarget,
        op: String,
        value: Expr,
    },
    /// `&perm follow base name = &lvalue;` — 智能引用声明
    RefDecl { perm: String, follow: String, base: String, name: String, init: Expr },
    /// `i++;` / `i--;`
    Inc { name: String, op: String },
    /// 表达式语句：裸调用 `add(1,2);`、`a[i];` 等
    Expr(Expr),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RetKind {
    Res, // res = respond
    Ref, // ref = refuse
}

#[derive(Debug, Clone, PartialEq)]
pub enum AssignTarget {
    Var(String),
    Index { base: Box<Expr>, idx: Box<Expr> },
    Prop { base: Box<Expr>, name: String },
}

#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    Int(i64),
    Float(f64),
    Str(String),
    Char(u8),
    Bool(bool),
    /// 变量引用（含 `this`）
    Var(String),
    /// `qual::name(args)`（qual=Some）或裸调用 `name(args)`（qual=None）
    Call { qual: Option<String>, name: String, args: Vec<Expr> },
    /// `obj.field` 属性访问（对象属性用 Objstream）
    Prop { base: Box<Expr>, name: String },
    /// `a[i]` 索引
    Index { base: Box<Expr>, idx: Box<Expr> },
    BinOp { op: String, l: Box<Expr>, r: Box<Expr> },
    /// 前缀解包：`get X` / `cause X`
    Unwrap { op: String, l: Box<Expr> },
    /// `new Class(args...)` → 分叉类流 + 自动 __init__
    New { cls: String, args: Vec<Expr> },
    /// `new Type[expr]` → 数组字面量
    NewArray { ty: String, size: Box<Expr> },
    /// `&lvalue` — 取址创建引用值（权限来自声明）
    RefOf(Box<Expr>),
}
