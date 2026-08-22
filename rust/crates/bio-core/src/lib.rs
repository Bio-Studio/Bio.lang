//! bio-core — BioLang 运行时核心。
//!
//! 内存规划（手写掌控版，对标旧 C 的 arena 设计并强化）：
//!
//! 1. **一切值进 arena，引用用 u32 句柄而非指针**
//!    - 句柄 = (page, slot) 打包，arena 扩容/搬迁不需要修指针；
//!    - 句柄天然可序列化（.img 打包、跨线程传递）；8 字节对齐无空洞；
//!    - 与旧 C 的 `aalloc`（裸指针 + 永不释放）相比，句柄方案在保持
//!      "程序级一次性生命周期" 的同时，获得可搬迁 + 可序列化两个能力。
//! 2. **字符串 = 全局字节池中的 (offset, len)**，写入即 intern，
//!    零拷贝复用；`StrRef` 8 字节。
//! 3. **Value 16 字节**：u32 tag（含 REFUSED 标志位）+ u64 负载 + 4B pad；
//!    所有标量（int/float/double/bool/char）内联，字符串/对象/数组走句柄。
//!    （v2 候选：NaN-boxing 压到 8 字节，代价是 int 精度受限——旧 LLVM
//!    后端统一 double 语义，解释器保留 i64，故 v1 不采用。）
//! 4. **请求模型 = Value 的 tag 位**：bit31 = refused，负载为 cause 的
//!    字符串句柄；`res`/`ref` 不产生堆分配，随值传递。
//! 5. **区域划分**：每个流（Unistream/Remstream/Threadstream...）在 arena
//!    内拥有自己的页区，线程隔离靠区域隔离实现（协作式调度，无锁）。

pub mod arena;
pub mod value;

pub use arena::{Arena, BumpArena, StrArena, StrRef};
pub use value::{Cause, Outcome, Tag, Value};
