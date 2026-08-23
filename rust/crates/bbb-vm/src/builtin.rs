//! 内置流（Rust 实现）：CIO/SIO/FIO/Com/Time/Obj/Solid/Arrays。
//!
//! 方法表 `HashMap<(qual, method), fn>`。签名统一：
//! `fn(&mut Interp, &[Value]) -> Outcome`。
//! 实现原则：与旧 C builtin.c 语义一致（examples 期望输出为准）。

use std::collections::HashMap;
use std::fs;
use std::time::Instant;

use bbb_core::value::{Cause, Outcome, Tag, Value};

use crate::interp::Interp;

pub type BuiltinFn = fn(&mut Interp, &[Value]) -> Outcome;

fn f(q: &'static str, m: &'static str, f: BuiltinFn) -> ((&'static str, &'static str), BuiltinFn) {
    ((q, m), f)
}

pub fn table() -> HashMap<(&'static str, &'static str), BuiltinFn> {
    let mut t = HashMap::new();
    for (k, v) in [
        // ---- CIO 控制台 ----
        f("CIO", "println", cio_println),
        f("CIO", "print", cio_print),
        f("CIO", "error", cio_error),
        f("IO", "println", cio_println),
        f("IO", "print", cio_print),
        // ---- SIO 字符串缓冲 ----
        f("SIO", "format", sio_format),
        f("SIO", "upper", sio_upper),
        f("SIO", "lower", sio_lower),
        f("SIO", "trim", sio_trim),
        f("SIO", "contains", sio_contains),
        f("SIO", "substring", sio_substring),
        f("SIO", "replace", sio_replace),
        f("SIO", "println", sio_println),
        f("SIO", "print", sio_print),
        f("SIO", "getln", sio_getln),
        // ---- FIO 文件 ----
        f("FIO", "writeFile", fio_write_file),
        f("FIO", "appendFile", fio_append_file),
        f("FIO", "readFile", fio_read_file),
        f("FIO", "exists", fio_exists),
        // ---- Com 计算 ----
        f("Com", "abs", com_abs),
        f("Com", "min", com_min),
        f("Com", "max", com_max),
        f("Com", "pow", com_pow),
        f("Com", "sqrt", com_sqrt),
        f("Com", "floor", com_floor),
        f("Com", "ceil", com_ceil),
        f("Com", "round", com_round),
        f("Com", "sign", com_sign),
        f("Com", "sin", com_sin),
        f("Com", "cos", com_cos),
        f("Com", "tan", com_tan),
        f("Com", "log", com_log),
        f("Com", "exp", com_exp),
        // ---- Time 定时器 ----
        f("Time", "start", time_start),
        f("Time", "sleep", time_sleep),
        f("Time", "elapsed", time_elapsed),
        f("Time", "fork", time_fork),
        f("Time", "reset", time_reset),
        // ---- Obj 对象 ----
        f("Obj", "set", obj_set),
        f("Obj", "get", obj_get),
        f("Obj", "call", obj_call),
        f("Obj", "new", obj_new),
        // ---- Solid 连续存储 ----
        f("Solid", "new", solid_new),
        f("Solid", "len", solid_len),
        f("Solid", "get", solid_get),
        f("Solid", "set", solid_set),
        f("Solid", "push", solid_push),
        f("Solid", "pop", solid_pop),
        f("Solid", "join", solid_join),
        f("Solid", "clear", solid_clear),
        // 裸数组（SolidData）复用 Solid 的数据方法
        f("SolidData", "len", solid_len),
        f("SolidData", "get", solid_get),
        f("SolidData", "set", solid_set),
        f("SolidData", "push", solid_push),
        f("SolidData", "pop", solid_pop),
        f("SolidData", "join", solid_join),
        f("SolidData", "clear", solid_clear),
        // ---- Arrays 集合 ----
        f("Arrays", "add", arrays_add),
        f("Arrays", "count", arrays_count),
        f("Arrays", "all", arrays_all),
        f("Arrays", "get", arrays_get),
        f("Arrays", "forget", arrays_forget),
        f("Arrays", "vector", arrays_vector),
        f("Arrays", "sort", arrays_sort),
        // ---- Threads 协作线程（顺序 join 式） ----
        f("Threads", "spawn", threads_spawn),
        f("Threads", "join", threads_join),
        f("Threads", "yield", threads_yield),
        f("Threads", "active", threads_active),
        f("Threads", "self", threads_self),
        // ---- Taskm 任务管理器 ----
        f("Taskm", "add", taskm_add),
        f("Taskm", "interval", taskm_interval),
        f("Taskm", "run", taskm_run),
        f("Taskm", "stop", taskm_stop),
        f("Taskm", "active", taskm_active),
        // ---- Ref 智能引用 ----
        f("Ref", "read", ref_read),
        f("Ref", "write", ref_write),
        f("Ref", "move", ref_move),
        f("Ref", "target", ref_target),
        f("Ref", "perm", ref_perm),
    ] {
        t.insert(k, v);
    }
    t
}

pub fn lookup(q: &str, m: &str) -> Option<BuiltinFn> {
    // 静态表每次重建开销可忽略（21 条）；或 once_cell——stdlib only，直接静态构造
    use std::sync::OnceLock;
    static T: OnceLock<HashMap<(&'static str, &'static str), BuiltinFn>> = OnceLock::new();
    let t = T.get_or_init(table);
    t.get(&(q, m)).copied()
}

fn arg_str(interp: &mut Interp, a: &Value) -> String {
    interp.fmt_value(a)
}

fn res(v: Value) -> Outcome {
    Outcome::Res(v)
}

fn refn(interp: &mut Interp, msg: &str) -> Outcome {
    Outcome::Ref(Cause(interp.intern(msg)))
}

// ---- CIO ----

fn cio_println(interp: &mut Interp, args: &[Value]) -> Outcome {
    let line = args.iter().map(|a| arg_str(interp, a)).collect::<Vec<_>>().join(" ");
    interp.stdout.push_str(&line);
    interp.stdout.push('\n');
    res(Value::nil())
}

fn cio_print(interp: &mut Interp, args: &[Value]) -> Outcome {
    // print 直接拼接（无分隔），println 空格分隔——旧 C 行为（03 输出依赖）
    let line = args.iter().map(|a| arg_str(interp, a)).collect::<String>();
    interp.stdout.push_str(&line);
    res(Value::nil())
}

fn cio_error(interp: &mut Interp, args: &[Value]) -> Outcome {
    let line = args.iter().map(|a| arg_str(interp, a)).collect::<Vec<_>>().join(" ");
    interp.stdout.push_str(&line);
    interp.stdout.push('\n');
    res(Value::nil())
}

// ---- SIO ----

fn sio_format(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(fmt) = args.first() else { return refn(interp, "SIO::format 需要格式串") };
    let fmt_s = arg_str(interp, fmt);
    let rest = &args[1..];
    let mut out = String::new();
    let mut it = fmt_s.chars().peekable();
    let mut ai = 0;
    while let Some(c) = it.next() {
        if c != '%' {
            out.push(c);
            continue;
        }
        match it.next() {
            Some('d') => {
                if ai < rest.len() {
                    out.push_str(&format!("{}", rest[ai].as_int_or_num() as i64));
                }
                ai += 1;
            }
            Some('f') => {
                if ai < rest.len() {
                    out.push_str(&format!("{}", rest[ai].as_int_or_num()));
                }
                ai += 1;
            }
            Some('s') => {
                if ai < rest.len() {
                    out.push_str(&arg_str(interp, &rest[ai]));
                }
                ai += 1;
            }
            Some('%') => out.push('%'),
            Some(other) => {
                out.push('%');
                out.push(other);
            }
            None => out.push('%'),
        }
    }
    res(Value::string(interp.intern(&out)))
}

fn sio_upper(interp: &mut Interp, args: &[Value]) -> Outcome {
    let s = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    res(Value::string(interp.intern(&s.to_uppercase())))
}

fn sio_lower(interp: &mut Interp, args: &[Value]) -> Outcome {
    let s = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    res(Value::string(interp.intern(&s.to_lowercase())))
}

fn sio_trim(interp: &mut Interp, args: &[Value]) -> Outcome {
    let s = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    res(Value::string(interp.intern(s.trim())))
}

fn sio_contains(interp: &mut Interp, args: &[Value]) -> Outcome {
    let hay = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    let needle = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    res(Value::boolean(hay.contains(&needle)))
}

fn sio_substring(interp: &mut Interp, args: &[Value]) -> Outcome {
    let s = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    let start = args.get(1).map(|a| a.as_int_or_num() as usize).unwrap_or(0);
    let len = args.get(2).map(|a| a.as_int_or_num() as usize);
    let sub: String = s.chars().skip(start).take(len.unwrap_or(s.len())).collect();
    res(Value::string(interp.intern(&sub)))
}

fn sio_replace(interp: &mut Interp, args: &[Value]) -> Outcome {
    let s = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    let from = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    let to = args.get(2).map(|a| arg_str(interp, a)).unwrap_or_default();
    res(Value::string(interp.intern(&s.replace(&from, &to))))
}

fn sio_println(interp: &mut Interp, args: &[Value]) -> Outcome {
    let line = args.iter().map(|a| arg_str(interp, a)).collect::<Vec<_>>().join(" ");
    interp.sio_buf.push_str(&line);
    interp.sio_buf.push('\n');
    res(Value::nil())
}

fn sio_print(interp: &mut Interp, args: &[Value]) -> Outcome {
    let line = args.iter().map(|a| arg_str(interp, a)).collect::<String>();
    interp.sio_buf.push_str(&line);
    res(Value::nil())
}

fn sio_getln(interp: &mut Interp, _args: &[Value]) -> Outcome {
    if let Some(pos) = interp.sio_buf.find('\n') {
        let line: String = interp.sio_buf.drain(..pos + 1).collect();
        let line = line.trim_end_matches('\n').to_string();
        res(Value::string(interp.intern(&line)))
    } else {
        let all = std::mem::take(&mut interp.sio_buf);
        res(Value::string(interp.intern(&all)))
    }
}

// ---- FIO ----

fn fio_write_file(interp: &mut Interp, args: &[Value]) -> Outcome {
    let path = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    let content = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    match fs::write(&path, content) {
        Ok(()) => res(Value::boolean(true)),
        Err(e) => refn(interp, &format!("FIO::writeFile 失败: {e}")),
    }
}

fn fio_append_file(interp: &mut Interp, args: &[Value]) -> Outcome {
    let path = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    let content = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    match fs::OpenOptions::new().append(true).create(true).open(&path)
        .and_then(|mut f| std::io::Write::write_all(&mut f, content.as_bytes())) {
        Ok(()) => res(Value::boolean(true)),
        Err(e) => refn(interp, &format!("FIO::appendFile 失败: {e}")),
    }
}

fn fio_read_file(interp: &mut Interp, args: &[Value]) -> Outcome {
    let path = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    match fs::read_to_string(&path) {
        Ok(s) => res(Value::string(interp.intern(&s))),
        Err(e) => refn(interp, &format!("FIO::readFile 失败: {e}")),
    }
}

fn fio_exists(interp: &mut Interp, args: &[Value]) -> Outcome {
    let path = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    res(Value::int(std::path::Path::new(&path).exists() as i64))
}

// ---- Com ----

fn num1(interp: &mut Interp, a: &Value) -> Result<f64, Outcome> {
    if matches!(a.tag(), Tag::Int | Tag::Num) {
        Ok(a.as_int_or_num())
    } else {
        Err(refn(interp, "Com 需要数值参数"))
    }
}

fn com_abs(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.abs())),
        Err(e) => e,
    }
}

fn com_min(interp: &mut Interp, args: &[Value]) -> Outcome {
    let a = match num1(interp, args.first().unwrap_or(&Value::nil())) { Ok(v) => v, Err(e) => return e };
    let b = match num1(interp, args.get(1).unwrap_or(&Value::nil())) { Ok(v) => v, Err(e) => return e };
    res(Value::num(a.min(b)))
}

fn com_max(interp: &mut Interp, args: &[Value]) -> Outcome {
    let a = match num1(interp, args.first().unwrap_or(&Value::nil())) { Ok(v) => v, Err(e) => return e };
    let b = match num1(interp, args.get(1).unwrap_or(&Value::nil())) { Ok(v) => v, Err(e) => return e };
    res(Value::num(a.max(b)))
}

fn com_pow(interp: &mut Interp, args: &[Value]) -> Outcome {
    let a = match num1(interp, args.first().unwrap_or(&Value::nil())) { Ok(v) => v, Err(e) => return e };
    let b = match num1(interp, args.get(1).unwrap_or(&Value::nil())) { Ok(v) => v, Err(e) => return e };
    res(Value::num(a.powf(b)))
}

fn com_sqrt(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.sqrt())),
        Err(e) => e,
    }
}

fn com_floor(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.floor())),
        Err(e) => e,
    }
}

fn com_ceil(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.ceil())),
        Err(e) => e,
    }
}

fn com_round(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.round())),
        Err(e) => e,
    }
}

fn com_sign(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::int(v.signum() as i64)),
        Err(e) => e,
    }
}

fn com_sin(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.sin())),
        Err(e) => e,
    }
}

fn com_cos(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.cos())),
        Err(e) => e,
    }
}

fn com_tan(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.tan())),
        Err(e) => e,
    }
}

fn com_log(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.ln())),
        Err(e) => e,
    }
}

fn com_exp(interp: &mut Interp, args: &[Value]) -> Outcome {
    match num1(interp, args.first().unwrap_or(&Value::nil())) {
        Ok(v) => res(Value::num(v.exp())),
        Err(e) => e,
    }
}

// ---- Time ----

fn time_start(interp: &mut Interp, _args: &[Value]) -> Outcome {
    interp.timers.insert(0, Instant::now());
    res(Value::nil())
}

fn time_sleep(_interp: &mut Interp, args: &[Value]) -> Outcome {
    let ms = args.first().map(|a| a.as_int_or_num()).unwrap_or(0.0);
    std::thread::sleep(std::time::Duration::from_millis(ms as u64));
    res(Value::nil())
}

fn time_elapsed(interp: &mut Interp, args: &[Value]) -> Outcome {
    let id = args.first().map(|a| a.as_int_or_num() as u32).unwrap_or(0);
    match interp.timers.get(&id) {
        Some(t) => {
            let secs = t.elapsed().as_secs_f64();
            res(Value::num(secs))
        }
        None => refn(interp, "Time: timer not started"),
    }
}

fn time_fork(interp: &mut Interp, _args: &[Value]) -> Outcome {
    interp.timer_seq += 1;
    let id = interp.timer_seq;
    interp.timers.insert(id, Instant::now());
    res(Value::int(id as i64))
}

fn time_reset(interp: &mut Interp, args: &[Value]) -> Outcome {
    let id = args.first().map(|a| a.as_int_or_num() as u32).unwrap_or(0);
    if id == 0 {
        return refn(interp, "Time refused: first timer (thread default) cannot be reset; use Time::fork()");
    }
    interp.timers.insert(id, Instant::now());
    res(Value::nil())
}

// ---- Obj ----

fn obj_set(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(obj) = args.first() else { return refn(interp, "Obj::set 需要对象") };
    let (Tag::Obj | Tag::Arr) = obj.tag() else { return refn(interp, "Obj::set 第一个参数不是对象") };
    let h = obj.as_handle();
    let key = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    let val = args.get(2).copied().unwrap_or(Value::nil());
    interp.obj_prop_set(h, &key, val);
    res(Value::nil())
}

fn obj_get(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(obj) = args.first() else { return refn(interp, "Obj::get 需要对象") };
    let (Tag::Obj | Tag::Arr) = obj.tag() else { return refn(interp, "Obj::get 第一个参数不是对象") };
    let h = obj.as_handle();
    let key = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    match interp.obj_prop_get(h, &key) {
        Some(v) => res(v),
        None => refn(interp, "missing attribute"),
    }
}

fn obj_call(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(obj) = args.first() else { return refn(interp, "Obj::call 需要对象") };
    let (Tag::Obj | Tag::Arr) = obj.tag() else { return refn(interp, "Obj::call 第一个参数不是对象") };
    let h = obj.as_handle();
    let name = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    let rest = args[2..].to_vec();
    interp.invoke_on_obj(h, &name, rest)
}

fn obj_new(interp: &mut Interp, args: &[Value]) -> Outcome {
    let cls = args.first().map(|a| arg_str(interp, a)).unwrap_or_default();
    let rest = args[1..].to_vec();
    res(interp.new_class(&cls, rest))
}

// ---- Solid ----

fn solid_new(interp: &mut Interp, _args: &[Value]) -> Outcome {
    let h = interp.solid_new(Vec::new());
    res(Value::obj(h))
}

fn solid_data_h(interp: &mut Interp, args: &[Value]) -> Result<u32, Outcome> {
    let Some(a) = args.first() else { return Err(refn(interp, "Solid 方法需要存储句柄")) };
    let (Tag::Obj | Tag::Arr) = a.tag() else { return Err(refn(interp, "Solid 参数不是句柄")) };
    let h = a.as_handle();
    let cls = interp.obj_class(h);
    if cls != "Solid" && cls != "SolidData" {
        return Err(refn(interp, "Solid 参数不是 Solid 实例"));
    }
    Ok(h)
}

fn solid_len(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    res(Value::int(interp.solid_data(h).len() as i64))
}

fn solid_get(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    let i = match args.get(1).ok_or(0).and_then(|a| Ok(a.as_int_or_num() as i64)) {
        Ok(i) => i,
        Err(_) => 0,
    };
    let data = interp.solid_data(h);
    if i < 0 || i as usize >= data.len() {
        return refn(interp, "index out of bounds");
    }
    res(data[i as usize])
}

fn solid_set(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    let i = args.get(1).map(|a| a.as_int_or_num() as i64).unwrap_or(0);
    let v = args.get(2).copied().unwrap_or(Value::nil());
    let len = interp.solid_data(h).len() as i64;
    if i < 0 || i >= len {
        return refn(interp, "index out of bounds");
    }
    interp.solid_set(h, i as usize, v);
    res(Value::nil())
}

fn solid_push(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    let v = args.get(1).copied().unwrap_or(Value::nil());
    interp.solid_push(h, v);
    res(Value::nil())
}

fn solid_pop(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    let data_h = match interp.solid_data_handle(h) {
        Some(dh) => dh,
        None => return refn(interp, "no data"),
    };
    match interp.objects[data_h as usize].fields.pop() {
        Some(v) => res(v),
        None => refn(interp, "pop from empty"),
    }
}

fn solid_join(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    let sep = args.get(1).map(|a| arg_str(interp, a)).unwrap_or_default();
    let data = interp.solid_data(h);
    let parts: Vec<String> = data.iter().map(|v| interp.fmt_value(v)).collect();
    res(Value::string(interp.intern(&parts.join(&sep))))
}

fn solid_clear(interp: &mut Interp, args: &[Value]) -> Outcome {
    let h = match solid_data_h(interp, args) { Ok(h) => h, Err(e) => return e };
    if let Some(dh) = interp.solid_data_handle(h) {
        interp.objects[dh as usize].fields.clear();
    }
    res(Value::nil())
}

// ---- Arrays ----

fn arrays_add(interp: &mut Interp, args: &[Value]) -> Outcome {
    if let Some(a) = args.first() {
        if let Tag::Obj | Tag::Arr = a.tag() {
            interp.arrays.push(a.as_handle());
        }
    }
    res(Value::nil())
}

fn arrays_count(interp: &mut Interp, _args: &[Value]) -> Outcome {
    res(Value::int(interp.arrays.len() as i64))
}

fn arrays_all(interp: &mut Interp, _args: &[Value]) -> Outcome {
    // 返回整个注册表（所有 Array/Vector 实例组成的数组）
    let vals: Vec<Value> = interp.arrays.iter().map(|h| Value::obj(*h)).collect();
    let dh = interp.objs_data_handle(vals);
    res(Value::arr(dh))
}

fn arrays_get(interp: &mut Interp, args: &[Value]) -> Outcome {
    let i = args.first().map(|a| a.as_int_or_num() as usize).unwrap_or(0);
    match interp.arrays.get(i) {
        Some(h) => res(Value::obj(*h)),
        None => refn(interp, "Arrays: index out of bounds"),
    }
}

fn arrays_forget(interp: &mut Interp, args: &[Value]) -> Outcome {
    if let Some(a) = args.first() {
        if let Tag::Obj | Tag::Arr = a.tag() {
            let h = a.as_handle();
            interp.arrays.retain(|x| *x != h);
        }
    }
    res(Value::nil())
}

fn arrays_vector(interp: &mut Interp, _args: &[Value]) -> Outcome {
    let v = interp.new_class("Vector", Vec::new());
    res(v)
}

fn arrays_sort(interp: &mut Interp, args: &[Value]) -> Outcome {
    // Arrays::sort(arr) — 原地排序数组（内部调用数组的 __sort__ 内部方法）
    let Some(a) = args.first() else {
        return refn(interp, "Arrays::sort 需要一个数组参数");
    };
    let (Tag::Obj | Tag::Arr) = a.tag() else {
        return refn(interp, "Arrays::sort 参数不是数组对象");
    };
    let h = a.as_handle();
    // 通过内部方法 __sort__ 原地排序（数组对象 → Solid 数据）
    let out = interp.invoke_on_obj(h, "__sort__", Vec::new());
    if out.is_refused() {
        // 裸数组（SolidData）直接排
        if let Some(dh) = interp.solid_data_handle(h) {
            interp.sort_data(dh);
            return res(Value::nil());
        }
        return refn(interp, "Arrays::sort 失败：不是可排序的数组");
    }
    res(Value::nil())
}

// ---- Threads ----

fn threads_spawn(interp: &mut Interp, args: &[Value]) -> Outcome {
    let name = args.first().map(|a| interp.fmt_value(a)).unwrap_or_default();
    let rest = args[1..].to_vec();
    interp.thread_seq += 1;
    let id = interp.thread_seq;
    let def_name = interp.reg.find_bare_method(&name).map(|(d, _)| d.name.clone());
    interp.threads.push(crate::interp::ThreadTask { id, name, def_name, args: rest, done: None });
    Outcome::Res(Value::int(id as i64))
}

fn threads_join(interp: &mut Interp, args: &[Value]) -> Outcome {
    let id = args.first().map(|a| a.as_int_or_num() as u32).unwrap_or(0);
    // 先执行其它未完成任务（逆序——11 期望 thread 2 先打印），再执行目标
    let others: Vec<u32> = interp.threads.iter().filter(|t| t.done.is_none() && t.id != id).map(|t| t.id).rev().collect();
    for oid in others {
        interp.run_thread(oid);
    }
    interp.run_thread(id)
}

fn threads_yield(_interp: &mut Interp, _args: &[Value]) -> Outcome {
    Outcome::Res(Value::nil()) // 顺序执行：no-op
}

fn threads_active(interp: &mut Interp, _args: &[Value]) -> Outcome {
    let n = interp.threads.iter().filter(|t| t.done.is_none()).count();
    Outcome::Res(Value::int(n as i64))
}

fn threads_self(interp: &mut Interp, _args: &[Value]) -> Outcome {
    Outcome::Res(Value::int(interp.running_thread.unwrap_or(0) as i64))
}

// ---- Taskm ----

fn taskm_add(interp: &mut Interp, args: &[Value]) -> Outcome {
    threads_spawn(interp, args)
}

fn taskm_interval(_interp: &mut Interp, _args: &[Value]) -> Outcome {
    Outcome::Res(Value::nil())
}

fn taskm_run(interp: &mut Interp, _args: &[Value]) -> Outcome {
    let ids: Vec<u32> = interp.threads.iter().filter(|t| t.done.is_none()).map(|t| t.id).collect();
    for id in ids {
        interp.run_thread(id);
    }
    Outcome::Res(Value::nil())
}

fn taskm_stop(interp: &mut Interp, _args: &[Value]) -> Outcome {
    interp.threads.clear();
    Outcome::Res(Value::nil())
}

fn taskm_active(interp: &mut Interp, _args: &[Value]) -> Outcome {
    threads_active(interp, &[])
}

// ---- Ref ----

fn ref_read(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(a) = args.first() else { return refn(interp, "Ref::read 需要引用") };
    if a.tag() != Tag::Ref { return refn(interp, "Ref::read 参数不是引用"); }
    interp.ref_read(a.as_handle())
}

fn ref_write(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(a) = args.first() else { return refn(interp, "Ref::write 需要引用") };
    if a.tag() != Tag::Ref { return refn(interp, "Ref::write 参数不是引用") };
    let v = args.get(1).copied().unwrap_or(Value::nil());
    match interp.ref_write(a.as_handle(), v) {
        Outcome::Res(_) => Outcome::Res(Value::nil()),
        // Ref::write 方法级拒绝消息带 "Ref refused: " 前缀（11 的 cause 输出）
        Outcome::Ref(_) => Outcome::Ref(Cause(interp.intern("Ref refused: reference is read-only, cannot write"))),
    }
}

fn ref_move(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(a) = args.first() else { return refn(interp, "Ref::move 需要引用") };
    if a.tag() != Tag::Ref { return refn(interp, "Ref::move 参数不是引用") };
    interp.ref_move(a.as_handle())
}

fn ref_target(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(a) = args.first() else { return refn(interp, "Ref::target 需要引用") };
    if a.tag() != Tag::Ref { return refn(interp, "Ref::target 参数不是引用") };
    let r = interp.refs[a.as_handle() as usize].clone();
    match &r.target {
        crate::interp::RefTarget::Var(name) => Outcome::Res(Value::string(interp.intern(name))),
        crate::interp::RefTarget::ArrElem { index, .. } => Outcome::Res(Value::int(*index)),
        crate::interp::RefTarget::ObjProp { name, .. } => Outcome::Res(Value::string(interp.intern(name))),
    }
}

fn ref_perm(interp: &mut Interp, args: &[Value]) -> Outcome {
    let Some(a) = args.first() else { return refn(interp, "Ref::perm 需要引用") };
    if a.tag() != Tag::Ref { return refn(interp, "Ref::perm 参数不是引用") };
    let r = interp.refs[a.as_handle() as usize].clone();
    Outcome::Res(Value::string(interp.intern(&r.perm)))
}
