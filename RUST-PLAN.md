# BiuBiuBiu Rust + LLVM 重写计划（RUST-PLAN.md）

> 2026-08-22 定稿，2026-08-22 更新（M2 完成，**完全原生手写路径**）。
> 目标：把 bio 重写为 **Rust 实现 + LLVM 编译后端**，标准层不改变
> （DESIGN.md 语义、examples/01-17 语法与输出、项目 CLI 行为）。

## 0. 实现路径（2026-08-22 用户拍板：完全原生，不借助 MPS）

- 词法器、AST、解析器、内存核心全部**手写 Rust**；
- **标准层定义（不改变，2026-08-23 修正）**：
  1. 语言设计标准 = **最新设计（用户亲自设计）**：MPS 语言模型
     structure.mps（43 概念）+ DESIGN.md；关键字表以 M2 实现为准
     （29 个：get/this/int/float/double/string/char/bool/true/false
     均为关键字——最新设计如此，旧 C 的 22 字表只是参考实现的历史状态）；
  2. 回归标准 = examples/01-17 输出（tests/examples.rs 固化 parse 层）；
  3. 工具链标准 = vscode/biolang-vscode（已恢复 0.16.0，含高亮/补全/格式化）；
  4. 旧 C 实现（git 891add2^）是参考实现，**不是标准**，语法细节
     与最新设计冲突时以最新设计为准。
- `tools/mps_gen_rust.py` 与 `ast_generated.rs` 仅历史对照，
  `bbb-syntax` 导出的是手写 `ast`。

## 1. 现状

- 2026-08-13：旧 C 实现 + VSCode 插件随 MPS 迁移提交（891add2）整体移除；
  2026-08-22 插件已从 git 历史恢复（vscode/bbb-vscode）；2026-08-23
  MPS 相关内容全部移除。
- `bin/` 预编译二进制（gitignore）：`bio` 解释器可用；`bio shell build`
  编译模式因引用已删除的 `src/` 而失效——**编译能力待 Rust 实现补回**。

## 2. 架构（rust/ cargo workspace，v0.4.0）

| crate | 职责 | 里程碑 |
|---|---|---|
| `bbb-syntax` | 手写 AST（ast.rs）+ 手写解析器（parser.rs，Pratt 表达式 + 语句/声明/成员全语法面）+ 手写词法器（零拷贝 token） | M2 ✅ |
| `bbb-core` | arena 内存规划 + Value/请求模型（见 §3） | M1 ✅ |
| `bbb-cli` | `bbb` 入口（lexer/parse/arena 调试命令） | M1/M2 ✅ |
| `bbb-vm`（规划） | 解释器：流注册表、eval、need 解析、协作线程/Taskm、项目 CLI、.img 打包 | M3 |
| `bbb-llvm`（规划） | LLVM IR **文本**后端（对标旧 `src/llvm.c` 1321 行，零依赖，系统 clang 链接） | M4 |

## 3. 内存规划（手写掌控版，对标旧 C arena 并强化）

1. **句柄化 arena**：引用一律 u32 句柄 `(page<<8)|slot`，不用裸指针——
   扩容/搬迁无需修指针；天然可序列化（.img 打包）；槽 0 为 null 哨兵。
2. **字符串字节池**：`StrRef(off,len)` 8 字节，写入即 intern，取用零拷贝。
3. **Value 16 字节**：`u64 负载 + u32 tag（bit31=REFUSED）+ pad`；
   标量（int/float/double/bool/char）全内联，字符串/对象/数组走句柄。
4. **请求模型**：`ref` = tag 置位，零分配；`Outcome::Res/Ref` 与语法层
   ResStatement/RefStatement 一一对应；`get`/`cause` 是位测试。
5. **流区域划分**：每个流（Unistream/Remstream/Threadstream…）在 arena 内
   拥有自己的页区；协作式调度 ⇒ 无锁。
6. **v2 候选**：NaN-boxing 压到 8 字节——代价是 int 精度受限（旧 LLVM 后端
   统一 double 语义，解释器保留 i64），评估后再定。

## 4. 里程碑

- **M1 ✅** 手写词法器 + arena/value 内存核心（10 测试）
- **M2 ✅** 手写 AST + 完整解析器：examples/01-17 + project 共 20 文件
  全量 parse 通过（固化为集成测试 examples.rs）；17 解析器单测；
  语法面：流签名/分叉/类/need/注解/智能引用/数组字面量/多返回值/二进制库流；
  **关键字表 = 最新设计（29 个）**：get/this/int/float/double/string/char/
  bool/true/false 均为关键字（用户亲自设计，2026-08-23 确认）
- **M3 ✅（2026-08-23）** 解释器 bbb-vm：流注册表（签名回退分叉、裸调用先当前流后全局）、
  Frame 作用域、res/ref/get/cause 位测试、多值返回→数组、注入 Array/Vector
  Bio 类（底层 Solid，Rust 实现）、need 校验（单/多文件）、项目目录模式
  （package.toml + src/ + utils/）；内置流 CIO/SIO/FIO/Com/Time/Obj/Solid/Arrays；
  **examples 01-08 + 12 + 13 与 bin/bio 输出逐字节一致，project 多文件跑通**；
  12 个 vm 回归测试固化。
  遗留（下一轮）：09/10 协作线程（需可暂停执行模型）、11 智能引用语义、
  14 二进制库 dlopen、15/16 注解（@call/@ucall/@onlyread/@unfork）
- **M4** LLVM 后端：AST→IR 文本（参考旧 `src/llvm.c`），统一 double 语义，
  `bio llvm` 子命令 + 独立 `bbb-llvm` 产物
- **M5** 回归：examples/01-17 输出 diff（与 bin/bio 解释器对比）+ 项目示例
  + 二进制库互操作（`Stream m & "libm.so"`）
- **M6** 性能：autotest 基准对标旧 C（update ~9.3ms 级），release LTO 优化

## 5. 参考与回归基准

- 旧 C 实现：`git show 891add2^:src/<file>`（interp.c 1101 行 / llvm.c 1321 行 /
  pack.c 545 行 / builtin.c 845 行…）
- 预编译解释器：`bin/bio`（跑 examples 的对照基准）
- BiolStudio（`/home/jack/Projects/BiolStudio`）：Python 功能层已能驱动
  `bin/bio` 解释器，Rust 实现就位后可平滑切换
- 标准层铁律：examples/ 与 DESIGN.md 是契约，实现层禁止改动

## 6. 注意事项

- 语法细节一律以 examples + 旧 C 源码为准（注释/字符串/char/数字词法、
  need 语义、智能引用 7 权限 × 4 层级 = 28 型、Solid 头指针语义…）
- 手写 AST（ast.rs）是唯一事实源
- 切换 Rust 工具链用 `~/.cargo/bin/cargo`（本机 PATH 未含）
