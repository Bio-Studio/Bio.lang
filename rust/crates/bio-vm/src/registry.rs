//! 流注册表：把 Program 的声明构建成可调用的流/方法表。

use std::collections::HashMap;

use bio_syntax::ast::{Decl, Member, Method, Program};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StreamKind {
    Signature, // Stream X { ... } 仅签名
    Fork,      // Sig X { ... } 实现
    Class,     // Class X { ... }
    Binary,    // Stream X & "lib.so"（本轮仅注册，调用拒绝）
    Main,      // Main 流
}

#[derive(Debug, Clone)]
pub struct StreamDef {
    pub name: String,
    pub kind: StreamKind,
    pub sig: Option<String>, // fork 的签名流名
    pub bin_file: Option<String>, // StreamBin 的库文件
    pub methods: HashMap<String, Method>,
    pub field_names: Vec<String>,
    pub field_types: Vec<String>, // 与 field_names 一一对应（默认值初始化用）
    pub annos: Vec<String>,       // @onlyread/@unfork（流注解）
}

impl StreamDef {
    pub fn is_class(&self) -> bool {
        self.kind == StreamKind::Class
    }
}

#[derive(Debug, Default)]
pub struct Registry {
    pub streams: HashMap<String, StreamDef>,
    pub order: Vec<String>, // 声明顺序（对象打印/调试用）
}

impl Registry {
    pub fn new() -> Self {
        Registry::default()
    }

    /// 把 Program 的声明注册进表。返回未满足的 need（name, kind）。
    pub fn register(&mut self, prog: &Program) -> Vec<(String, String)> {
        let mut unmet = Vec::new();
        let mut needs = Vec::new();
        for d in &prog.decls {
            match d {
                Decl::Need { kind, name } => needs.push((kind.clone(), name.clone())),
                Decl::StreamSig { name, members, annos, .. } => {
                    let def = build_stream(name.clone(), StreamKind::Signature, None, members, annos);
                    self.insert(def);
                }
                Decl::StreamBin { name, file, members, .. } => {
                    let def = build_stream(name.clone(), StreamKind::Binary, None, members, &[]);
                    let def = StreamDef { bin_file: Some(file.clone()), ..def };
                    self.insert(def);
                }
                Decl::Class { name, members, annos, .. } => {
                    let def = build_stream(name.clone(), StreamKind::Class, None, members, annos);
                    self.insert(def);
                }
                Decl::Fork { sig, name, members, annos, .. } => {
                    let mut def = build_stream(name.clone(), StreamKind::Fork, Some(sig.clone()), members, annos);
                    // 字段/签名方法继承自签名流（15：int count 声明在 Stream ReadOnly）
                    if let Some(sig_def) = self.streams.get(sig).cloned() {
                        let mut names = sig_def.field_names.clone();
                        let mut types = sig_def.field_types.clone();
                        for (i, n) in names.iter().enumerate() {
                            if !def.field_names.contains(n) {
                                def.field_names.push(n.clone());
                                def.field_types.push(types[i].clone());
                            }
                        }
                        for (mn, mm) in &sig_def.methods {
                            def.methods.entry(mn.clone()).or_insert_with(|| mm.clone());
                        }
                    }
                    self.insert(def);
                }
                Decl::Const { .. } => {}
            }
        }
        if let Some(m) = &prog.main {
            let mut methods = HashMap::new();
            for meth in &m.methods {
                methods.insert(meth.name.clone(), meth.clone());
            }
            let def = StreamDef {
                name: "Main".into(),
                kind: StreamKind::Main,
                sig: None,
                methods,
                field_names: Vec::new(),
                field_types: Vec::new(),
                annos: Vec::new(),
                bin_file: None,
            };
            self.insert(def);
        }
        // need 校验
        for (kind, name) in needs {
            let ok = match kind.as_str() {
                "value" => prog.decls.iter().any(|d| matches!(d, Decl::Const { name: n, .. } if n == &name)),
                "function" => self.find_bare_method(&name).is_some(),
                "stream" | "Stream" => self.streams.contains_key(&name),
                "Class" => self.streams.get(&name).map(|s| s.is_class()).unwrap_or(false),
                _ => false,
            };
            if !ok {
                unmet.push((kind, name));
            }
        }
        unmet
    }

    fn insert(&mut self, def: StreamDef) {
        self.order.push(def.name.clone());
        self.streams.insert(def.name.clone(), def);
    }

    /// 签名流调用回退：qual 是签名流时找其分叉实现。
    pub fn resolve_qual(&self, qual: &str) -> Option<&StreamDef> {
        let d = self.streams.get(qual)?;
        if d.kind == StreamKind::Signature {
            for other in self.streams.values() {
                if other.sig.as_deref() == Some(qual) {
                    return Some(other);
                }
            }
        }
        Some(d)
    }

    /// 裸调用：全局按方法名扫描（Main 优先，然后声明顺序）。
    pub fn find_bare_method(&self, name: &str) -> Option<(&StreamDef, &Method)> {
        if let Some(main) = self.streams.get("Main") {
            if let Some(m) = main.methods.get(name) {
                return Some((main, m));
            }
        }
        for key in &self.order {
            if key == "Main" {
                continue;
            }
            if let Some(d) = self.streams.get(key) {
                if let Some(m) = d.methods.get(name) {
                    // 签名方法（无体）不是实现，跳过；找分叉的实现
                    if m.body.is_empty() {
                        continue;
                    }
                    return Some((d, m));
                }
            }
        }
        None
    }

    /// 类定义（new 用）。
    pub fn class(&self, name: &str) -> Option<&StreamDef> {
        let d = self.streams.get(name)?;
        if d.is_class() {
            Some(d)
        } else {
            None
        }
    }
}

fn build_stream(
    name: String,
    kind: StreamKind,
    sig: Option<String>,
    members: &[Member],
    annos: &[String],
) -> StreamDef {
    let mut methods = HashMap::new();
    let mut field_names = Vec::new();
    let mut field_types = Vec::new();
    for m in members {
        match m {
            Member::Method(meth) => {
                methods.insert(meth.name.clone(), meth.clone());
            }
            Member::Field { ty, names } => {
                for n in names {
                    field_names.push(n.clone());
                    field_types.push(ty.clone());
                }
            }
        }
    }
    StreamDef { name, kind, sig, bin_file: None, methods, field_names, field_types, annos: annos.to_vec() }
}
