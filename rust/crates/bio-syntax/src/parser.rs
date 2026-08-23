//! BioLang 解析器（完全原生手写）。
//!
//! 语法面以旧 C 实现（src/parser.c）与 examples/01-17 为准：
//! - 表达式：优先级 字面量/引用/调用/索引/属性链 → 一元(get/cause/&/new)
//!   → 算术(+ - * / %) → 比较(== != < > <= >=) → 逻辑(&& ||)
//! - 语句：if/while/for/break/continue/res/ref/ALL/const/thread/
//!   &引用声明/类型声明/赋值/自增自减/调用/表达式语句
//! - 声明：program/const/need/Stream(签名|二进制库)/Class/Main/分叉
//! - 成员：方法（体或签名）+ 字段（逗号分隔）+ type T; 泛型 + 泛型风格
//! - 参数：`name type`（名字在前），可带 `&perm follow` 引用修饰
//!
//! 错误处理：收集全部错误（ParseError 带行列），恢复策略为推进 token，
//! 保证不死循环；返回的 Program 在 errors 非空时不可信（调用方检查）。

use crate::ast::*;
use crate::lexer::{Token, TokenKind};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseError {
    pub line: u32,
    pub col: u32,
    pub msg: String,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}:{}: {}", self.line, self.col, self.msg)
    }
}

const TYPE_NAMES: &[&str] = &["int", "float", "double", "string", "char", "bool"];
const PERMS: &[&str] = &["r", "w", "m", "rw", "rm", "wm", "rwm"];
const FOLLOWS: &[&str] = &["u", "f", "a", "t"];
const ASSIGN_OPS: &[&str] = &["=", "+=", "-=", "*=", "/=", "%="];
const METHOD_ANNOS: &[&str] = &["read", "write", "call", "ucall"];
const DECL_ANNOS: &[&str] = &["onlyread", "unfork"];

fn is_type_name(s: &str) -> bool {
    TYPE_NAMES.contains(&s)
}

pub struct Parser<'a> {
    toks: &'a [Token<'a>],
    pos: usize,
    pub errors: Vec<ParseError>,
}

impl<'a> Parser<'a> {
    pub fn new(toks: &'a [Token<'a>]) -> Self {
        Parser { toks, pos: 0, errors: Vec::new() }
    }

    // ---- 游标 ----

    fn peek(&self, k: usize) -> &'a Token<'a> {
        let i = (self.pos + k).min(self.toks.len().saturating_sub(1));
        &self.toks[i]
    }

    fn next(&mut self) -> &'a Token<'a> {
        let t = self.peek(0);
        if self.pos < self.toks.len().saturating_sub(1) {
            self.pos += 1;
        }
        t
    }

    fn at_eof(&self) -> bool {
        self.peek(0).kind == TokenKind::Eof
    }

    fn is_op(&self, k: usize, op: &str) -> bool {
        let t = self.peek(k);
        t.kind == TokenKind::Op && t.text == op
    }

    fn at_kw(&self, kw: &str) -> bool {
        let t = self.peek(0);
        t.kind == TokenKind::Keyword && t.text == kw
    }

    fn eat_op(&mut self, op: &str) -> bool {
        if self.is_op(0, op) {
            self.next();
            true
        } else {
            false
        }
    }

    fn error(&mut self, msg: impl Into<String>) {
        let t = self.peek(0);
        self.errors.push(ParseError { line: t.span.line, col: t.span.col, msg: msg.into() });
    }

    fn expect_op(&mut self, op: &str) -> bool {
        if self.eat_op(op) {
            true
        } else {
            self.error(format!("期望 '{}'，得到 '{}'", op, self.peek(0).text));
            false
        }
    }

    fn expect_id(&mut self) -> String {
        let t = self.peek(0);
        if t.kind == TokenKind::Ident || (t.kind == TokenKind::Keyword && t.text != "program") {
            self.next();
            t.text.to_string()
        } else {
            self.error(format!("期望标识符，得到 '{}'", t.text));
            String::new()
        }
    }

    /// 方法名：关键字 `new` 允许（如 Array::new / Obj::new）。
    fn expect_method_name(&mut self) -> String {
        if self.at_kw("new") {
            self.next();
            return "new".to_string();
        }
        self.expect_id()
    }

    fn err_if(&mut self, cond: bool, msg: impl Into<String>) {
        if cond {
            self.error(msg);
        }
    }

    // ---- 顶层 ----

    pub fn parse_program(&mut self) -> Program {
        let mut kind = String::new();
        let mut decls = Vec::new();
        let mut main = None;

        while !self.at_eof() && self.errors.len() < 50 {
            if self.at_kw("program") {
                self.next();
                let k = self.expect_id();
                self.expect_op(";");
                kind = k;
                continue;
            }
            if self.at_kw("const") {
                self.next();
                let ty = self.expect_id(); // 类型（int/string/...）
                let name = self.expect_id();
                let init = if self.eat_op("=") { self.parse_expr() } else { Expr::Int(0) };
                self.expect_op(";");
                decls.push(Decl::Const { name, ty, init });
                continue;
            }
            if self.at_kw("need") {
                self.next();
                let k = self.next().text.to_string();
                let name = self.expect_id();
                if self.is_op(0, "{") {
                    self.skip_block();
                    self.eat_op(";");
                } else {
                    self.expect_op(";");
                }
                self.err_if(!matches!(k.as_str(), "value" | "function" | "stream" | "Stream" | "Class"),
                            format!("need 只支持 value/function/stream/Class，得到 '{k}'"));
                decls.push(Decl::Need { kind: k, name });
                continue;
            }
            if self.at_kw("Stream") {
                self.next();
                let name = self.expect_id();
                if self.eat_op("&") {
                    let t = self.peek(0);
                    let file = match t.kind {
                        TokenKind::Str | TokenKind::Ident => self.next().text.to_string(),
                        _ => { self.error("期望二进制库文件名（字符串或标识符）"); String::new() }
                    };
                    let members = self.parse_members();
                    let annos = self.parse_decl_annos();
                    decls.push(Decl::StreamBin { name, file, members, annos });
                } else {
                    let members = self.parse_members();
                    let annos = self.parse_decl_annos();
                    decls.push(Decl::StreamSig { name, members, annos });
                }
                continue;
            }
            if self.at_kw("Class") {
                self.next();
                let name = self.expect_id();
                let members = self.parse_members();
                let annos = self.parse_decl_annos();
                decls.push(Decl::Class { name, members, annos });
                continue;
            }
            if self.at_kw("Main") {
                self.next();
                self.expect_op("{");
                let methods = self.parse_methods_until("}");
                self.expect_op("}");
                main = Some(MainDecl { methods });
                continue;
            }
            if self.peek(0).kind == TokenKind::Ident {
                let sig = self.next().text.to_string();
                let name = self.expect_id();
                let members = self.parse_members();
                let annos = self.parse_decl_annos();
                decls.push(Decl::Fork { sig, name, members, annos });
                continue;
            }
            self.error(format!("无法解析顶层声明 '{}'", self.peek(0).text));
            self.next(); // 推进防死循环
        }
        Program { kind, decls, main }
    }

    /// 跳过 { ... } 块（need 的细节假设块）。
    fn skip_block(&mut self) {
        if !self.eat_op("{") {
            return;
        }
        let mut depth = 1;
        while depth > 0 && !self.at_eof() {
            if self.eat_op("{") {
                depth += 1;
            } else if self.eat_op("}") {
                depth -= 1;
            } else {
                self.next();
            }
        }
    }

    // ---- 成员（流/类公共） ----

    /// 成员解析：方法与字段任意交错，直到 }。
    /// - `void m(params) {}` / `void m(params);`（签名）
    /// - `int m() {...}` / `int x, y;` / `int[] a;`
    /// - `type T;` 泛型占位
    /// - `T n;` / `T[] a;` / `T m() {...}` 泛型风格
    fn parse_members(&mut self) -> Vec<Member> {
        let mut out = Vec::new();
        self.expect_op("{");
        while !self.is_op(0, "}") && !self.at_eof() {
            let t = self.peek(0);
            let is_prim_kw = t.kind == TokenKind::Keyword && is_type_name(t.text);
            if t.kind == TokenKind::Keyword && t.text == "void" {
                self.next();
                let name = self.expect_id();
                let params = self.parse_params();
                let (body, _is_sig) = self.parse_method_tail();
                let annos = self.parse_method_annos();
                out.push(Member::Method(Method { ret: "void".into(), name, params, body, annos }));
                continue;
            }
            // 泛型占位：`type T;`（type 是关键字语义，词法器按 ident 处理）
            if t.kind == TokenKind::Ident && t.text == "type" {
                self.next();
                self.expect_id();
                self.expect_op(";");
                continue;
            }
            if t.kind == TokenKind::Ident || is_prim_kw {
                let ty0 = self.next().text.to_string();
                let mut ty = ty0.clone();
                let is_arr = self.eat_arr_suffix(&mut ty);
                if is_arr {
                    // T[] a;（字段）或 T[] m() {...}（数组返回方法）
                    if (self.peek(0).kind == TokenKind::Ident || self.peek(0).kind == TokenKind::Keyword)
                        && self.is_op(1, "(") {
                        let name = self.expect_id();
                        let params = self.parse_params();
                        let (body, _is_sig) = self.parse_method_tail();
                        let annos = self.parse_method_annos();
                        out.push(Member::Method(Method { ret: ty, name, params, body, annos }));
                    } else {
                        let names = self.parse_field_names();
                        out.push(Member::Field { ty, names });
                    }
                    continue;
                }
                // T m() {...} — 泛型/基类型返回方法（lookahead: 名字 + (；get/cause 等关键字可作方法名）
                if (self.peek(0).kind == TokenKind::Ident || self.peek(0).kind == TokenKind::Keyword)
                    && self.is_op(1, "(") {
                    let name = self.expect_id();
                    let params = self.parse_params();
                    let (body, _is_sig) = self.parse_method_tail();
                    let annos = self.parse_method_annos();
                    out.push(Member::Method(Method { ret: ty, name, params, body, annos }));
                    continue;
                }
                // 字段：T x, y; 或（基类型）int x;
                let names = self.parse_field_names();
                out.push(Member::Field { ty, names });
                continue;
            }
            self.error(format!("无法解析流/类成员 '{}'", t.text));
            self.next();
        }
        self.expect_op("}");
        out
    }

    /// Main 流专用：只有方法。
    fn parse_methods_until(&mut self, end: &str) -> Vec<Method> {
        let mut out = Vec::new();
        while !self.is_op(0, end) && !self.at_eof() {
            let t = self.peek(0);
            let (ret, name) = if t.kind == TokenKind::Keyword && t.text == "void" {
                self.next();
                ("void".to_string(), self.expect_id())
            } else if (t.kind == TokenKind::Ident || t.kind == TokenKind::Keyword) && is_type_name(t.text) {
                let mut ty = self.next().text.to_string();
                self.eat_arr_suffix(&mut ty);
                (ty, self.expect_id())
            } else {
                self.error(format!("期望方法（返回类型 + 名字），得到 '{}'", t.text));
                self.next();
                continue;
            };
            let params = self.parse_params();
            let (body, _sig) = self.parse_method_tail();
            let annos = self.parse_method_annos();
            out.push(Method { ret, name, params, body, annos });
        }
        out
    }

    fn parse_method_tail(&mut self) -> (Vec<Stmt>, bool) {
        if self.eat_op("{") {
            let stmts = self.parse_stmts_until("}");
            (stmts, false)
        } else {
            self.expect_op(";");
            (Vec::new(), true)
        }
    }

    fn eat_arr_suffix(&mut self, ty: &mut String) -> bool {
        if self.is_op(0, "[") {
            self.next();
            self.expect_op("]");
            ty.push_str("[]");
            true
        } else {
            false
        }
    }

    /// 字段名列表：`x, y;`（逗号分隔后跟分号）。
    fn parse_field_names(&mut self) -> Vec<String> {
        let mut names = vec![self.expect_id()];
        while self.eat_op(",") {
            names.push(self.expect_id());
        }
        self.expect_op(";");
        names
    }

    /// 参数：`a int` / `a int[]` / `&perm follow a IO`
    fn parse_params(&mut self) -> Vec<Param> {
        let mut out = Vec::new();
        self.expect_op("(");
        while !self.is_op(0, ")") && !self.at_eof() {
            let (ref_perm, ref_follow) = if self.eat_op("&") {
                let perm = self.expect_id();
                let follow = self.expect_id();
                (Some(perm), Some(follow))
            } else {
                (None, None)
            };
            let t = self.peek(0);
            if t.kind == TokenKind::Ident || (t.kind == TokenKind::Keyword && t.text != "program") {
                let name = self.next().text.to_string();
                let mut ty = String::new();
                let mut is_arr = false;
                if self.eat_op("[") {
                    self.expect_op("]");
                    is_arr = true;
                }
                // 参数类型：基类型关键字（int/string/...）或任意标识符（流名/类名/泛型 T）
                if self.peek(0).kind == TokenKind::Ident
                    || (self.peek(0).kind == TokenKind::Keyword && is_type_name(self.peek(0).text)) {
                    ty = self.next().text.to_string();
                    if self.eat_op("[") {
                        self.expect_op("]");
                        is_arr = true;
                    }
                } else {
                    self.error(format!("参数 '{}' 缺少类型（语法：name type）", name));
                }
                out.push(Param { name, ty, is_arr, ref_perm, ref_follow });
            } else {
                self.error(format!("无法解析参数 '{}'", t.text));
                self.next();
            }
            if !self.eat_op(",") {
                break;
            }
        }
        self.expect_op(")");
        out
    }

    fn parse_method_annos(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        while self.is_op(0, "@") {
            self.next();
            let a = self.expect_id();
            self.err_if(!METHOD_ANNOS.contains(&a.as_str()),
                        format!("未知方法注解 @{a}（仅 @read/@write/@call/@ucall）"));
            out.push(a);
        }
        out
    }

    fn parse_decl_annos(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        while self.is_op(0, "@") {
            self.next();
            let a = self.expect_id();
            self.err_if(!DECL_ANNOS.contains(&a.as_str()),
                        format!("未知流注解 @{a}（仅 @onlyread/@unfork）"));
            out.push(a);
        }
        out
    }

    // ---- 语句 ----

    fn parse_stmts_until(&mut self, end: &str) -> Vec<Stmt> {
        let mut out = Vec::new();
        while !self.is_op(0, end) && !self.at_eof() {
            let before = self.errors.len();
            let s = self.parse_stmt();
            out.push(s);
            if self.errors.len() > before {
                // 出错时推进到分号或块结束，避免级联
                while !self.is_op(0, ";") && !self.is_op(0, end) && !self.at_eof() {
                    self.next();
                }
                self.eat_op(";");
            }
        }
        if !self.at_eof() {
            self.next(); // 消费 end
        }
        out
    }

    fn parse_stmt(&mut self) -> Stmt {
        if self.at_kw("if") {
            return self.parse_if();
        }
        if self.at_kw("while") {
            return self.parse_while();
        }
        if self.at_kw("for") {
            return self.parse_for();
        }
        if self.at_kw("break") {
            self.next();
            self.expect_op(";");
            return Stmt::Break;
        }
        if self.at_kw("continue") {
            self.next();
            self.expect_op(";");
            return Stmt::Continue;
        }
        if self.at_kw("res") || self.at_kw("ref") {
            let kind = if self.at_kw("res") { RetKind::Res } else { RetKind::Ref };
            self.next();
            let mut values = Vec::new();
            if !self.is_op(0, ";") {
                values.push(self.parse_expr());
                while self.eat_op(",") {
                    values.push(self.parse_expr());
                }
            }
            self.expect_op(";");
            return Stmt::Ret { kind, values };
        }
        if self.at_kw("const") || self.at_kw("thread") {
            let is_const = self.at_kw("const");
            let modif = self.next().text.to_string();
            let _ = &modif;
            // const/thread 后可跟 ALL 或类型（const int x = 10; / thread int x = 10;）
            let t = self.peek(0);
            if t.kind == TokenKind::Keyword && t.text == "ALL" {
                self.next();
                let name = self.expect_id();
                let value = self.parse_assign_rhs("=");
                return Stmt::Assign {
                    vtype: Some("ALL".into()), is_const, is_thread: !is_const,
                    target: AssignTarget::Var(name), op: "=".into(), value,
                };
            }
            let mut ty = self.expect_id();
            self.eat_arr_suffix(&mut ty);
            let name = self.expect_id();
            let value = if self.eat_op("=") { self.parse_expr() } else { Expr::Int(0) };
            self.expect_op(";");
            return Stmt::Assign {
                vtype: Some(ty), is_const, is_thread: !is_const,
                target: AssignTarget::Var(name), op: "=".into(), value,
            };
        }
        if self.at_kw("ALL") {
            self.next();
            let name = self.expect_id();
            let value = self.parse_assign_rhs("=");
            self.expect_op(";");
            return Stmt::Assign {
                vtype: Some("ALL".into()), is_const: false, is_thread: false,
                target: AssignTarget::Var(name), op: "=".into(), value,
            };
        }
        // 基类型声明：`int x = e;` / `int[] a = e;` / `string s;`（类型是关键字）
        if self.peek(0).kind == TokenKind::Keyword && is_type_name(self.peek(0).text) {
            let mut ty = self.next().text.to_string();
            self.eat_arr_suffix(&mut ty);
            let name = self.expect_id();
            let value = if self.eat_op("=") { self.parse_expr() } else { Expr::Int(0) };
            self.expect_op(";");
            return Stmt::Assign {
                vtype: Some(ty), is_const: false, is_thread: false,
                target: AssignTarget::Var(name), op: "=".into(), value,
            };
        }
        if self.is_op(0, "&") {
            return self.parse_ref_decl();
        }
        // this:: 开头的语句（this 是关键字）：属性赋值/方法调用
        if self.peek(0).kind == TokenKind::Ident || self.at_kw("this") {
            return self.parse_ident_stmt();
        }
        self.error(format!("无法解析语句 '{}'", self.peek(0).text));
        Stmt::Expr(Expr::Int(0))
    }

    fn parse_assign_rhs(&mut self, op: &str) -> Expr {
        if !self.eat_op(op) {
            self.error(format!("期望 '{}'", op));
        }
        self.parse_expr()
    }

    fn parse_if(&mut self) -> Stmt {
        self.next(); // if
        self.expect_op("(");
        let cond = self.parse_expr();
        self.expect_op(")");
        self.expect_op("{");
        let then = self.parse_stmts_until("}");
        let els = if self.at_kw("else") {
            self.next();
            if self.at_kw("if") {
                Some(vec![self.parse_if()])
            } else {
                self.expect_op("{");
                Some(self.parse_stmts_until("}"))
            }
        } else {
            None
        };
        Stmt::If { cond, then, els }
    }

    fn parse_while(&mut self) -> Stmt {
        self.next(); // while
        self.expect_op("(");
        let cond = self.parse_expr();
        self.expect_op(")");
        self.expect_op("{");
        let body = self.parse_stmts_until("}");
        Stmt::While { cond, body }
    }

    fn parse_for(&mut self) -> Stmt {
        self.next(); // for
        self.expect_op("(");
        let init = if self.is_op(0, ";") {
            self.next(); // 空 init，消费分号
            None
        } else {
            Some(Box::new(self.parse_stmt())) // 完整语句（含分号）
        };
        let cond = if self.is_op(0, ";") {
            self.next(); // 空 cond，消费分号
            None
        } else {
            let c = self.parse_expr();
            self.expect_op(";");
            Some(c)
        };
        // update 是完整语句（examples 写法 `k = k + 1;`）；`i++` 无分号由 Inc 分支兼容
        let update = if self.is_op(0, ")") { None } else { Some(Box::new(self.parse_stmt())) };
        self.expect_op(")");
        self.expect_op("{");
        let body = self.parse_stmts_until("}");
        Stmt::For { init, cond, update, body }
    }

    /// `&perm follow base name = &lvalue;` — 智能引用声明。
    fn parse_ref_decl(&mut self) -> Stmt {
        self.next(); // &
        let perm = self.expect_id();
        self.err_if(!PERMS.contains(&perm.as_str()),
                    format!("无效引用权限 '{perm}'（r/w/m 组合：r, w, m, rw, rm, wm, rwm）"));
        let follow = self.expect_id();
        self.err_if(!FOLLOWS.contains(&follow.as_str()),
                    format!("无效引用层级 '{follow}'（应为 u/f/a/t）"));
        let mut base = self.expect_id();
        self.eat_arr_suffix(&mut base);
        let name = self.expect_id();
        self.expect_op("=");
        let init = self.parse_expr();
        self.err_if(!matches!(init, Expr::RefOf(_)),
                    "引用声明要求 &<表达式> 初始化（如 &rw u int p = &a[0];）");
        self.expect_op(";");
        Stmt::RefDecl { perm, follow, base, name, init }
    }

    /// 标识符开头的语句：声明/赋值/自增/索引/属性/调用。
    fn parse_ident_stmt(&mut self) -> Stmt {
        let t = self.peek(0);

        // int[] a = expr; / int[] a;（类型名 + []）
        if is_type_name(t.text) && self.is_op(1, "[") && self.is_op(2, "]") {
            self.next(); self.next(); self.next();
            let name = self.expect_id();
            let value = if self.eat_op("=") { self.parse_expr() } else { Expr::Int(0) };
            self.expect_op(";");
            return Stmt::Assign {
                vtype: Some("int[]".into()), is_const: false, is_thread: false,
                target: AssignTarget::Var(name), op: "=".into(), value,
            };
        }
        // int x = e; / int x;（类型名 + 标识符）
        if is_type_name(t.text) && self.peek(1).kind == TokenKind::Ident {
            let mut ty = self.next().text.to_string();
            self.eat_arr_suffix(&mut ty);
            let name = self.expect_id();
            let value = if self.eat_op("=") { self.parse_expr() } else { Expr::Int(0) };
            self.expect_op(";");
            return Stmt::Assign {
                vtype: Some(ty), is_const: false, is_thread: false,
                target: AssignTarget::Var(name), op: "=".into(), value,
            };
        }
        // i++; / i--;
        if self.is_op(1, "++") || self.is_op(1, "--") {
            let name = self.next().text.to_string();
            let op = self.next().text.to_string();
            if !self.is_op(0, ";") {
                // for update 子句里无分号
                return Stmt::Inc { name, op };
            }
            self.next();
            return Stmt::Inc { name, op };
        }
        // a[i] = v; / a[i] += v; / a[i]; 索引
        if self.is_op(1, "[") {
            let arr = self.next().text.to_string();
            self.next(); // [
            let idx = self.parse_expr();
            self.expect_op("]");
            let target = AssignTarget::Index {
                base: Box::new(Expr::Var(arr.clone())),
                idx: Box::new(idx.clone()),
            };
            if self.is_op(0, "=") || ASSIGN_OPS.contains(&self.peek(0).text) {
                let op = self.next().text.to_string();
                let value = self.parse_expr();
                self.expect_op(";");
                return Stmt::Assign { vtype: None, is_const: false, is_thread: false, target, op, value };
            }
            self.expect_op(";");
            return Stmt::Expr(Expr::Index { base: Box::new(Expr::Var(arr)), idx: Box::new(idx) });
        }
        // qual::name(...); / qual::attr = v; / qual::attr += v;
        if self.is_op(1, "::") {
            let qual = self.next().text.to_string();
            self.next(); // ::
            let nm = self.expect_method_name();
            if self.is_op(0, "=") || ASSIGN_OPS.contains(&self.peek(0).text) {
                let op = self.next().text.to_string();
                let value = self.parse_expr();
                self.expect_op(";");
                return Stmt::Assign {
                    vtype: None, is_const: false, is_thread: false,
                    target: AssignTarget::Prop { base: Box::new(Expr::Var(qual)), name: nm },
                    op, value,
                };
            }
            self.expect_op("(");
            let mut args = Vec::new();
            while !self.is_op(0, ")") && !self.at_eof() {
                args.push(self.parse_expr());
                if !self.eat_op(",") {
                    break;
                }
            }
            self.expect_op(")");
            self.expect_op(";");
            return Stmt::Expr(Expr::Call { qual: Some(qual), name: nm, args });
        }
        // x = e; / x += e;
        if self.is_op(1, "=") || ASSIGN_OPS.contains(&self.peek(1).text) {
            let name = self.next().text.to_string();
            let op = self.next().text.to_string();
            let value = self.parse_expr();
            self.expect_op(";");
            return Stmt::Assign {
                vtype: None, is_const: false, is_thread: false,
                target: AssignTarget::Var(name), op, value,
            };
        }
        // fname(args); 裸调用语句
        if self.is_op(1, "(") {
            let name = self.next().text.to_string();
            self.next(); // (
            let mut args = Vec::new();
            while !self.is_op(0, ")") && !self.at_eof() {
                args.push(self.parse_expr());
                if !self.eat_op(",") {
                    break;
                }
            }
            self.expect_op(")");
            self.expect_op(";");
            return Stmt::Expr(Expr::Call { qual: None, name, args });
        }
        // 其他表达式语句
        let e = self.parse_expr();
        if !self.is_op(0, ";") && !self.is_op(0, ")") {
            self.error(format!("语句后期望 ';'，得到 '{}'", self.peek(0).text));
        } else {
            self.eat_op(";");
        }
        Stmt::Expr(e)
    }

    // ---- 表达式（Pratt 风格优先级） ----

    pub fn parse_expr(&mut self) -> Expr {
        self.parse_binop(0)
    }

    fn parse_binop(&mut self, min_bp: u8) -> Expr {
        let mut left = self.parse_unary();
        loop {
            let t = self.peek(0);
            if t.kind != TokenKind::Op {
                break;
            }
            let (bp, op) = match t.text {
                "||" => (1, "||"),
                "&&" => (2, "&&"),
                "==" | "!=" => (3, t.text),
                "<" | ">" | "<=" | ">=" => (4, t.text),
                "+" | "-" => (5, t.text),
                "*" | "/" | "%" => (6, t.text),
                _ => break,
            };
            if bp < min_bp {
                break;
            }
            self.next();
            let right = self.parse_binop(bp + 1);
            left = Expr::BinOp { op: op.to_string(), l: Box::new(left), r: Box::new(right) };
        }
        left
    }

    fn parse_unary(&mut self) -> Expr {
        let t = self.peek(0);
        if t.kind == TokenKind::Op && t.text == "&" {
            self.next();
            return Expr::RefOf(Box::new(self.parse_unary()));
        }
        self.parse_primary()
    }

    fn parse_primary(&mut self) -> Expr {
        let t = self.peek(0);
        match t.kind {
            TokenKind::Int => {
                self.next();
                let v = t.text.parse::<i64>().unwrap_or(0);
                self.parse_prop_chain(Expr::Int(v))
            }
            TokenKind::Float => {
                self.next();
                let v = t.text.parse::<f64>().unwrap_or(0.0);
                self.parse_prop_chain(Expr::Float(v))
            }
            TokenKind::Str => {
                self.next();
                self.parse_prop_chain(Expr::Str(t.text.to_string()))
            }
            TokenKind::Char => {
                self.next();
                self.parse_prop_chain(Expr::Char(decode_char(t.text)))
            }
            TokenKind::Keyword if t.text == "true" || t.text == "false" => {
                self.next();
                self.parse_prop_chain(Expr::Bool(t.text == "true"))
            }
            TokenKind::Keyword if t.text == "new" => {
                self.next();
                // new Type[expr] → 数组字面量；new Class(args) → Obj::new
                let cls = self.expect_id();
                if self.eat_op("[") {
                    let size = self.parse_expr();
                    self.expect_op("]");
                    self.parse_prop_chain(Expr::NewArray { ty: cls, size: Box::new(size) })
                } else {
                    self.expect_op("(");
                    let mut args = Vec::new();
                    while !self.is_op(0, ")") && !self.at_eof() {
                        args.push(self.parse_expr());
                        if !self.eat_op(",") {
                            break;
                        }
                    }
                    self.expect_op(")");
                    self.parse_prop_chain(Expr::New { cls, args })
                }
            }
            TokenKind::Keyword if t.text == "get" || t.text == "cause" => {
                // 前缀解包：get/cause X；`get(...)`/`cause(...)` 是裸调用（方法名 get/cause）
                let op = t.text;
                if self.is_op(1, "(") {
                    self.next();
                    self.next(); // (
                    let mut args = Vec::new();
                    while !self.is_op(0, ")") && !self.at_eof() {
                        args.push(self.parse_expr());
                        if !self.eat_op(",") {
                            break;
                        }
                    }
                    self.expect_op(")");
                    self.parse_prop_chain(Expr::Call { qual: None, name: op.to_string(), args })
                } else {
                    let is_prefix = !(self.is_op(1, "::") || self.is_op(1, ".") || self.is_op(1, "["));
                    if is_prefix {
                        self.next();
                        let l = self.parse_unary();
                        self.parse_prop_chain(Expr::Unwrap { op: op.to_string(), l: Box::new(l) })
                    } else {
                        // get 作普通标识符（变量名等）
                        self.next();
                        self.parse_prop_chain(Expr::Var(op.to_string()))
                    }
                }
            }
            TokenKind::Keyword if t.text == "this" => {
                self.next();
                self.parse_prop_chain(Expr::Var("this".to_string()))
            }
            TokenKind::Keyword => {
                self.next();
                self.parse_prop_chain(Expr::Var(t.text.to_string()))
            }
            TokenKind::Ident => {
                let name = self.next().text.to_string();
                if self.eat_op("::") {
                    let mname = self.expect_method_name();
                    if self.is_op(0, "(") {
                        self.next();
                        let mut args = Vec::new();
                        while !self.is_op(0, ")") && !self.at_eof() {
                            args.push(self.parse_expr());
                            if !self.eat_op(",") {
                                break;
                            }
                        }
                        self.expect_op(")");
                        self.parse_prop_chain(Expr::Call { qual: Some(name), name: mname, args })
                    } else {
                        // qual::name 属性访问（this::hp）
                        self.parse_prop_chain(Expr::Prop {
                            base: Box::new(Expr::Var(name)),
                            name: mname,
                        })
                    }
                } else if self.is_op(0, "(") {
                    // 裸调用
                    self.next();
                    let mut args = Vec::new();
                    while !self.is_op(0, ")") && !self.at_eof() {
                        args.push(self.parse_expr());
                        if !self.eat_op(",") {
                            break;
                        }
                    }
                    self.expect_op(")");
                    self.parse_prop_chain(Expr::Call { qual: None, name, args })
                } else if self.is_op(0, "[") {
                    // 索引读取
                    self.next();
                    let idx = self.parse_expr();
                    self.expect_op("]");
                    self.parse_prop_chain(Expr::Index { base: Box::new(Expr::Var(name)), idx: Box::new(idx) })
                } else {
                    self.parse_prop_chain(Expr::Var(name))
                }
            }
            TokenKind::Op if t.text == "(" => {
                self.next();
                let e = self.parse_expr();
                self.expect_op(")");
                self.parse_prop_chain(e)
            }
            TokenKind::Op if t.text == "-" => {
                // 一元负号（宽松支持；语法面未见，作扩展）
                self.next();
                let e = self.parse_unary();
                Expr::BinOp { op: "-".into(), l: Box::new(Expr::Int(0)), r: Box::new(e) }
            }
            _ => {
                self.error(format!("无法解析表达式 '{}'", t.text));
                self.next();
                Expr::Int(0)
            }
        }
    }

    /// 后缀属性链：`obj.field.field...`（对象属性访问）。
    fn parse_prop_chain(&mut self, mut e: Expr) -> Expr {
        while self.is_op(0, ".") {
            self.next();
            let t = self.peek(0);
            if t.kind != TokenKind::Ident && t.kind != TokenKind::Keyword {
                self.error(format!("无效属性名 '{}'", t.text));
                self.next();
                break;
            }
            let name = self.next().text.to_string();
            e = Expr::Prop { base: Box::new(e), name };
        }
        e
    }
}

/// 字符字面量解码：`'x'` / `'\n'` / `'\\'` / `'\''` 等。
fn decode_char(s: &str) -> u8 {
    match s {
        "\\n" => b'\n',
        "\\t" => b'\t',
        "\\r" => b'\r',
        "\\0" => 0,
        "\\\\" => b'\\',
        "\\'" => b'\'',
        "\\\"" => b'"',
        _ => s.as_bytes().first().copied().unwrap_or(0),
    }
}

/// 便捷入口：源码 → Program。errors 非空时解析失败。
pub fn parse_source(src: &str) -> (Program, Vec<ParseError>) {
    let mut toks = Vec::new();
    match crate::lexer::tokenize(src, &mut toks) {
        Ok(()) => {}
        Err(e) => {
            let prog = Program { kind: String::new(), decls: Vec::new(), main: None };
            return (prog, vec![ParseError { line: e.line, col: e.col, msg: e.msg.to_string() }]);
        }
    }
    let mut p = Parser::new(&toks);
    let prog = p.parse_program();
    (prog, p.errors)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_ok(src: &str) -> Program {
        let (p, errs) = parse_source(src);
        assert!(errs.is_empty(), "parse errors: {errs:?}");
        p
    }

    #[test]
    fn hello() {
        let p = parse_ok("program main;\nMain { void exec() { CIO::println(\"hi\"); } }");
        assert_eq!(p.kind, "main");
        let m = p.main.unwrap();
        assert_eq!(m.methods[0].name, "exec");
        assert!(matches!(m.methods[0].body[0], Stmt::Expr(Expr::Call { ref qual, .. }) if qual.as_deref() == Some("CIO")));
    }

    #[test]
    fn requests_model() {
        let src = r#"
program main;
Stream Calc { int add(a int, b int); }
Calc MyCalc { int add(a int, b int) { res a + b; } }
Main {
  void exec() {
    ALL r = MyCalc::add(3, 4);
    ALL bad = MyCalc::div(1, 0);
    CIO::println("x", get r, cause bad);
    if (r) { CIO::println("ok"); } else { CIO::println("no"); }
  }
}"#;
        let p = parse_ok(src);
        assert_eq!(p.decls.len(), 2);
        let m = p.main.unwrap();
        assert!(matches!(m.methods[0].body[0], Stmt::Assign { vtype: Some(ref v), .. } if v == "ALL"));
        assert!(matches!(m.methods[0].body[2], Stmt::Expr(Expr::Call { ref args, .. }) if args.len() == 3));
    }

    #[test]
    fn control_flow() {
        let src = r#"
program main;
Main {
  void exec() {
    ALL i = 1;
    while (i <= 10) { i = i + 1; }
    for (ALL k = 1; k <= 5; k = k + 1;) { }
    for (;;) { break; }
    if (i > 5) { } else if (i < 2) { } else { }
  }
}"#;
        let p = parse_ok(src);
        let m = p.main.unwrap();
        assert!(matches!(m.methods[0].body[2], Stmt::For { .. }));
        assert!(matches!(m.methods[0].body[3], Stmt::For { cond: None, .. }));
    }

    #[test]
    fn smart_refs_and_threads() {
        let src = r#"
program main;
Calc Worker {
  void threadJob(n int) {
    thread int local_note = 0;
    &w a int wa = &local_note;
    Ref::write(wa, n * 2);
    res get Ref::read(ra);
  }
}
Main {
  void exec() {
    int counter = 0;
    &rwm t int p = &counter;
    ALL t1 = get Threads::spawn("factorial", 10);
  }
}"#;
        let p = parse_ok(src);
        assert_eq!(p.decls.len(), 1);
    }

    #[test]
    fn classes_fields_and_arrays() {
        let src = r#"
program main;
Class Hero {
  void __init__(name string, hp int) { Obj::set(this, "name", name); }
  int getHp() { res 100; }
}
Main {
  void exec() {
    ALL h = new Hero("TAK", 88);
    int[] a = Solid::new().res;
    a[0] = 1;
    ALL x = a[0];
    CIO::println("hp =", h.hp);
  }
}"#;
        let p = parse_ok(src);
        let m = p.main.unwrap();
        let exec = &m.methods[0].body;
        assert!(matches!(exec[1], Stmt::Assign { vtype: Some(ref v), .. } if v == "int[]"));
        // a[0] = 1; → Assign(target: Index)
        assert!(matches!(exec[2], Stmt::Assign { target: AssignTarget::Index { .. }, .. }));
        // ALL x = a[0]; → 右侧是 Index
        assert!(matches!(exec[3], Stmt::Assign { value: Expr::Index { .. }, .. }));
    }

    #[test]
    fn need_and_binary_stream() {
        let src = r#"
program main;
need value GREETING;
need function greet;
need stream IO;
Stream m & "libm.so.6" { double sin(x double); }
Main { void exec() { } }
"#;
        let p = parse_ok(src);
        assert_eq!(p.decls.len(), 4);
        assert!(matches!(p.decls[3], Decl::StreamBin { ref file, .. } if file == "libm.so.6"));
    }

    #[test]
    fn annotations_and_phonebooth() {
        let src = r#"
program main;
Class Hero { void __init__() { } } @unfork
Main {
  void exec() {
  }
  int fast() { res 1; } @call
}"#;
        let p = parse_ok(src);
        assert!(matches!(p.decls[0], Decl::Class { ref annos, .. } if annos == &["unfork"]));
        let m = p.main.unwrap();
        assert_eq!(m.methods[1].annos, vec!["call"]);
    }

    #[test]
    fn new_array_literal() {
        let src = r#"
program main;
Main { void exec() { ALL a = new int[10]; ALL b = new Hero[3]; } }
"#;
        let p = parse_ok(src);
        let m = p.main.unwrap();
        assert!(matches!(m.methods[0].body[0], Stmt::Assign { value: Expr::NewArray { ref ty, .. }, .. } if ty == "int"));
        assert!(matches!(m.methods[0].body[1], Stmt::Assign { value: Expr::NewArray { ref ty, .. }, .. } if ty == "Hero"));
    }

    #[test]
    fn multi_value_res() {
        let src = r#"
program main;
Main { void exec() { } int pair() { res 1, 2, 3; } }
"#;
        let p = parse_ok(src);
        let m = p.main.unwrap();
        match &m.methods[1].body[0] {
            Stmt::Ret { kind: RetKind::Res, values } => assert_eq!(values.len(), 3),
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn parse_error_reported() {
        let (_p, errs) = parse_source("program main;\nMain { void exec() { x = ; } }");
        assert!(!errs.is_empty());
        assert!(errs[0].line >= 1);
    }

    #[test]
    fn generic_type_member() {
        let src = r#"
program main;
Class Box {
  type T;
  T n;
  T[] a;
  T get() { res n; }
}
Main { void exec() { } }
"#;
        let p = parse_ok(src);
        let cls = &p.decls[0];
        if let Decl::Class { members, .. } = cls {
            // type T; 只注册名字不产生成员：T n; / T[] a; / T get() = 3 个
            assert_eq!(members.len(), 3);
            assert!(matches!(members[0], Member::Field { ref ty, .. } if ty == "T"));
            assert!(matches!(members[1], Member::Field { ref ty, .. } if ty == "T[]"));
            assert!(matches!(members[2], Member::Method(Method { ref ret, .. }) if ret == "T"));
        } else {
            panic!("expected class");
        }
    }
}
