//! bbb-llvm — BioLang 编译器（M4 里程碑）。
//!
//! 方案（对齐旧 src/llvm.c）：**零依赖**发射 LLVM IR 文本，交给系统 clang
//! 编译成原生可执行文件。统一 double 语义在解释器与编译器间保持一致。
//!
//! M4 v1 支持子集（17-llvm.bio 为准）：
//! - int/float/double 变量声明与赋值、算术 + - * / %、比较 == != < > <= >=
//! - if/else、while、for（含 break/continue）
//! - 方法定义与调用（含 Main::exec 外的方法，如 square）、递归 v2
//! - res 返回值 / ref（打印后退出）、get/cause、ALL 声明
//! - CIO::println / CIO::print（字符串+数字混合 → printf）

use std::collections::HashMap;

use bbb_syntax::ast::*;

/// 值类型。
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Ty {
    I64,
    F64,
}

impl Ty {
    fn llvm(self) -> &'static str {
        match self {
            Ty::I64 => "i64",
            Ty::F64 => "double",
        }
    }
}

/// 编译产物。
pub struct Module {
    pub ir: String,
}

struct Ctx {
    out: String,
    tmp: usize,
    str_i: usize,
    vars: Vec<HashMap<String, (Ty, String)>>, // 作用域栈：变量名 → (类型, 寄存器名)
    funcs: HashMap<String, (Ty, Vec<(String, Ty)>)>, // 方法表：名 → (返回类型, 参数)
    labels: usize,
    loop_end: Vec<String>,
    loop_continue: Vec<String>, // continue 目标（for=update，while=cond）
    current_ret: Ty,
    str_list: String, // 字符串常量延迟输出（函数外）
}

impl Ctx {
    fn new() -> Self {
        Ctx {
            out: String::new(),
            tmp: 0,
            str_i: 0,
            vars: vec![HashMap::new()],
            funcs: HashMap::new(),
            labels: 0,
            loop_end: Vec::new(),
            loop_continue: Vec::new(),
            current_ret: Ty::I64,
            str_list: String::new(),
        }
    }

    fn emit(&mut self, s: impl AsRef<str>) {
        self.out.push_str(s.as_ref());
        self.out.push('\n');
    }

    fn reg(&mut self, hint: &str) -> String {
        self.tmp += 1;
        format!("%{hint}{}", self.tmp)
    }

    fn label(&mut self, hint: &str) -> String {
        self.labels += 1;
        format!("{hint}{}", self.labels)
    }

    fn str_const(&mut self, s: &str) -> String {
        // C 风格转义
        let mut esc = String::new();
        for b in s.bytes() {
            match b {
                b'"' => esc.push_str("\\22"),
                b'\\' => esc.push_str("\\5C"),
                b'\n' => esc.push_str("\\0A"),
                b'\t' => esc.push_str("\\09"),
                0x20..=0x7E => esc.push(b as char),
                _ => esc.push_str(&format!("\\{:02X}", b)),
            }
        }
        self.str_i += 1;
        let name = format!("@.str{}", self.str_i);
        // LLVM 字节数：\XX 转义序列计 1 字节，其余字符 1 字节
        let mut llvm_len = 0usize;
        let chars: Vec<char> = esc.chars().collect();
        let mut k = 0;
        while k < chars.len() {
            if chars[k] == '\\' && k + 2 < chars.len() {
                llvm_len += 1;
                k += 3;
            } else {
                llvm_len += 1;
                k += 1;
            }
        }
        self.str_list.push_str(&format!("{name} = private unnamed_addr constant [{} x i8] c\"{esc}\\00\", align 1\n",
                          llvm_len + 1));
        name
    }

    fn var_get(&self, name: &str) -> Option<(Ty, String)> {
        for scope in self.vars.iter().rev() {
            if let Some(v) = scope.get(name) {
                return Some(v.clone());
            }
        }
        None
    }

    fn var_set(&mut self, name: &str, ty: Ty, reg: String) {
        self.vars.last_mut().unwrap().insert(name.to_string(), (ty, reg));
    }
}

/// 编译 Program → IR 文本。
pub fn compile(prog: &Program) -> Result<String, String> {
    let mut ctx = Ctx::new();
    ctx.emit("; BioLang LLVM backend (M4) — generated from AST");
    ctx.emit("declare i32 @printf(ptr, ...)");
    ctx.emit("declare void @exit(i32)");
    ctx.emit("");

    // 收集方法签名
    collect_methods(prog, &mut ctx);

    // 编译 Main 流方法（exec 之外的方法作为普通函数）
    if let Some(m) = &prog.main {
        for method in &m.methods {
            if method.name != "exec" {
                let fname = format!("@main_{}", method.name);
                compile_function(&mut ctx, &fname, method)?;
            }
        }
    }
    // 编译 fork/class 方法（@<流名>_<方法名>）
    for d in &prog.decls {
        if let (Decl::Class { name, members, .. } | Decl::Fork { name, members, .. }) = d {
            for mem in members {
                if let Member::Method(m2) = mem {
                    let fname = format!("@{}_{}", name, m2.name);
                    compile_function(&mut ctx, &fname, m2)?;
                }
            }
        }
    }
    // Main::exec → @__bio_main
    if let Some(m) = &prog.main {
        if let Some(exec) = m.methods.iter().find(|x| x.name == "exec") {
            let mut e2 = exec.clone();
            e2.name = "__bio_main".into();
            e2.ret = "void".into();
            compile_function(&mut ctx, "@__bio_main", &e2)?;
        }
    }
    // 字符串常量（函数外）
    if !ctx.str_list.is_empty() {
        ctx.out.push_str(&ctx.str_list);
        ctx.out.push('\n');
    }
    // C main 包装
    ctx.emit("define i32 @main(i32 %argc, ptr %argv) {");
    ctx.emit("entry:");
    ctx.emit("  call void @__bio_main()");
    ctx.emit("  ret i32 0");
    ctx.emit("}");
    Ok(ctx.out)
}

fn collect_methods(prog: &Program, ctx: &mut Ctx) {
    let mut methods: Vec<(String, Ty, Vec<(String, Ty)>)> = Vec::new();
    if let Some(m) = &prog.main {
        for m2 in &m.methods {
            if m2.name != "exec" {
                let (rt, params) = sig_of(m2);
                methods.push((format!("main_{}", m2.name), rt, params));
            }
        }
    }
    for d in &prog.decls {
        if let (Decl::Class { name, members, .. } | Decl::Fork { name, members, .. }) = d {
            for mem in members {
                if let Member::Method(m2) = mem {
                    let (rt, params) = sig_of(m2);
                    methods.push((format!("{}_{}", name, m2.name), rt, params));
                }
            }
        }
    }
    for (n, rt, params) in methods {
        ctx.funcs.insert(n, (rt, params));
    }
}

fn decl_name(d: &Decl) -> String {
    match d {
        Decl::Class { name, .. } => name.clone(),
        _ => "s".into(),
    }
}

fn sig_of(m: &Method) -> (Ty, Vec<(String, Ty)>) {
    let rt = ty_of(&m.ret);
    let params = m
        .params
        .iter()
        .map(|p| (p.name.clone(), ty_of(&p.ty)))
        .collect();
    (rt, params)
}

fn ty_of(t: &str) -> Ty {
    match t {
        "float" | "double" => Ty::F64,
        _ => Ty::I64,
    }
}

fn compile_function(ctx: &mut Ctx, fname: &str, method: &Method) -> Result<(), String> {
    let (rt, params) = sig_of(method);
    ctx.current_ret = rt;
    let mut sig = format!("define {} {fname}(", rt.llvm());
    let mut body_sig = Vec::new();
    for (i, (n, t)) in params.iter().enumerate() {
        if i > 0 {
            sig.push_str(", ");
        }
        let arg = format!("%{}", n);
        sig.push_str(&format!("{} {arg}", t.llvm()));
        body_sig.push((n.clone(), *t, arg));
    }
    sig.push(')');
    ctx.emit(sig);
    ctx.emit("{");
    ctx.emit("entry:");
    // 参数 alloc + store
    ctx.vars.push(HashMap::new());
    for (n, t, arg) in body_sig {
        let alloca = ctx.reg(&format!("{}_", n));
        ctx.emit(format!("  {alloca} = alloca {}", t.llvm()));
        ctx.emit(format!("  store {} {arg}, ptr {alloca}", t.llvm()));
        ctx.var_set(&n, t, alloca);
    }
    // 语句
    let (flow, ret_reg) = compile_block(ctx, &method.body)?;
    if let Some(r) = ret_reg {
        ctx.emit(format!("  ret {} {r}", rt.llvm()));
    } else if !flow {
        ctx.emit(format!("  ret {} {}", rt.llvm(), if rt == Ty::I64 { "0" } else { "0.0" }));
    }
    ctx.emit("}");
    ctx.emit("");
    ctx.vars.pop();
    Ok(())
}

/// 语句块编译结果。
struct BlockOut {
    /// 最后一个 res 的寄存器（Some = 函数已 ret 前）
    ret: Option<String>,
}

/// 编译语句块，返回 (是否以终止指令结束, res 寄存器)。
/// 块内 break/continue/ret 后不再编译后续语句（不可达丢弃，LLVM 合法）。
fn compile_block(ctx: &mut Ctx, stmts: &[Stmt]) -> Result<(bool, Option<String>), String> {
    let mut ret = None;
    for st in stmts {
        match st {
            Stmt::Ret { kind, values } => {
                if *kind == RetKind::Ref {
                    // ref：拒绝 → 打印消息后退出（消息表达式 v2；先直接 exit）
                    ctx.emit("  call void @exit(i32 1)");
                    ctx.emit("  unreachable");
                    return Ok((true, None));
                } else if let Some(v) = values.first() {
                    let (ty, val) = compile_expr(ctx, v)?;
                    ctx.current_ret = ty;
                    return Ok((true, Some(val)));
                }
            }
            Stmt::Expr(e) => {
                // 调用语句：CIO::println 等
                compile_call_stmt(ctx, e)?;
            }
            Stmt::Assign { vtype, target, op, value, .. } => {
                if let AssignTarget::Var(name) = target {
                    let (vty, vval) = compile_expr(ctx, value)?;
                    let _ = op;
                    match ctx.var_get(name) {
                        Some((t, reg)) => {
                            let cast = coerce(ctx, vval, vty, t);
                            ctx.emit(format!("  store {} {cast}, ptr {reg}", t.llvm()));
                        }
                        None => {
                            let reg = ctx.reg(&format!("{}_", name));
                            let alloca_t = if vtype.as_deref() == Some("ALL") || vtype.is_none() { vty } else { ty_of(vtype.as_deref().unwrap_or("int")) };
                            let alloca = ctx.reg(&format!("{}a_", name));
                            ctx.emit(format!("  {alloca} = alloca {}", alloca_t.llvm()));
                            let cast = coerce(ctx, vval, vty, alloca_t);
                            ctx.emit(format!("  store {} {cast}, ptr {alloca}", alloca_t.llvm()));
                            let _ = reg;
                            ctx.var_set(name, alloca_t, alloca);
                        }
                    }
                }
            }
            Stmt::If { cond, then, els } => {
                let (_, cval) = compile_expr(ctx, cond)?;
                let then_l = ctx.label("then");
                let else_l = ctx.label("else");
                let end_l = ctx.label("endif");
                let c = ctx.reg("c");
                ctx.emit(format!("  {c} = icmp ne i64 {cval}, 0"));
                ctx.emit(format!("  br i1 {c}, label %{then_l}, label %{else_l}"));
                ctx.emit(format!("{then_l}:"));
                let (t1, r1) = compile_block(ctx, then)?;
                if !t1 && r1.is_none() {
                    ctx.emit(format!("  br label %{end_l}"));
                }
                ctx.emit(format!("{else_l}:"));
                if let Some(e) = els {
                    let (t2, r2) = compile_block(ctx, e)?;
                    if !t2 && r2.is_none() {
                        ctx.emit(format!("  br label %{end_l}"));
                    }
                } else {
                    ctx.emit(format!("  br label %{end_l}"));
                }
                ctx.emit(format!("{end_l}:"));
            }
            Stmt::While { cond, body } => {
                let cond_l = ctx.label("wcond");
                let body_l = ctx.label("wbody");
                let end_l = ctx.label("wend");
                ctx.emit(format!("  br label %{cond_l}"));
                ctx.emit(format!("{cond_l}:"));
                let (_, cval) = compile_expr(ctx, cond)?;
                let c = ctx.reg("c");
                ctx.emit(format!("  {c} = icmp ne i64 {cval}, 0"));
                ctx.emit(format!("  br i1 {c}, label %{body_l}, label %{end_l}"));
                ctx.emit(format!("{body_l}:"));
                ctx.loop_end.push(end_l.clone());
                ctx.loop_continue.push(cond_l.clone());
                let (flow, r) = compile_block(ctx, body)?;
                ctx.loop_end.pop();
                ctx.loop_continue.pop();
                if !flow {
                    ctx.emit(format!("  br label %{cond_l}"));
                }
                if let Some(r) = r {
                    ret = Some(r);
                }
                ctx.emit(format!("{end_l}:"));
            }
            Stmt::For { init, cond, update, body } => {
                if let Some(i) = init {
                    compile_block(ctx, std::slice::from_ref(i))?;
                }
                let cond_l = ctx.label("fcond");
                let body_l = ctx.label("fbody");
                let upd_l = ctx.label("fupd");
                let end_l = ctx.label("fend");
                ctx.emit(format!("  br label %{cond_l}"));
                ctx.emit(format!("{cond_l}:"));
                if let Some(c) = cond {
                    let (_, cval) = compile_expr(ctx, c)?;
                    let c = ctx.reg("c");
                    ctx.emit(format!("  {c} = icmp ne i64 {cval}, 0"));
                    ctx.emit(format!("  br i1 {c}, label %{body_l}, label %{end_l}"));
                } else {
                    ctx.emit(format!("  br label %{body_l}"));
                }
                ctx.emit(format!("{body_l}:"));
                ctx.loop_end.push(end_l.clone());
                ctx.loop_continue.push(upd_l.clone());
                let (flow, r) = compile_block(ctx, body)?;
                ctx.loop_end.pop();
                ctx.loop_continue.pop();
                if let Some(r) = r {
                    ret = Some(r);
                }
                if !flow {
                    ctx.emit(format!("  br label %{upd_l}"));
                }
                ctx.emit(format!("{upd_l}:"));
                if let Some(u) = update {
                    compile_block(ctx, std::slice::from_ref(u))?;
                }
                ctx.emit(format!("  br label %{cond_l}"));
                ctx.emit(format!("{end_l}:"));
            }
            Stmt::Break => {
                if let Some(e) = ctx.loop_end.last() {
                    ctx.emit(format!("  br label %{e}"));
                    return Ok((true, None));
                }
            }
            Stmt::Continue => {
                if let Some(c) = ctx.loop_continue.last() {
                    ctx.emit(format!("  br label %{c}"));
                    return Ok((true, None));
                }
            }
            Stmt::Inc { name, op } => {
                if let Some((t, reg)) = ctx.var_get(name) {
                    let delta = if op == "++" { 1 } else { -1 };
                    let t1 = ctx.reg("inc");
                    ctx.emit(format!("  {t1} = load {}, ptr {reg}", t.llvm()));
                    let t2 = ctx.reg("inc");
                    ctx.emit(format!("  {t2} = add {} {t1}, {delta}", t.llvm()));
                    ctx.emit(format!("  store {} {t2}, ptr {reg}", t.llvm()));
                }
            }
            Stmt::RefDecl { .. } => {}
        }
    }
    Ok((false, ret))
}

/// 调用语句：CIO::println/print → printf。
fn compile_call_stmt(ctx: &mut Ctx, e: &Expr) -> Result<(), String> {
    if let Expr::Call { qual, name, args } = e {
        if qual.as_deref() == Some("CIO") || qual.as_deref() == Some("IO") {
            if name == "println" || name == "print" {
                return emit_printf(ctx, args, name == "println");
            }
        }
    }
    Ok(())
}

fn emit_printf(ctx: &mut Ctx, args: &[Expr], newline: bool) -> Result<(), String> {
    // 拼格式串：字符串参数原样（转义 %），数字参数 → %ld / %lf
    let mut fmt = String::new();
    let mut call_args: Vec<(Ty, String)> = Vec::new();
    // println 参数空格分隔（print 直接拼接——与解释器一致）
    let sep = if newline { " " } else { "" };
    let mut first = true;
    for a in args {
        if !first {
            fmt.push_str(sep);
        }
        first = false;
        match a {
            Expr::Str(s) => {
                fmt.push_str(&s.replace('%', "%%"));
            }
            other => {
                let (ty, val) = compile_expr(ctx, other)?;
                match ty {
                    Ty::I64 => fmt.push_str("%ld"),
                    Ty::F64 => fmt.push_str("%lf"),
                }
                call_args.push((ty, val));
            }
        }
    }
    if newline {
        fmt.push('\n'); // 真换行字节，由 str_const 转义成 \0A
    }
    let sc = ctx.str_const(&fmt);
    let mut call = format!("  call i32 (ptr, ...) @printf(ptr {sc}");
    for (ty, val) in &call_args {
        call.push_str(&format!(", {} {}", ty.llvm(), val));
    }
    call.push(')');
    ctx.emit(call);
    Ok(())
}

fn coerce(ctx: &mut Ctx, val: String, from: Ty, to: Ty) -> String {
    if from == to {
        return val;
    }
    let r = ctx.reg("cv");
    match (from, to) {
        (Ty::I64, Ty::F64) => ctx.emit(format!("  {r} = sitofp i64 {val} to double")),
        (Ty::F64, Ty::I64) => ctx.emit(format!("  {r} = fptosi double {val} to i64")),
        _ => return val,
    }
    r
}

fn compile_expr(ctx: &mut Ctx, e: &Expr) -> Result<(Ty, String), String> {
    match e {
        Expr::Int(v) => Ok((Ty::I64, v.to_string())),
        Expr::Float(v) => Ok((Ty::F64, format!("{v:.17}"))),
        Expr::Var(name) => match ctx.var_get(name) {
            Some((t, reg)) => {
                let r = ctx.reg("v");
                ctx.emit(format!("  {r} = load {}, ptr {reg}", t.llvm()));
                Ok((t, r))
            }
            None => Err(format!("未定义变量 {name}")),
        },
        Expr::Unwrap { op, l } => {
            let (t, v) = compile_expr(ctx, l)?;
            let _ = op;
            Ok((t, v)) // get/cause 单值语义
        }
        Expr::BinOp { op, l, r } => {
            let (lt, lv) = compile_expr(ctx, l)?;
            let (rt, rv) = compile_expr(ctx, r)?;
            let ty = if lt == Ty::F64 || rt == Ty::F64 { Ty::F64 } else { Ty::I64 };
            let lv = coerce(ctx, lv, lt, ty);
            let rv = coerce(ctx, rv, rt, ty);
            let out = ctx.reg("b");
            match op.as_str() {
                "+" | "-" | "*" | "/" | "%" => {
                    let opc = match (op.as_str(), ty) {
                        ("+", Ty::I64) => "add", ("+", Ty::F64) => "fadd",
                        ("-", Ty::I64) => "sub", ("-", Ty::F64) => "fsub",
                        ("*", Ty::I64) => "mul", ("*", Ty::F64) => "fmul",
                        ("/", Ty::I64) => "sdiv", ("/", Ty::F64) => "fdiv",
                        ("%", Ty::I64) => "srem", ("%", Ty::F64) => "frem",
                        _ => "add",
                    };
                    ctx.emit(format!("  {out} = {opc} {} {lv}, {rv}", ty.llvm()));
                    Ok((ty, out))
                }
                "==" | "!=" | "<" | ">" | "<=" | ">=" => {
                    let cond_op = match op.as_str() {
                        "==" => "eq", "!=" => "ne", "<" => "slt", ">" => "sgt",
                        "<=" => "sle", ">=" => "sge", _ => "eq",
                    };
                    let cmp_op = if ty == Ty::F64 {
                        match cond_op {
                            "eq" => "oeq", "ne" => "one", "slt" => "olt", "sgt" => "ogt",
                            "sle" => "ole", "sge" => "oge", _ => "oeq",
                        }
                    } else {
                        cond_op
                    };
                    if ty == Ty::F64 {
                        let t1 = ctx.reg("cmp");
                        ctx.emit(format!("  {t1} = fcmp {cmp_op} double {lv}, {rv}"));
                        ctx.emit(format!("  {out} = zext i1 {t1} to i64"));
                    } else {
                        let t1 = ctx.reg("cmp");
                        ctx.emit(format!("  {t1} = icmp {cmp_op} i64 {lv}, {rv}"));
                        ctx.emit(format!("  {out} = zext i1 {t1} to i64"));
                    }
                    Ok((Ty::I64, out))
                }
                _ => Err(format!("不支持的运算符 {op}")),
            }
        }
        Expr::Call { qual, name, args } => {
            // 内部方法调用；函数名 = <owner>_<name>（qual 指定或裸调用回退 main_/全局）
            let key = if let Some(q) = qual {
                format!("{q}_{name}")
            } else if ctx.funcs.contains_key(name) {
                name.clone()
            } else {
                format!("main_{name}")
            };
            let fname = format!("@{key}");
            let (rt, _) = ctx
                .funcs
                .get(&key)
                .cloned()
                .unwrap_or((Ty::I64, vec![]));
            if !ctx.funcs.contains_key(&key) {
                // 未知方法：拒绝（printf 消息 + exit）——后端子集边界
                let msg = ctx.str_const(&format!("stream {key} refuses: no method {name}\n"));
                ctx.emit(format!("  call i32 (ptr, ...) @printf(ptr {msg})"));
                ctx.emit("  call void @exit(i32 1)");
                ctx.emit("  unreachable");
                return Ok((rt, "0".into()));
            }
            let call_reg = ctx.reg("call");
            let mut call = format!("  {call_reg} = call {} {fname}(", rt.llvm());
            for (i, a) in args.iter().enumerate() {
                let (at, av) = compile_expr(ctx, a)?;
                let _ = at;
                if i > 0 {
                    call.push_str(", ");
                }
                call.push_str(&format!("{} {av}", rt.llvm()));
            }
            call.push(')');
            ctx.emit(call);
            let _ = qual;
            Ok((rt, call_reg))
        }
        _ => Err(format!("LLVM 后端暂不支持该表达式: {e:?}")),
    }
}

// 占位（保持模块结构）
