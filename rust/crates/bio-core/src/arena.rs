//! Arena 内存规划（bio-core）。
//!
//! # BumpArena<T> — 类型化 bump 分配器
//!
//! - 分页：每页固定 `PAGE_SLOTS` 槽；页表 `Vec<Box<[T]>>` 只增不减；
//! - `alloc(v) -> u32`：句柄 = `(page << SHIFT) | slot`，0 保留为 null；
//! - `get(h) -> &T` / `get_mut`：O(1)；
//! - 永不释放单个槽（程序级生命周期，与旧 C `aalloc` 语义一致）；
//! - 句柄优点：扩容搬迁安全、可序列化、对齐无空洞、u32 省内存。
//!
//! # StrArena — 字符串字节池
//!
//! 分页字节缓冲（页 4 KiB 起步，几何增长），`push(&str) -> StrRef{off,len}`
//! 只拷贝一次；之后取用零拷贝。`StrRef` 8 字节，可安全穿越线程边界
//! （字节池只增，读不竞争——协作式调度下无并发写）。

use std::marker::PhantomData;

/// 字符串引用：(offset, len) 指向 StrArena 字节池。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct StrRef {
    pub off: u32,
    pub len: u32,
}

impl StrRef {
    pub const NULL: StrRef = StrRef { off: 0, len: 0 };
    pub fn is_null(self) -> bool {
        self.off == 0 && self.len == 0
    }
}

/// 类型化 bump arena。`T: Copy` 约束保证句柄读取无别名问题。
pub struct BumpArena<T: Copy> {
    pages: Vec<Box<[T]>>,
    next: u32, // 当前页已用槽数
    _marker: PhantomData<T>,
}

const PAGE_SLOTS: u32 = 256;      // 每页槽数（2^8）
const SHIFT: u32 = 8;
const SLOT_MASK: u32 = PAGE_SLOTS - 1;
const MAX_HANDLE: u32 = u32::MAX >> 1; // 最高位留给 null 标志扩展

impl<T: Copy> BumpArena<T> {
    pub fn new() -> Self {
        BumpArena { pages: Vec::new(), next: 0, _marker: PhantomData }
    }

    fn ensure_page(&mut self) {
        if self.next == PAGE_SLOTS || self.pages.is_empty() {
            let page: Box<[T]> = (0..PAGE_SLOTS).map(|_| unsafe { std::mem::zeroed() }).collect();
            self.pages.push(page);
            self.next = 1; // 槽 0 保留为 null 哨兵，句柄永不等于 0
        }
    }

    /// 分配一个槽，返回 u32 句柄。0 永不返回（保留为 null）。
    #[inline]
    pub fn alloc(&mut self, v: T) -> u32 {
        self.ensure_page();
        let page = self.pages.len() as u32 - 1;
        let slot = self.next;
        self.pages[page as usize][slot as usize] = v;
        self.next += 1;
        (page << SHIFT) | slot
    }

    /// 句柄 → 不可变引用。
    #[inline]
    pub fn get(&self, h: u32) -> &T {
        debug_assert!(h != 0 && h <= MAX_HANDLE);
        let page = (h >> SHIFT) as usize;
        let slot = (h & SLOT_MASK) as usize;
        &self.pages[page][slot]
    }

    /// 句柄 → 可变引用（bump 语义下互斥由外部保证）。
    #[inline]
    pub fn get_mut(&mut self, h: u32) -> &mut T {
        debug_assert!(h != 0 && h <= MAX_HANDLE);
        let page = (h >> SHIFT) as usize;
        let slot = (h & SLOT_MASK) as usize;
        &mut self.pages[page][slot]
    }

    pub fn pages(&self) -> usize {
        self.pages.len()
    }

    pub fn capacity(&self) -> u32 {
        self.pages.len() as u32 * PAGE_SLOTS
    }
}

impl<T: Copy> Default for BumpArena<T> {
    fn default() -> Self {
        Self::new()
    }
}

/// 字符串字节池。
pub struct StrArena {
    pages: Vec<Vec<u8>>,
    cur: Vec<u8>,
}

impl StrArena {
    pub fn new() -> Self {
        StrArena { pages: Vec::new(), cur: Vec::with_capacity(4096) }
    }

    /// 写入一个字符串，返回 (offset, len)。数据拷贝一次后永驻。
    #[inline]
    pub fn push(&mut self, s: &str) -> StrRef {
        let bytes = s.as_bytes();
        if self.cur.len() + bytes.len() > self.cur.capacity() {
            // 当前页放不下：封页，开新页（容量几何增长）
            if !self.cur.is_empty() {
                self.pages.push(std::mem::take(&mut self.cur));
            }
            let cap = (4096usize).max(bytes.len().next_power_of_two());
            self.cur = Vec::with_capacity(cap);
        }
        let off = self.total_len() as u32;
        self.cur.extend_from_slice(bytes);
        StrRef { off, len: bytes.len() as u32 }
    }

    /// 按 StrRef 取回字符串视图（零拷贝）。
    #[inline]
    pub fn get<'a>(&'a self, r: StrRef) -> &'a str {
        if r.is_null() {
            return "";
        }
        let start = r.off as usize;
        let end = start + r.len as usize;
        let mut acc = 0usize;
        for page in &self.pages {
            let page_len = page.len();
            if start < acc + page_len && end <= acc + page_len {
                return std::str::from_utf8(&page[start - acc..end - acc]).unwrap_or("");
            }
            acc += page_len;
        }
        std::str::from_utf8(&self.cur[start - acc..end - acc]).unwrap_or("")
    }

    fn total_len(&self) -> usize {
        self.pages.iter().map(|p| p.len()).sum::<usize>() + self.cur.len()
    }
}

impl Default for StrArena {
    fn default() -> Self {
        Self::new()
    }
}

/// 通用别名：对象/数组/线程等句柄表都用 BumpArena。
pub type Arena<T> = BumpArena<T>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bump_arena_alloc_get() {
        let mut a = BumpArena::<u64>::new();
        let h1 = a.alloc(42);
        let h2 = a.alloc(7);
        assert_ne!(h1, h2);
        assert_eq!(*a.get(h1), 42);
        assert_eq!(*a.get(h2), 7);
        *a.get_mut(h1) = 99;
        assert_eq!(*a.get(h1), 99);
    }

    #[test]
    fn bump_arena_multi_page() {
        let mut a = BumpArena::<u32>::new();
        let mut last = 0;
        for i in 1..1000u32 {
            last = a.alloc(i);
        }
        assert_eq!(*a.get(last), 999);
        assert!(a.pages() >= 3);
    }

    #[test]
    fn str_arena_roundtrip() {
        let mut s = StrArena::new();
        let a = s.push("hello");
        let b = s.push("世界");
        let c = s.push("x".repeat(5000).as_str());
        assert_eq!(s.get(a), "hello");
        assert_eq!(s.get(b), "世界");
        assert_eq!(s.get(c).len(), 5000);
        assert!(s.get(StrRef::NULL).is_empty());
    }

    #[test]
    fn str_arena_cross_page() {
        // 跨页边界的长串必须完整可读
        let mut s = StrArena::new();
        let long = "abc".repeat(2000);
        let r = s.push(&long);
        assert_eq!(s.get(r), long);
    }
}
