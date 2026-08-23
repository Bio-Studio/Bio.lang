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
//! - Class + new（对象 = malloc struct，this 指针传参，__init__ 自动调用，
//!   this::字段 GEP 读写，对象方法调用）——2026-08-23 扩展

use std::collections::HashMap;

use bbb_syntax::ast::*;

/// 值类型。
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Ty {
    I64,
    F64,
    Ptr, // 对象/流指针
}

impl Ty {
    fn llvm(self) -> &'static str {
        match self {
            Ty::I64 => "i64",
            Ty::F64 => "double",
            Ty::Ptr => "ptr",
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
    void_methods: std::collections::HashSet<String>, // void 方法集合（调用时用 call void）
    classes: HashMap<String, Vec<(String, Ty)>>,     // 类：名 → 字段列表（名, 类型）
    var_ty: HashMap<String, String>,                 // 变量名 → 类名（对象变量，属性访问用）
    current_class: Option<String>,                   // 当前编译的类名（this:: 属性用）
    this_reg: Option<String>,                        // 当前方法 this 指针寄存器（类方法）
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
            void_methods: std::collections::HashSet::new(),
            classes: HashMap::new(),
            var_ty: HashMap::new(),
            current_class: None,
            this_reg: None,
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
    ctx.emit("declare ptr @malloc(i64)");
    ctx.emit("");

    // 收集类字段表（先于类型声明）
    collect_classes(prog, &mut ctx);
    // 发射 struct 类型声明
    let class_names: Vec<String> = ctx.classes.keys().cloned().collect();
    for cname in &class_names {
        let fields = &ctx.classes[cname];
        let mut ty = String::from("type {");
        for (i, (_n, t)) in fields.iter().enumerate() {
            if i > 0 {
                ty.push_str(", ");
            }
            ty.push_str(t.llvm());
        }
        if fields.is_empty() {
            ty.push_str("i8");
        }
        ty.push('}');
        ctx.emit(format!("%struct.{cname} = {ty}"));
    }
    if !ctx.classes.is_empty() {
        ctx.emit("");
    }

    // 收集方法签名
    collect_methods(prog, &mut ctx);

    // 编译 Main 流方法（exec 之外的方法作为普通函数）
    if let Some(m) = &prog.main {
        for method in &m.methods {
            if method.name != "exec" {
                let fname = format!("@main_{}", method.name);
                compile_function(&mut ctx, &fname, method, false)?;
            }
        }
    }
    // 编译 fork/class 方法（@<流名>_<方法名>，类方法首参为 this 指针）
    for d in &prog.decls {
        if let (Decl::Class { name, members, .. } | Decl::Fork { name, members, .. }) = d {
            for mem in members {
                if let Member::Method(m2) = mem {
                    let fname = format!("@{}_{}", name, m2.name);
                    let is_class = ctx.classes.contains_key(name);
                    compile_function(&mut ctx, &fname, m2, is_class)?;
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
            compile_function(&mut ctx, "@__bio_main", &e2, false)?;
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
            let is_class = ctx.classes.contains_key(name);
            for mem in members {
                if let Member::Method(m2) = mem {
                    let (rt, mut params) = sig_of(m2);
                    if is_class {
                        // 类方法：首参 this 指针
                        params.insert(0, ("this".into(), Ty::Ptr));
                    }
                    methods.push((format!("{}_{}", name, m2.name), rt, params));
                }
            }
        }
    }
    for (n, rt, params) in methods {
        ctx.funcs.insert(n.clone(), (rt, params));
        let _ = &n;
    }
    // void 方法登记（从原始方法表再扫一遍）
    for d in &prog.decls {
        if let (Decl::Class { name, members, .. } | Decl::Fork { name, members, .. }) = d {
            for mem in members {
                if let Member::Method(m2) = mem {
                    if m2.ret == "void" {
                        ctx.void_methods.insert(format!("{}_{}", name, m2.name));
                    }
                }
            }
        }
    }
    if let Some(m) = &prog.main {
        for m2 in &m.methods {
            if m2.ret == "void" && m2.name != "exec" {
                ctx.void_methods.insert(format!("main_{}", m2.name));
            }
        }
    }
}

fn decl_name(d: &Decl) -> String {
    match d {
        Decl::Class { name, .. } => name.clone(),
        _ => "s".into(),
    }
}

/// 收集类字段表：类名 → [(字段名, 类型)]（new/this:: 用）。
fn collect_classes(prog: &Program, ctx: &mut Ctx) {
    for d in &prog.decls {
        if let Decl::Class { name, members, .. } = d {
            let mut fields = Vec::new();
            for mem in members {
                if let Member::Field { ty, names } = mem {
                    for n in names {
                        fields.push((n.clone(), ty_of(ty)));
                    }
                }
            }
            ctx.classes.insert(name.clone(), fields);
        }
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
        "int" | "char" | "bool" => Ty::I64,
        "void" => Ty::I64, // void 方法：返回 i64 0（调用方忽略）
        _ => Ty::Ptr, // 类名/流名/对象 → 指针
    }
}

fn compile_function(ctx: &mut Ctx, fname: &str, method: &Method, is_class: bool) -> Result<(), String> {
    // 类方法：记录当前类名（this:: 属性访问）
    let saved_class = ctx.current_class.clone();
    if is_class {
        if let Some(cname) = fname.strip_prefix('@').and_then(|f| f.split('_').next()) {
            ctx.current_class = Some(cname.to_string());
        }
    }
    let r = compile_function_inner(ctx, fname, method, is_class);
    ctx.current_class = saved_class;
    r
}

fn compile_function_inner(ctx: &mut Ctx, fname: &str, method: &Method, is_class: bool) -> Result<(), String> {
    let (rt, params) = sig_of(method);
    ctx.current_ret = rt;
    let mut sig = format!("define {} {fname}(", rt.llvm());
    let mut body_sig = Vec::new();
    let mut first = true;
    // 类方法：首参为 this 指针（隐含）
    if is_class {
        sig.push_str("ptr %this");
        first = false;
    }
    for (i, (n, t)) in params.iter().enumerate() {
        if !first {
            sig.push_str(", ");
        }
        first = false;
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
    if is_class {
        let alloca = ctx.reg("this_");
        ctx.emit(format!("  {alloca} = alloca ptr"));
        ctx.emit(format!("  store ptr %this, ptr {alloca}"));
        ctx.this_reg = Some(alloca.clone());
    }
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
        ctx.emit(format!("  ret {} {}", rt.llvm(), if rt == Ty::I64 { "0" } else if rt == Ty::F64 { "0.0" } else { "null" }));
    }
    ctx.emit("}");
    ctx.emit("");
    ctx.vars.pop();
    ctx.this_reg = None;
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
                            let alloca_t = if vtype.as_deref() == Some("ALL") || vtype.is_none() { vty } else { ty_of(vtype.as_deref().unwrap_or("int")) };
                            let alloca = ctx.reg(&format!("{}a_", name));
                            ctx.emit(format!("  {alloca} = alloca {}", alloca_t.llvm()));
                            let cast = coerce(ctx, vval, vty, alloca_t);
                            ctx.emit(format!("  store {} {cast}, ptr {alloca}", alloca_t.llvm()));
                            ctx.var_set(name, alloca_t, alloca);
                            // 对象变量：记录类名（属性访问用）
                            if alloca_t == Ty::Ptr {
                                if let Some(vt) = vtype {
                                    if !matches!(vt.as_str(), "ALL") && !vt.is_empty() {
                                        ctx.var_ty.insert(name.clone(), vt.clone());
                                    }
                                }
                            }
                        }
                    }
                } else if let AssignTarget::Prop { base, name } = target {
                    // 属性赋值：obj.field = v / this.field = v
                    let (vty, vval) = compile_expr(ctx, value)?;
                    let (bt, bv) = compile_expr(ctx, base)?;
                    if bt != Ty::Ptr {
                        return Err(format!("property assignment on non-object: {name}"));
                    }
                    let bval = if let Expr::Var(vn) = base.as_ref() {
                        if vn == "this" {
                            match ctx.this_reg.clone() {
                                Some(reg) => {
                                    let p = ctx.reg("thp");
                                    ctx.emit(format!("  {p} = load ptr, ptr {reg}"));
                                    p
                                }
                                None => bv,
                            }
                        } else {
                            bv
                        }
                    } else {
                        bv
                    };
                    let cls = field_owner(ctx, base);
                    let (ft, idx) = field_index(ctx, &cls, name)?;
                    let gp = ctx.reg("gep");
                    ctx.emit(format!(
                        "  {gp} = getelementptr %struct.{cls}, ptr {bval}, i32 0, i32 {idx}"
                    ));
                    let cast = coerce(ctx, vval, vty, ft);
                    ctx.emit(format!("  store {} {cast}, ptr {gp}", ft.llvm()));
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

/// 调用语句：CIO::println/print → printf；其他调用（对象方法等）→ 正常 call。
fn compile_call_stmt(ctx: &mut Ctx, e: &Expr) -> Result<(), String> {
    if let Expr::Call { qual, name, args } = e {
        if qual.as_deref() == Some("CIO") || qual.as_deref() == Some("IO") {
            if name == "println" || name == "print" {
                return emit_printf(ctx, args, name == "println");
            }
        }
    }
    // 其他调用：编译（副作用保留）
    compile_expr(ctx, e)?;
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
                    Ty::Ptr => fmt.push_str("%p"),
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

fn is_void_method(ctx: &Ctx, key: &str) -> bool {
    ctx.void_methods.contains(key)
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

/// 属性所属类：base 是 this → 当前类；base 是对象变量 → 变量类型对应的类。
/// 简化：从变量类型名推断（vars 表里对象变量以类名注册）。
fn field_owner(ctx: &Ctx, base: &Expr) -> String {
    if let Expr::Var(vn) = base {
        if vn == "this" {
            // 当前类：找 this_reg 所在函数——用 classes 里第一个含该字段的类兜底
            // 更准确：compile_function 时记录当前类名
            if let Some(c) = &ctx.current_class {
                return c.clone();
            }
        }
        // 对象变量：vars 存 (Ty, reg)，类型信息丢失——用 var_ty 表
        if let Some(t) = ctx.var_ty.get(vn) {
            return t.clone();
        }
    }
    String::new()
}

fn field_index(ctx: &Ctx, cls: &str, name: &str) -> Result<(Ty, u32), String> {
    let fields = ctx.classes.get(cls).ok_or_else(|| {
        format!("property {name} on unknown class {cls}")
    })?;
    for (i, (n, t)) in fields.iter().enumerate() {
        if n == name {
            return Ok((*t, i as u32));
        }
    }
    Err(format!("class {cls} has no field {name}"))
}

fn compile_expr(ctx: &mut Ctx, e: &Expr) -> Result<(Ty, String), String> {
    match e {
        Expr::Int(v) => Ok((Ty::I64, v.to_string())),
        Expr::Float(v) => Ok((Ty::F64, format!("{v:.17}"))),
        Expr::Var(name) => {
            if name == "this" {
                // this 指针
                match ctx.this_reg.clone() {
                    Some(reg) => {
                        let p = ctx.reg("thisv");
                        ctx.emit(format!("  {p} = load ptr, ptr {reg}"));
                        return Ok((Ty::Ptr, p));
                    }
                    None => return Err("this used outside a class method".to_string()),
                }
            }
            match ctx.var_get(name) {
                Some((t, reg)) => {
                    let r = ctx.reg("v");
                    ctx.emit(format!("  {r} = load {}, ptr {reg}", t.llvm()));
                    Ok((t, r))
                }
                None => Err(format!("undefined variable {name}")),
            }
        }
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
                _ => Err(format!("unsupported operator {op}")),
            }
        }
        Expr::Call { qual, name, args } => {
            // 对象方法调用：qual 是对象变量 → 类名_方法名 + 传 this
            let obj_this = if let Some(q) = qual {
                ctx.var_ty.get(q).cloned()
            } else {
                None
            };
            let key = if let Some(owner) = &obj_this {
                format!("{owner}_{name}")
            } else if let Some(q) = qual {
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
            let is_void = rt == Ty::I64 && is_void_method(ctx, &key);
            let mut call = if is_void {
                format!("  call void {fname}(")
            } else {
                format!("  {call_reg} = call {} {fname}(", rt.llvm())
            };
            let mut first = true;
            // 对象方法调用：qual 是对象变量 → 传 this 指针
            if obj_this.is_some() {
                if let Some(q) = qual {
                    if let Some((Ty::Ptr, reg)) = ctx.var_get(q) {
                        let p = ctx.reg("thisp");
                        ctx.emit(format!("  {p} = load ptr, ptr {reg}"));
                        call.push_str(&format!("ptr {p}"));
                        first = false;
                    }
                }
            }
            for a in args {
                let (at, av) = compile_expr(ctx, a)?;
                if !first {
                    call.push_str(", ");
                }
                first = false;
                call.push_str(&format!("{} {av}", at.llvm()));
            }
            call.push(')');
            ctx.emit(call);
            let _ = qual;
            if is_void {
                Ok((Ty::I64, "0".into()))
            } else {
                Ok((rt, call_reg))
            }
        }
        Expr::New { cls, args } => {
            // new Class(args...) → malloc 对象 + 调 __init__(this, args...)
            let fields = ctx
                .classes
                .get(cls)
                .cloned()
                .unwrap_or_default();
            let size = fields.len().max(1) * 8;
            let obj = ctx.reg("obj");
            ctx.emit(format!("  {obj} = call ptr @malloc(i64 {size})"));
            // 调 __init__（若存在）
            let init_key = format!("{cls}___init__");
            if ctx.funcs.contains_key(&init_key) {
                let call_reg = ctx.reg("init");
                let mut call = format!("  call void @{init_key}(ptr {obj}");
                for a in args {
                    let (at, av) = compile_expr(ctx, a)?;
                    call.push_str(&format!(", {} {av}", at.llvm()));
                }
                call.push(')');
                ctx.emit(call);
                let _ = call_reg;
            }
            Ok((Ty::Ptr, obj))
        }
        Expr::Prop { base, name } => {
            // 对象属性读：obj.name / this.name → GEP load
            let (bt, bv) = compile_expr(ctx, base)?;
            if bt != Ty::Ptr {
                return Err(format!("property access on non-object: {name}"));
            }
            // 从 this 指针寄存器取值（若 base 是 this 变量）
            let bval = if let Expr::Var(vn) = base.as_ref() {
                if vn == "this" {
                    match ctx.this_reg.clone() {
                        Some(reg) => {
                            let p = ctx.reg("thp");
                            ctx.emit(format!("  {p} = load ptr, ptr {reg}"));
                            p
                        }
                        None => bv,
                    }
                } else {
                    bv
                }
            } else {
                bv
            };
            // 找字段类型
            let cls = field_owner(ctx, base);
            let (ft, idx) = field_index(ctx, &cls, name)?;
            let gp = ctx.reg("gep");
            ctx.emit(format!(
                "  {gp} = getelementptr %struct.{cls}, ptr {bval}, i32 0, i32 {idx}"
            ));
            let v = ctx.reg("fld");
            ctx.emit(format!("  {v} = load {}, ptr {gp}", ft.llvm()));
            Ok((ft, v))
        }
        _ => Err(format!("LLVM backend does not support this expression yet: {e:?}")),
    }
}

// 占位（保持模块结构）
