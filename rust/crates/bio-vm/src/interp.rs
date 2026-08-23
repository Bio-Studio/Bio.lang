//! 解释器核心（M3）：作用域、流调用、表达式求值、语句执行。

use std::collections::HashMap;
use std::time::Instant;

use bio_core::arena::{StrArena, StrRef};
use bio_core::value::{Cause, Outcome, Tag, Value};
use bio_syntax::ast::*;
use bio_syntax::parser::parse_source;

use crate::builtin;
use crate::registry::{Registry, StreamDef, StreamKind};
use crate::BUILTIN_CLASS_SRC;

/// 控制流信号（语句执行结果）。
pub enum Flow {
    Next,
    Ret(Outcome),
    Break,
    Continue,
}

/// 调用栈帧：方法作用域 + this。
pub struct Frame {
    pub scope: HashMap<String, Value>,
    pub this: Option<u32>, // 对象/流实例句柄
}

/// 对象数据：类名 + 声明字段 + 动态属性。
#[derive(Clone)]
pub struct ObjData {
    pub def: StrRef, // 类名（"Array"/"Vector"/"Solid"/用户类）
    pub fields: Vec<Value>,
    pub attrs: Vec<(StrRef, Value)>,
}

impl Default for ObjData {
    fn default() -> Self {
        ObjData { def: StrRef::NULL, fields: Vec::new(), attrs: Vec::new() }
    }
}

/// 引用值（智能引用，&perm follow base）。
pub struct RefVal {
    pub target: u32, // 指向对象句柄
    pub index: i64,  // 元素偏移（数组指针）
    pub perm: StrRef,
    pub follow: StrRef,
    pub base: StrRef,
    pub is_array_elem: bool,
    pub base_handle: u32, // 数组对象句柄
}

/// 运行结果。
pub struct RunOutcome {
    pub stdout: String,
    pub unmet_needs: Vec<(String, String)>,
}

pub struct Interp {
    pub strs: StrArena,
    pub objects: Vec<ObjData>,
    pub refs: Vec<RefVal>,
    pub reg: Registry,
    pub frames: Vec<Frame>,
    pub stdout: String,
    pub sio_buf: String,
    pub timers: HashMap<u32, Instant>,
    pub timer_seq: u32,
    pub arrays: Vec<u32>, // Arrays 集合（对象句柄）
    pub consts: Vec<(String, Value)>,
    pub stream_instances: HashMap<String, u32>, // fork/class 流单例（持久字段状态）
}

impl Interp {
    pub fn new() -> Self {
        Interp {
            strs: StrArena::new(),
            objects: Vec::new(),
            refs: Vec::new(),
            reg: Registry::new(),
            frames: Vec::new(),
            stdout: String::new(),
            sio_buf: String::new(),
            timers: HashMap::new(),
            timer_seq: 0,
            arrays: Vec::new(),
            consts: Vec::new(),
            stream_instances: HashMap::new(),
        }
    }

    pub fn intern(&mut self, s: &str) -> StrRef {
        self.strs.push(s)
    }

    fn cause(&mut self, s: &str) -> Outcome {
        Outcome::Ref(Cause(self.intern(s)))
    }

    // ---- 顶层入口 ----

    /// 解释单个 Program（含 need 校验）。
    pub fn run(&mut self, prog: &Program) -> RunOutcome {
        // 注入内置类（Array/Vector，Bio 语言编写）
        let builtin_src = BUILTIN_CLASS_SRC.to_string();
        let (builtin_prog, errs) = parse_source(&builtin_src);
        if errs.is_empty() {
            self.reg.register(&builtin_prog);
        }
        // 用户声明
        let mut unmet = self.reg.register(prog);
        // 顶层常量求值
        for d in &prog.decls {
            if let Decl::Const { name, init, .. } = d {
                let v = self.eval_expr(init);
                self.consts.push((name.clone(), v));
            }
        }
        // 执行 Main::exec
        let exec = prog
            .main
            .as_ref()
            .and_then(|m| m.methods.iter().find(|x| x.name == "exec"))
            .cloned();
        if let Some(method) = exec {
            self.frames.push(Frame { scope: HashMap::new(), this: None });
            let outcome = self.exec_method_body(&method);
            self.frames.pop();
            // Main::exec 的返回值丢弃（与旧 C 一致；显式输出走 CIO）
            let _ = outcome;
        }
        // need 校验（多文件/单文件统一）
        unmet.retain(|(k, n)| !self.consts.iter().any(|(cn, _)| cn == n && k == "value"));
        RunOutcome { stdout: self.stdout.clone(), unmet_needs: unmet }
    }

    // ---- 对象 ----

    pub fn new_object(&mut self, def_name: &str, field_count: usize) -> u32 {
        self.new_object_typed(def_name, &[])
    }

    /// 按字段类型初始化默认值：数值 0 / string "" / 其他 nil（13-need 依赖 int hp = 0）。
    pub fn new_object_typed(&mut self, def_name: &str, field_types: &[String]) -> u32 {
        let def = self.intern(def_name);
        let fields = field_types
            .iter()
            .map(|ty| match ty.as_str() {
                "int" | "float" | "double" => Value::int(0),
                "string" => Value::string(self.intern("")),
                "char" => Value::chr(0),
                "bool" => Value::boolean(false),
                _ => Value::nil(),
            })
            .collect();
        self.objects.push(ObjData { def, fields, attrs: Vec::new() });
        (self.objects.len() - 1) as u32
    }

    /// 对象字段查找（this 或对象值上的 Prop）。先声明字段后动态属性。
    pub fn obj_prop_get(&mut self, h: u32, name: &str) -> Option<Value> {
        let def_name = self.objects[h as usize].def;
        let defname_str = self.strs.get(def_name).to_string();
        if let Some(d) = self.reg.streams.get(&defname_str) {
            if let Some(i) = d.field_names.iter().position(|n| n == name) {
                return Some(self.objects[h as usize].fields[i]);
            }
        }
        for (k, v) in self.objects[h as usize].attrs.iter() {
            if self.strs.get(*k) == name {
                return Some(*v);
            }
        }
        None
    }

    pub fn obj_prop_set(&mut self, h: u32, name: &str, v: Value) {
        let nref = self.intern(name);
        let def_name = self.objects[h as usize].def;
        let defname_str = self.strs.get(def_name).to_string();
        if let Some(d) = self.reg.streams.get(&defname_str) {
            if let Some(i) = d.field_names.iter().position(|n| n == name) {
                self.objects[h as usize].fields[i] = v;
                return;
            }
        }
        let o = &mut self.objects[h as usize];
        for (k, slot) in o.attrs.iter_mut() {
            if self.strs.get(*k) == name {
                *slot = v;
                return;
            }
        }
        o.attrs.push((nref, v));
    }

    /// 对象类名。
    pub fn obj_class(&self, h: u32) -> String {
        self.strs.get(self.objects[h as usize].def).to_string()
    }

    // ---- 变量 ----

    fn var_get(&mut self, name: &str) -> Option<Value> {
        for f in self.frames.iter().rev() {
            if let Some(v) = f.scope.get(name) {
                return Some(*v);
            }
        }
        for (n, v) in self.consts.iter().rev() {
            if n == name {
                return Some(*v);
            }
        }
        // this 关键字 → 当前实例
        if name == "this" {
            return self.current_this().map(Value::obj);
        }
        // 流字段（fork/class 单例字段）→ 当前 this 实例字段
        if let Some(h) = self.current_this() {
            if let Some(v) = self.obj_prop_get(h, name) {
                return Some(v);
            }
        }
        // 流名 → 流值（流作为参数/对象传递：CIO、Calc...）
        if self.reg.streams.contains_key(name) || builtin_stream_name(name) {
            let key = format!("$stream:{name}");
            if let Some(h) = self.stream_instances.get(&key) {
                return Some(Value::obj(*h));
            }
            let h = self.new_object(name, 0);
            self.stream_instances.insert(key, h);
            return Some(Value::obj(h));
        }
        None
    }

    fn var_set(&mut self, name: &str, v: Value) {
        for f in self.frames.iter_mut().rev() {
            if f.scope.contains_key(name) {
                f.scope.insert(name.to_string(), v);
                return;
            }
        }
        // 未声明：写入当前帧（宽松；标准：变量须先声明）
        if let Some(f) = self.frames.last_mut() {
            f.scope.insert(name.to_string(), v);
        }
    }

    // ---- 方法调用 ----

    /// 执行方法体（当前帧已压栈）。返回 res/ref 或默认 ref(nothing)。
    fn exec_method_body(&mut self, method: &Method) -> Outcome {
        for st in &method.body {
            match self.exec_stmt(st) {
                Flow::Ret(o) => return o,
                Flow::Break => return self.cause("break outside loop"),
                Flow::Continue => return self.cause("continue outside loop"),
                Flow::Next => {}
            }
        }
        self.cause("nothing")
    }

    /// 调用方法（def 为所属流，this 为实例句柄）。
    fn call_method(
        &mut self,
        def: Option<StreamDef>,
        method: Method,
        this: Option<u32>,
        args: Vec<Value>,
    ) -> Outcome {
        let _ = def;
        let mut scope = HashMap::new();
        for (p, a) in method.params.iter().zip(args.iter()) {
            scope.insert(p.name.clone(), *a);
        }
        self.frames.push(Frame { scope, this });
        let r = self.exec_method_body(&method);
        self.frames.pop();
        r
    }

    /// 调用表达式：Outcome → Value（refused 位 + cause）。
    fn call_to_value(&mut self, qual: Option<&str>, name: &str, args: Vec<Value>) -> Value {
        match self.invoke(qual, name, args) {
            Outcome::Res(v) => v,
            Outcome::Ref(c) => Value::refused_str(c.0),
        }
    }

    /// 统一调用入口：内置流 → 对象方法 → 流方法 → 裸方法 → 拒绝。
    fn invoke(&mut self, qual: Option<&str>, name: &str, args: Vec<Value>) -> Outcome {
        if let Some(q) = qual {
            // 内置流
            if builtin::lookup(q, name).is_some() {
                let f = builtin::lookup(q, name).unwrap();
                return f(self, &args);
            }
            // this::method
            if q == "this" {
                if let Some(h) = self.current_this() {
                    return self.invoke_on_obj(h, name, args);
                }
                return self.cause("no this context");
            }
            // 流名
            if let Some(def) = self.reg.resolve_qual(q).cloned() {
                if let Some(m) = def.methods.get(name).cloned() {
                    let singleton = self.stream_instance(&def);
                    return self.call_method(Some(def), m, singleton, args);
                }
            }
            // 变量（对象值 / 流值）
            if let Some(v) = self.var_get(q) {
                if let Tag::Obj | Tag::Arr = v.tag() {
                    let h = v.as_handle();
                    let cls = self.obj_class(h);
                    // 内置流实例（cio CIO 传参）
                    if let Some(bf) = builtin::lookup(&cls, name) {
                        return bf(self, &args);
                    }
                    if self.reg.class(&cls).is_some() || cls == "Solid" {
                        return self.invoke_on_obj(h, name, args);
                    }
                }
            }
            return self.cause(&format!("stream {q} refuses: no method {name}"));
        } else {
            // 裸调用：先查当前流上下文（04：流内 bare call），再全局方法名
            if let Some(h) = self.current_this() {
                let cls = self.obj_class(h);
                if let Some(def) = self.reg.streams.get(&cls).cloned() {
                    if let Some(m) = def.methods.get(name).cloned() {
                        if !m.body.is_empty() {
                            return self.call_method(Some(def), m, Some(h), args);
                        }
                    }
                }
            }
            let found = self.reg.find_bare_method(name).map(|(d, m)| (d.clone(), m.clone()));
            if let Some((def, m)) = found {
                let singleton = self.stream_instance(&def);
                return self.call_method(Some(def), m, singleton, args);
            }
            return self.cause(&format!("no method {name}"));
        }
    }

    fn current_this(&self) -> Option<u32> {
        self.frames.iter().rev().find_map(|f| f.this)
    }

    /// 对象方法调用（a::set(0,10) / h::getHp()）。
    pub fn invoke_on_obj(&mut self, h: u32, name: &str, args: Vec<Value>) -> Outcome {
        let cls = self.obj_class(h);
        if let Some(def) = self.reg.class(&cls).cloned() {
            if let Some(m) = def.methods.get(name).cloned() {
                return self.call_method(Some(def), m, Some(h), args);
            }
        }
        self.cause(&format!("object {cls} refuses: no method {name}"))
    }

    /// 流实例：fork/class 有**持久单例**（流级字段状态跨调用保持）；signature/main 无。
    fn stream_instance(&mut self, def: &StreamDef) -> Option<u32> {
        match def.kind {
            StreamKind::Fork | StreamKind::Class => {
                let key = def.name.clone();
                if let Some(h) = self.stream_instances.get(&key) {
                    return Some(*h);
                }
                let h = self.new_object_typed(&def.name, &def.field_types);
                self.stream_instances.insert(key, h);
                Some(h)
            }
            _ => None,
        }
    }

    // ---- 表达式 ----

    pub fn eval_expr(&mut self, e: &Expr) -> Value {
        match e {
            Expr::Int(v) => Value::int(*v),
            Expr::Float(v) => Value::num(*v),
            Expr::Str(s) => Value::string(self.intern(s)),
            Expr::Char(c) => Value::chr(*c),
            Expr::Bool(b) => Value::boolean(*b),
            Expr::Var(name) => self
                .var_get(name)
                .unwrap_or_else(|| Value::nil()),
            Expr::Call { qual, name, args } => {
                let vals: Vec<Value> = args.iter().map(|a| self.eval_expr(a)).collect();
                self.call_to_value(qual.as_deref(), name, vals)
            }
            Expr::Prop { base, name } => {
                if name == "res" {
                    // Solid::new().res — 取响应值本身
                    return self.eval_expr(base);
                }
                let bv = self.eval_expr(base);
                match bv.tag() {
                    Tag::Obj | Tag::Arr => {
                        let h = bv.as_handle();
                        self.obj_prop_get(h, name).unwrap_or(Value::nil())
                    }
                    Tag::Ref => Value::nil(),
                    _ => Value::nil(),
                }
            }
            Expr::Index { base, idx } => {
                let bv = self.eval_expr(base);
                let i = self.eval_expr(idx);
                self.index_get(bv, i)
            }
            Expr::BinOp { op, l, r } => {
                let lv = self.eval_expr(l);
                let rv = self.eval_expr(r);
                self.binop(op, lv, rv)
            }
            Expr::Unwrap { op, l } => {
                let v = self.eval_expr(l);
                if op == "get" {
                    if v.refused() {
                        Value::nil()
                    } else {
                        v
                    }
                } else {
                    // cause
                    if v.refused() {
                        Value::string(v.cause())
                    } else {
                        Value::string(self.intern(""))
                    }
                }
            }
            Expr::New { cls, args } => {
                let vals: Vec<Value> = args.iter().map(|a| self.eval_expr(a)).collect();
                self.new_class(cls, vals)
            }
            Expr::NewArray { ty: _ty, size } => {
                let n = self.eval_expr(size);
                let n = n.as_int_or_num() as usize;
                self.new_class("Array", vec![Value::int(n as i64)])
            }
            Expr::RefOf(target) => self.make_ref(target),
        }
    }

    pub fn new_class(&mut self, cls: &str, args: Vec<Value>) -> Value {
        if let Some(def) = self.reg.class(cls).cloned() {
            let h = self.new_object_typed(cls, &def.field_types);
            if let Some(m) = def.methods.get("__init__").cloned() {
                self.call_method(Some(def), m, Some(h), args);
            }
            Value::obj(h)
        } else {
            // 内置类（Array/Vector 已注入 registry；未知类拒绝）
            Value::nil()
        }
    }

    fn make_ref(&mut self, target: &Expr) -> Value {
        // &lvalue：变量 / 数组元素（简化：整变量引用）
        match target {
            Expr::Var(name) => {
                // 找到变量所在帧并记录——简化实现：拷贝当前值 + 名字
                let name_ref = self.intern(name);
                let perm = self.intern("rw");
                let follow = self.intern("u");
                let base = self.intern("");
                self.refs.push(RefVal { target: u32::MAX, perm, follow, base, is_array_elem: false, base_handle: u32::MAX, index: 0 });
                let h = (self.refs.len() - 1) as u32;
                let _ = name_ref;
                Value::reff(h)
            }
            _ => Value::reff(0),
        }
    }

    fn index_get(&mut self, base: Value, idx: Value) -> Value {
        let i = idx.as_int_or_num() as i64;
        match base.tag() {
            Tag::Obj | Tag::Arr => {
                let h = base.as_handle();
                let cls = self.obj_class(h);
                if cls == "Solid" {
                    let d = &self.objects[h as usize];
                    // Solid 数据存 fields[0] 无法放 Vec——Solid 数据放 attrs？见 builtin
                    // 此处由 builtin Solid::get 处理；直接访问 fallback
                    let _ = d;
                    Value::nil()
                } else {
                    // Array/Vector：调对象 get 方法
                    self.invoke_on_obj(h, "get", vec![Value::int(i)]).get()
                }
            }
            _ => Value::nil(),
        }
    }

    fn binop(&mut self, op: &str, l: Value, r: Value) -> Value {
        if l.refused() || r.refused() {
            return Value::nil().with_refused();
        }
        match op {
            "+" => match (l.tag(), r.tag()) {
                (Tag::Int, Tag::Int) => Value::int(l.as_int_or_num() as i64 + r.as_int_or_num() as i64),
                _ => Value::num(l.as_int_or_num() + r.as_int_or_num()),
            },
            "-" => match (l.tag(), r.tag()) {
                (Tag::Int, Tag::Int) => Value::int(l.as_int_or_num() as i64 - r.as_int_or_num() as i64),
                _ => Value::num(l.as_int_or_num() - r.as_int_or_num()),
            },
            "*" => match (l.tag(), r.tag()) {
                (Tag::Int, Tag::Int) => Value::int(l.as_int_or_num() as i64 * r.as_int_or_num() as i64),
                _ => Value::num(l.as_int_or_num() * r.as_int_or_num()),
            },
            "/" => {
                let d = r.as_int_or_num();
                if d == 0.0 {
                    return Value::nil().with_refused();
                }
                match (l.tag(), r.tag()) {
                    (Tag::Int, Tag::Int) => Value::int(l.as_int_or_num() as i64 / d as i64),
                    _ => Value::num(l.as_int_or_num() / d),
                }
            }
            "%" => {
                let d = r.as_int_or_num() as i64;
                if d == 0 {
                    return Value::nil().with_refused();
                }
                Value::int(l.as_int_or_num() as i64 % d)
            }
            "==" => Value::boolean(self.val_cmp(&l, &r) == std::cmp::Ordering::Equal),
            "!=" => Value::boolean(self.val_cmp(&l, &r) != std::cmp::Ordering::Equal),
            "<" => Value::boolean(self.val_cmp(&l, &r) == std::cmp::Ordering::Less),
            ">" => Value::boolean(self.val_cmp(&l, &r) == std::cmp::Ordering::Greater),
            "<=" => Value::boolean(self.val_cmp(&l, &r) != std::cmp::Ordering::Greater),
            ">=" => Value::boolean(self.val_cmp(&l, &r) != std::cmp::Ordering::Less),
            _ => Value::nil(),
        }
    }

    fn val_cmp(&self, l: &Value, r: &Value) -> std::cmp::Ordering {
        if !matches!((l.tag(), r.tag()), (Tag::Int | Tag::Num, Tag::Int | Tag::Num)) {
            return std::cmp::Ordering::Equal;
        }
        l.as_int_or_num().partial_cmp(&r.as_int_or_num()).unwrap_or(std::cmp::Ordering::Equal)
    }

    // ---- 语句 ----

    pub fn exec_stmt(&mut self, s: &Stmt) -> Flow {
        match s {
            Stmt::If { cond, then, els } => {
                let c = self.eval_expr(cond);
                if c.truthy() {
                    self.exec_block(then)
                } else if let Some(e) = els {
                    self.exec_block(e)
                } else {
                    Flow::Next
                }
            }
            Stmt::While { cond, body } => {
                loop {
                    let c = self.eval_expr(cond);
                    if !c.truthy() {
                        return Flow::Next;
                    }
                    match self.exec_block(body) {
                        Flow::Break => return Flow::Next,
                        Flow::Continue => continue,
                        Flow::Ret(o) => return Flow::Ret(o),
                        Flow::Next => {}
                    }
                }
            }
            Stmt::For { init, cond, update, body } => {
                if let Some(i) = init {
                    if let Flow::Ret(o) = self.exec_stmt(i) {
                        return Flow::Ret(o);
                    }
                }
                loop {
                    if let Some(c) = cond {
                        if !self.eval_expr(c).truthy() {
                            return Flow::Next;
                        }
                    }
                    match self.exec_block(body) {
                        Flow::Break => return Flow::Next,
                        Flow::Continue => {
                            if let Some(u) = update {
                                if let Flow::Ret(o) = self.exec_stmt(u) {
                                    return Flow::Ret(o);
                                }
                            }
                            continue;
                        }
                        Flow::Ret(o) => return Flow::Ret(o),
                        Flow::Next => {}
                    }
                    if let Some(u) = update {
                        if let Flow::Ret(o) = self.exec_stmt(u) {
                            return Flow::Ret(o);
                        }
                    }
                }
            }
            Stmt::Break => Flow::Break,
            Stmt::Continue => Flow::Continue,
            Stmt::Ret { kind, values } => match kind {
                RetKind::Res => {
                    let mut vals: Vec<Value> = values.iter().map(|v| self.eval_expr(v)).collect();
                    match vals.len() {
                        0 => Flow::Ret(Outcome::Res(Value::nil())),
                        1 => Flow::Ret(Outcome::Res(vals.remove(0))),
                        _ => {
                            // 多值 → 数组（Solid）
                            let h = self.solid_new(vals);
                            Flow::Ret(Outcome::Res(Value::obj(h)))
                        }
                    }
                }
                RetKind::Ref => {
                    let reason = values
                        .first()
                        .map(|v| self.eval_expr(v))
                        .map(|v| match v.tag() {
                            Tag::Str => v.as_str(),
                            _ => {
                                let s = self.fmt_value(&v);
                                self.intern(&s)
                            }
                        })
                        .unwrap_or_else(|| self.intern("nothing"));
                    Flow::Ret(Outcome::Ref(Cause(reason)))
                }
            },
            Stmt::Assign { vtype, is_const, is_thread, target, op, value } => {
                let _ = (is_const, is_thread);
                let v = self.eval_expr(value);
                match target {
                    AssignTarget::Var(name) => {
                        let v = if op == "=" {
                            v
                        } else {
                            let old = self.var_get(name).unwrap_or(Value::nil());
                            self.binop(&op[..1], old, v)
                        };
                        if vtype.is_some() {
                            if let Some(f) = self.frames.last_mut() {
                                f.scope.insert(name.clone(), v);
                            }
                        } else {
                            self.var_set(name, v);
                        }
                    }
                    AssignTarget::Prop { base, name } => {
                        let bv = self.eval_expr(base);
                        if let Tag::Obj | Tag::Arr = bv.tag() {
                            let h = bv.as_handle();
                            let v = if op == "=" {
                                v
                            } else {
                                let old = self.obj_prop_get(h, name).unwrap_or(Value::nil());
                                self.binop(&op[..1], old, v)
                            };
                            self.obj_prop_set(h, name, v);
                        }
                    }
                    AssignTarget::Index { base, idx } => {
                        let bv = self.eval_expr(base);
                        let i = self.eval_expr(idx);
                        self.index_set(bv, i, v);
                    }
                }
                Flow::Next
            }
            Stmt::RefDecl { base, name, init, .. } => {
                let rv = self.eval_expr(init);
                let _ = base;
                if let Some(f) = self.frames.last_mut() {
                    f.scope.insert(name.clone(), rv);
                }
                Flow::Next
            }
            Stmt::Inc { name, op } => {
                let old = self.var_get(name).unwrap_or(Value::nil());
                let delta = if op == "++" { 1 } else { -1 };
                let nv = match old.tag() {
                    Tag::Int => Value::int(old.as_int_or_num() as i64 + delta),
                    Tag::Num => Value::num(old.as_int_or_num() + delta as f64),
                    _ => old,
                };
                self.var_set(name, nv);
                Flow::Next
            }
            Stmt::Expr(e) => {
                self.eval_expr(e);
                Flow::Next
            }
        }
    }

    fn exec_block(&mut self, stmts: &[Stmt]) -> Flow {
        for s in stmts {
            match self.exec_stmt(s) {
                Flow::Next => {}
                other => return other,
            }
        }
        Flow::Next
    }

    fn index_set(&mut self, base: Value, idx: Value, v: Value) {
        let i = idx.as_int_or_num() as i64;
        match base.tag() {
            Tag::Obj | Tag::Arr => {
                let h = base.as_handle();
                let cls = self.obj_class(h);
                if cls == "Solid" {
                    if let Some(d) = self.objects.get_mut(h as usize) {
                        if let Some(slot) = d.attrs.iter().position(|(k, _)| self.strs.get(*k) == "$data") {
                            // Solid 数据存 attrs 的 "$data" 键（builtin 约定）
                            let _ = slot;
                        }
                        let _ = (d, i, v);
                    }
                } else {
                    self.invoke_on_obj(h, "set", vec![Value::int(i), v]);
                }
            }
            _ => {}
        }
    }

    /// 创建 Solid 实例（多值返回/内置）。
    pub fn solid_new(&mut self, data: Vec<Value>) -> u32 {
        let def = self.intern("Solid");
        self.objects.push(ObjData { def, fields: vec![], attrs: Vec::new() });
        let h = (self.objects.len() - 1) as u32;
        // 数据存 attrs "$data"
        let key = self.intern("$data");
        let dh = self.objs_data_handle(data);
        self.objects[h as usize].attrs.push((key, Value::arr(dh)));
        h
    }

    fn objs_data_handle(&mut self, data: Vec<Value>) -> u32 {
        // 数据 Vec 存独立 "Data" 对象
        let def = self.intern("SolidData");
        self.objects.push(ObjData { def, fields: data, attrs: Vec::new() });
        (self.objects.len() - 1) as u32
    }

    // ---- 值格式化（打印/字符串化） ----

    pub fn fmt_value(&mut self, v: &Value) -> String {
        if v.refused() {
            return self.strs.get(v.cause()).to_string();
        }
        match v.tag() {
            Tag::Nil => "nil".to_string(),
            Tag::Int => v.as_int_or_num().to_string(),
            Tag::Num => {
                let f = v.as_int_or_num();
                let s = format!("{f}");
                s
            }
            Tag::Bool => if v.as_bool() { "true" } else { "false" }.to_string(),
            Tag::Str => self.strs.get(v.as_str()).to_string(),
            Tag::Char => (v.as_char() as char).to_string(),
            Tag::Obj | Tag::Arr => {
                let h = v.as_handle();
                self.fmt_object(h)
            }
            Tag::Ref => "<ref>".to_string(),
        }
    }

    fn fmt_object(&mut self, h: u32) -> String {
        let cls = self.obj_class(h);
        if cls == "Solid" || cls == "SolidData" {
            let data = self.solid_data(h).to_vec();
            return format!("[{}]", data.iter().map(|x| self.fmt_value(x)).collect::<Vec<_>>().join(", "));
        }
        if cls == "Array" || cls == "Vector" {
            // data 属性（this::data = Solid::new().res）或声明字段
            let o = &self.objects[h as usize];
            let mut data_h = o.fields.first().map(|v| v.as_handle());
            if data_h.is_none() {
                for (k, v) in &o.attrs {
                    if self.strs.get(*k) == "data" {
                        if let Tag::Obj | Tag::Arr = v.tag() {
                            data_h = Some(v.as_handle());
                        }
                    }
                }
            }
            if let Some(dh) = data_h {
                let data = self.solid_data(dh).to_vec();
                return format!("[{}]", data.iter().map(|x| self.fmt_value(x)).collect::<Vec<_>>().join(", "));
            }
        }
        // 一般对象：<object Cls {k: v, ...}>
        let o = self.objects[h as usize].clone();
        let mut parts = Vec::new();
        for (k, val) in &o.attrs {
            let kn = self.strs.get(*k).to_string();
            if kn == "$data" {
                continue;
            }
            parts.push(format!("{kn}: {}", self.fmt_value(val)));
        }
        format!("<object {} {{{}}}>", cls, parts.join(", "))
    }

    /// Solid/SolidData 的数据读取（借用处理：复制出来）。
    pub fn solid_data(&self, h: u32) -> Vec<Value> {
        let o = &self.objects[h as usize];
        let cls = self.strs.get(o.def).to_string();
        if cls == "Solid" {
            // attrs "$data" → Arr 句柄 → SolidData
            for (k, v) in &o.attrs {
                if self.strs.get(*k) == "$data" {
                    if let Tag::Arr = v.tag() {
                        return self.objects[v.as_handle() as usize].fields.clone();
                    }
                }
            }
            Vec::new()
        } else {
            o.fields.clone()
        }
    }

    /// 修改 Solid 数据。
    pub fn solid_set(&mut self, h: u32, i: usize, v: Value) {
        let data_h = self.solid_data_handle(h);
        if let Some(dh) = data_h {
            self.objects[dh as usize].fields[i] = v;
        }
    }

    pub fn solid_data_handle(&self, h: u32) -> Option<u32> {
        let o = &self.objects[h as usize];
        let cls = self.strs.get(o.def).to_string();
        if cls == "SolidData" {
            return Some(h);
        }
        for (k, v) in &o.attrs {
            if self.strs.get(*k) == "$data" {
                if let Tag::Arr = v.tag() {
                    return Some(v.as_handle());
                }
            }
        }
        None
    }

    pub fn solid_push(&mut self, h: u32, v: Value) {
        let data_h = self.solid_data_handle(h);
        if let Some(dh) = data_h {
            self.objects[dh as usize].fields.push(v);
        }
    }
}

impl Default for Interp {
    fn default() -> Self {
        Self::new()
    }
}

/// 内置流名（流可作为值传递）。
pub fn builtin_stream_name(name: &str) -> bool {
    matches!(name, "CIO" | "SIO" | "FIO" | "IO" | "Com" | "Time" | "Obj" | "Solid" | "Arrays" | "Ref" | "Threads" | "Taskm")
}
