# BioLang Rust + LLVM 重写计划（RUST-PLAN.md）

> 2026-08-22 定稿。目标：把 bio 重写为 **Rust 实现 + LLVM 编译后端**，
> 流程为「**MPS 语言模型 → 生成 Rust 代码 → 手动优化（掌控内存规划）**」。
> **标准层不改变**：DESIGN.md 语义、examples/01-17 语法与输出、项目 CLI 行为。

## 1. 现状

- 2026-08-13：旧 C 实现（解释器 + LLVM 后端 + 打包）整体移除（git 提交 891add2），
  仓库变为 **JetBrains MPS 语言工作台**：`languages/biolang/languageModels/structure.mps`
  （43 个概念，语言的唯一事实源）+ `solutions/biolang.sandbox` + examples 回归集。
- `bin/` 保留预编译二进制（gitignore）：`bio` 解释器可用；`bio shell build`
  编译模式因引用已删除的 `src/` 而失效——**编译能力待 Rust 实现补回**。

## 2. 生成管线（MPS → Rust，v1 已落地）

| 环节 | 状态 | 说明 |
|---|---|---|
| `tools/mps_gen_rust.py` | ✅ | 解析 structure.mps → 生成 `rust/crates/bio-syntax/src/ast_generated.rs`（39 struct + 4 抽象 enum，覆盖 43 概念）+ `concepts_generated.rs`（概念注册表：id/别名/父级） |
| 手动优化 | 按需 | 复制 `ast_generated.rs` → `ast.rs` 再改造（arena 索引、紧凑布局）；结构模型更新后重跑生成器 diff 同步 |
| MPS 原生 generator | 占位 | `languages/biolang/generator/`（README 指引；MPS 内 New → Generator 即可挂接，产出物与 v1 对齐） |

## 3. 架构（rust/ cargo workspace，v0.4.0）

| crate | 职责 | 里程碑 |
|---|---|---|
| `bio-syntax` | 生成的 AST 类型 + 手写词法器（零拷贝 token：kind + 源切片 + 行列，错误路径才扫描） | M1 ✅ |
| `bio-core` | arena 内存规划 + Value/请求模型（见 §4） | M1 ✅ |
| `bio-cli` | `bio-rs` 入口（lexer/arena 调试命令） | M1 ✅ |
| `bio-vm`（规划） | 解释器：流注册表、eval、need 解析、协作线程/Taskm、项目 CLI、.img 打包 | M3 |
| `bio-llvm`（规划） | LLVM IR **文本**后端（对标旧 `src/llvm.c` 1321 行，零依赖，系统 clang 链接） | M4 |

## 4. 内存规划（手写掌控版，对标旧 C arena 并强化）

1. **句柄化 arena**：引用一律 u32 句柄 `(page<<8)|slot`，不用裸指针——
   扩容/搬迁无需修指针；天然可序列化（.img 打包）；槽 0 为 null 哨兵。
2. **字符串字节池**：`StrRef(off,len)` 8 字节，写入即 intern，取用零拷贝。
3. **Value 16 字节**：`u64 负载 + u32 tag（bit31=REFUSED）+ pad`；
   标量（int/float/double/bool/char）全内联，字符串/对象/数组走句柄。
4. **请求模型**：`ref` = tag 置位，零分配；`Outcome::Res/Ref` 与 MPS 概念
   ResStatement/RefStatement 一一对应；`get`/`cause` 是位测试。
5. **流区域划分**：每个流（Unistream/Remstream/Threadstream…）在 arena 内
   拥有自己的页区；协作式调度 ⇒ 无锁。
6. **v2 候选**：NaN-boxing 压到 8 字节——代价是 int 精度受限（旧 LLVM 后端
   统一 double 语义，解释器保留 i64），评估后再定。

## 5. 里程碑

- **M1 ✅** 生成骨架 + 词法器 + arena/value 核心（16 个单元测试全绿；
  `bio-rs lexer` 实测 examples 出 230 token；10 万次 arena 分配压力测试通过）
- **M2** 手写 parser（Pratt 表达式 + 语句级），AST 用生成类型 + span 定位
- **M3** 解释器：流注册表（CIO/FIO/SIO/Com/Time/Rem/Solid/Array/Ref/Threads/Taskm…）、
  need 依赖解析、项目 CLI（init/build/run/install/destroy）、.img/.zip 打包
- **M4** LLVM 后端：AST→IR 文本（参考旧 `src/llvm.c`），统一 double 语义，
  `bio llvm` 子命令 + 独立 `bio-llvm` 产物
- **M5** 回归：examples/01-17 输出 diff（与 bin/bio 解释器对比）+ 项目示例
  + 二进制库互操作（`Stream m & "libm.so"`）
- **M6** 性能：autotest 基准对标旧 C（update ~9.3ms 级），release LTO 优化

## 6. 参考与回归基准

- 旧 C 实现：`git show 891add2^:src/<file>`（interp.c 1101 行 / llvm.c 1321 行 /
  pack.c 545 行 / builtin.c 845 行…）
- 预编译解释器：`bin/bio`（跑 examples 的对照基准）
- BiolStudio（`/home/jack/Projects/BiolStudio`）：Python 功能层已能驱动
  `bin/bio` 解释器，Rust 实现就位后可平滑切换
- 标准层铁律：examples/ 与 DESIGN.md 是契约，实现层禁止改动

## 7. 注意事项

- 语法细节一律以 examples + 旧 C 源码为准（注释/字符串/char/数字词法、
  need 语义、智能引用 7 权限 × 4 层级 = 28 型、Solid 头指针语义…）
- 生成文件带 `@generated` 标记，禁止手改；手动优化在副本（ast.rs）上进行
- 切换 Rust 工具链用 `~/.cargo/bin/cargo`（本机 PATH 未含）
