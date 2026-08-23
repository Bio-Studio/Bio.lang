//! Value / 请求模型（bbb-core）。
//!
//! # Value — 16 字节标量优先表示
//!
//! ```text
//! ┌──────────────┬──────────────────┬──────────────┐
//! │ tag: u32     │ data: u64        │ pad: u32     │
//! │ bit31 REFUSED│ 负载             │ (对齐)       │
//! └──────────────┴──────────────────┴──────────────┘
//! ```
//!
//! - 标量（Int/Num/Bool/Char）全部内联，零堆分配；
//! - Str/Obj/Arr/Ref 走句柄（u32 → arena），8 字节以内；
//! - **REFUSED 位**：`ref "原因"` 产生的请求结果 = 同值 + 标志位，
//!   不额外分配；`get`/`cause` 只是位测试；
//! - `Outcome`：解释器内部用，`Res(Value)` / `Ref(Cause)` 二态，
//!   与语法层 ResStatement/RefStatement 一一对应。
//!
//! # 类型标签（低 24 位）
//!
//! Nil / Int / Num / Bool / Str / Char / Obj / Arr / Ref

use crate::arena::StrRef;

pub const TAG_MASK: u32 = 0x00FF_FFFF;
pub const REFUSED: u32 = 0x8000_0000;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum Tag {
    Nil = 0,
    Int = 1,
    Num = 2,   // float/double 统一 double 语义（LLVM 后端同款）
    Bool = 3,
    Str = 4,
    Char = 5,
    Obj = 6,
    Arr = 7,
    Ref = 8,   // 智能引用句柄（&perm follow base）
}

impl Tag {
    #[inline]
    pub fn from_bits(bits: u32) -> Tag {
        match bits & TAG_MASK {
            0 => Tag::Nil,
            1 => Tag::Int,
            2 => Tag::Num,
            3 => Tag::Bool,
            4 => Tag::Str,
            5 => Tag::Char,
            6 => Tag::Obj,
            7 => Tag::Arr,
            _ => Tag::Ref,
        }
    }
}

/// 16 字节 Value（对齐 8；字段按对齐降序排列保证紧凑）。
#[derive(Debug, Clone, Copy)]
#[repr(C, align(8))]
pub struct Value {
    data: u64, // 负载
    tag: u32,  // 低 24 位类型 + bit31 REFUSED
    _pad: u32,
}

impl Value {
    pub const NIL: Value = Value { tag: Tag::Nil as u32, data: 0, _pad: 0 };

    #[inline]
    pub fn nil() -> Self {
        Self::NIL
    }

    #[inline]
    pub fn int(v: i64) -> Self {
        Value { tag: Tag::Int as u32, data: v as u64, _pad: 0 }
    }

    #[inline]
    pub fn num(v: f64) -> Self {
        Value { tag: Tag::Num as u32, data: v.to_bits(), _pad: 0 }
    }

    #[inline]
    pub fn boolean(v: bool) -> Self {
        Value { tag: Tag::Bool as u32, data: v as u64, _pad: 0 }
    }

    #[inline]
    pub fn string(r: StrRef) -> Self {
        Value { tag: Tag::Str as u32, data: ((r.off as u64) << 32) | r.len as u64, _pad: 0 }
    }

    #[inline]
    pub fn chr(v: u8) -> Self {
        Value { tag: Tag::Char as u32, data: v as u64, _pad: 0 }
    }

    #[inline]
    pub fn obj(h: u32) -> Self {
        Value { tag: Tag::Obj as u32, data: h as u64, _pad: 0 }
    }

    #[inline]
    pub fn arr(h: u32) -> Self {
        Value { tag: Tag::Arr as u32, data: h as u64, _pad: 0 }
    }

    #[inline]
    pub fn reff(h: u32) -> Self {
        Value { tag: Tag::Ref as u32, data: h as u64, _pad: 0 }
    }

    #[inline]
    pub fn tag(&self) -> Tag {
        Tag::from_bits(self.tag)
    }

    #[inline]
    pub fn refused(&self) -> bool {
        self.tag & REFUSED != 0
    }

    /// 标记为拒绝（请求模型：ref）。保留原值，仅置位。
    #[inline]
    pub fn with_refused(mut self) -> Self {
        self.tag |= REFUSED;
        self
    }

    /// 拒绝值：REFUSED 位 + 原因字符串句柄（cause）。
    #[inline]
    pub fn refused_str(r: StrRef) -> Self {
        Value {
            tag: Tag::Str as u32 | REFUSED,
            data: ((r.off as u64) << 32) | r.len as u64,
            _pad: 0,
        }
    }

    /// 拒绝原因（未拒绝时返回空串句柄）。
    #[inline]
    pub fn cause(&self) -> StrRef {
        if self.refused() && self.tag() == Tag::Str {
            self.as_str()
        } else {
            StrRef::NULL
        }
    }

    /// 数值视图：Int → i64 → f64；Num → f64（统一 double 语义）。
    #[inline]
    pub fn as_int_or_num(&self) -> f64 {
        match self.tag() {
            Tag::Int => self.as_int() as f64,
            Tag::Num => self.as_num(),
            Tag::Bool => self.as_bool() as u8 as f64,
            Tag::Char => self.as_char() as f64,
            _ => 0.0,
        }
    }

    #[inline]
    pub fn as_int(&self) -> i64 {
        debug_assert_eq!(self.tag(), Tag::Int);
        self.data as i64
    }

    #[inline]
    pub fn as_num(&self) -> f64 {
        debug_assert_eq!(self.tag(), Tag::Num);
        f64::from_bits(self.data)
    }

    #[inline]
    pub fn as_bool(&self) -> bool {
        debug_assert_eq!(self.tag(), Tag::Bool);
        self.data != 0
    }

    #[inline]
    pub fn as_str(&self) -> StrRef {
        debug_assert_eq!(self.tag(), Tag::Str);
        StrRef { off: (self.data >> 32) as u32, len: self.data as u32 }
    }

    #[inline]
    pub fn as_char(&self) -> u8 {
        debug_assert_eq!(self.tag(), Tag::Char);
        self.data as u8
    }

    #[inline]
    pub fn as_handle(&self) -> u32 {
        debug_assert!(matches!(self.tag(), Tag::Obj | Tag::Arr | Tag::Ref));
        self.data as u32
    }

    /// 真值判定（与旧实现一致）：0 / "" / 拒绝 = false，其余 true。
    #[inline]
    pub fn truthy(&self) -> bool {
        if self.refused() {
            return false;
        }
        match self.tag() {
            Tag::Nil => false,
            Tag::Int => self.as_int() != 0,
            Tag::Num => self.as_num() != 0.0,
            Tag::Bool => self.as_bool(),
            Tag::Str => !self.as_str().is_null(),
            Tag::Char => self.as_char() != 0,
            _ => true,
        }
    }
}

impl PartialEq for Value {
    fn eq(&self, other: &Self) -> bool {
        if self.refused() != other.refused() {
            return false;
        }
        self.tag == other.tag && self.data == other.data
    }
}
impl Eq for Value {}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.refused() {
            return write!(f, "<refused>");
        }
        match self.tag() {
            Tag::Nil => write!(f, "nil"),
            Tag::Int => write!(f, "{}", self.as_int()),
            Tag::Num => write!(f, "{}", self.as_num()),
            Tag::Bool => write!(f, "{}", self.as_bool()),
            Tag::Str => write!(f, "<str {}>", self.data),
            Tag::Char => write!(f, "{}", self.as_char() as char),
            Tag::Obj => write!(f, "<object {}>", self.as_handle()),
            Tag::Arr => write!(f, "<array {}>", self.as_handle()),
            Tag::Ref => write!(f, "<ref {}>", self.as_handle()),
        }
    }
}

/// 请求结果：Res = 响应，Ref = 拒绝（携带 cause 字符串）。
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Outcome {
    Res(Value),
    Ref(Cause),
}

/// 拒绝原因：字符串句柄（arena 内，零拷贝）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Cause(pub StrRef);

impl Outcome {
    #[inline]
    pub fn is_refused(&self) -> bool {
        matches!(self, Outcome::Ref(_))
    }

    /// `get`：取实际值（拒绝时返回 Nil）。
    #[inline]
    pub fn get(self) -> Value {
        match self {
            Outcome::Res(v) => v,
            Outcome::Ref(_) => Value::nil(),
        }
    }

    /// `cause`：取拒绝原因（未拒绝时返回空字符串）。
    #[inline]
    pub fn cause(self) -> StrRef {
        match self {
            Outcome::Res(_) => StrRef::NULL,
            Outcome::Ref(c) => c.0,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn value_size_is_16() {
        assert_eq!(std::mem::size_of::<Value>(), 16);
    }

    #[test]
    fn scalars_inline() {
        assert_eq!(Value::int(42).as_int(), 42);
        assert_eq!(Value::num(3.5).as_num(), 3.5);
        assert!(Value::boolean(true).as_bool());
        assert_eq!(Value::chr(b'x').as_char(), b'x');
        assert_eq!(Value::nil().tag(), Tag::Nil);
    }

    #[test]
    fn string_roundtrip() {
        let r = StrRef { off: 12, len: 5 };
        let v = Value::string(r);
        assert_eq!(v.tag(), Tag::Str);
        assert_eq!(v.as_str(), r);
    }

    #[test]
    fn refused_flag() {
        let ok = Value::int(7);
        let bad = ok.with_refused();
        assert!(bad.refused());
        assert!(!ok.refused());
        assert_ne!(ok, bad);
        assert_eq!(bad.truthy(), false);
    }

    #[test]
    fn truthiness() {
        assert!(!Value::nil().truthy());
        assert!(!Value::int(0).truthy());
        assert!(Value::int(1).truthy());
        assert!(!Value::num(0.0).truthy());
        assert!(!Value::boolean(false).truthy());
        assert!(!Value::string(StrRef::NULL).truthy());
    }

    #[test]
    fn outcome_get_cause() {
        let res = Outcome::Res(Value::int(10));
        assert_eq!(res.get(), Value::int(10));
        assert!(res.cause().is_null());

        let cause = Cause(StrRef { off: 3, len: 9 });
        let rej = Outcome::Ref(cause);
        assert!(rej.is_refused());
        assert_eq!(rej.get(), Value::nil());
        assert_eq!(rej.cause(), cause.0);
    }
}
