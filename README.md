# BioLang — 解释器原型 (C)

Biolang 语言的 C 实现（仅解释器，编译器以后再说）。命令行工具名为 **`bio`**。

## 构建与运行

```bash
make              # 编译 → ./bio
make install      # 安装到 ~/.local/bin/bio

bio               # 跑内置 12 个演示
bio 程序.bl       # 运行 BioLang 源文件
bio --tokens x.bl # 查看词法分析结果（调试用）
```

## 代码结构（多模块）

```
src/
├── bio.h        # 公共头：类型定义 + 模块 API
├── arena.c      # 简易内存池（aalloc/astrdup）
├── lexer.c      # 词法分析（tokenize）
├── parser.c     # 语法分析（AST 构建，公开 parse_program_tokens）
├── value.c      # 值 / 请求结果模型（res/ref）
├── builtin.c    # 内置流（CIO/FIO/SIO/IO/Com/Time/Rem/Solid/Array/Ref/…）
├── bts.c        # Threads 协作式线程 + Taskm 任务调度
├── interp.c     # 解释器（流注册表、求值、假设检查、run_source）
└── main.c       # 入口 + 内置演示
```

## VSCode 插件

目录：`vscode/biolang-vscode/`（已安装到 `~/.vscode/extensions/bio.biolang-0.2.1`）

- **语法高亮**：`.bl` / `.bio` 文件（关键字含 `cause`/类型 int·float·double·string·char/字符串/数字/调用/注释/运算符/智能引用 `&r u x`）
- **代码片段**：main 骨架、stream 签名（参数 `a int`）、fork 分叉、class、res/ref cause、need（含 Class）、sref 智能引用、bins/bincall 二进制库、spawn 线程、taskm、if/while/for
- **补全**：`流::` 方法补全（内置 CIO/FIO/SIO/Array/Threads/Taskm/Ref/Console + 文档中声明的流）、`&` 智能引用权限 r/w/rw → 跟随层 u/m/a 两级提示
- **运行命令**：`BioLang: 运行当前文件 (bio)`（编辑器右上角按钮 + 命令面板）打开 **Webview 交互式面板**——程序输出实时显示，底部输入框可直接向 CIO 提供多次输入（Enter 发送）；`BioLang: 运行内置演示` 直接跑
- 重启 VS Code（或 "Developer: Reload Window"）后生效；运行命令依赖 `bio` 在 PATH（`~/.local/bin/bio`）

## 已实现的语言特性

| 特性 | 语法 | 示例 |
|---|---|---|
| 主程序 | `program main;` + `Main { void exec() { ... } }` | `Main { void exec() { CIO::println("Hi"); } }` |
| 流签名 | `Stream S { int add(a int, b int); }`（参数语法唯一：名称 类型） | `Stream Calc { int add(a int, b int); }` |
| 流分叉 | `S Impl { void m(...) { ... } }` | `Calc MyCalc { void add(a int, b int) { res a + b; } }` |
| 类声明 | `Class C { void __init__() {...} int hp; int[] a; }` | 本质也是流，方法不需要 overwrite |
| **字段声明** | 类/流可声明属性，**方法和字段任意顺序交错**，支持逗号分隔：`int x, y;` / `int[] a;` / `string s;` / `type T; T n; T[] a;`（泛型风格）。`new` 时按类型物化默认值（数值 0 / 字符串 "" / 数组 []） | `Class Vector { int x, y; }` → `v.x`、`v::x`、`this::x` 都可读写 |
| **new 实例化** | `new Class(args...)` → 分叉类流 + 自动调 `__init__(args...)`，返回对象（Result 包）；**无任何特判**，所有类（含内置 Array）同一条路 | `ALL h = new Hero("TAK", 88);`、`ALL a = new Array(3);` → `h.hp`、`h::getName()` |
| **对象模型** | 对象方法里 `this` = 对象本身；`Obj::new/get/set/forget/call/class`；对象流调用 `obj::method()`。方法内**裸名赋值**命中已声明字段 → 写实例属性（`x = x;` 即 `this::x = x;`），读取优先取参数/本地 | `Obj::set(this, "hp", 88);`、`ALL nm = h::getName();`、`__init__(x int) { x = x; }` |
| **流/对象作为参数** | 流是一等值，可作方法参数并调用其方法；也支持智能引用修饰参数 `&权限 跟随 名 类型` | `void show(cio CIO) { cio::println(...); }`、`v::show(CIO)`；`void show(&w f io IO) { io::println(...); }`、`v::show(&w f CIO)`；`void add(&r f vec Vector) { this::x += vec::x; }` |
| 请求/回应/拒绝 | `res expr;` 回应；`cause expr;` 拒绝（原稿语法）；兼容 `ref cause ...;` | 方法内 `res a + b;` 或 `cause "除数不能为 0";` |
| **多返回类型** | 方法可声明返回类型 `void/int/float/double/string/char`（可带 `[]` 数组），**所有流统一**：签名流/分叉/Class/Main | `int add(a int, b int) { res a + b; }`、`string[] titles() { res "勇者", "传说"; }` |
| **res 多值** | `res a, b, c;` 逗号分隔 → 返回数组 | `res 1, 2, 3;` → `[1, 2, 3]`；单值 `res x;` 保持原样 |
| 请求结果 | `ALL x = 流::方法(...);` | `ALL r = MyCalc::add(3, 4);` |
| 结果解包 | **`res X` / `cause X` 前缀提取运算符**（原稿语法：`res add(a,b)` 取结果、`cause add(a,b)` 取拒绝原因），可作表达式或语句；`.res` / `.cause` 属性等价；`res r;` / `cause r;` 语句级转发 | `length = res IO::readInt();`、`ALL why = cause Calc::div(1,0);`、`r.res`、`bad.cause`；`res r;` 转发成功值、`cause r;` 转发拒绝原因 |
| **拒绝传播** | 被拒的 Result 作为参数 → 请求随之被拒 | `MyCalc::add(1, bad)` → ref |
| 控制流 | `if/else if/else`、`while`、`for(;;)`、`break`、`continue` | 条件真值：0/空串/被拒 = 假 |
| **子流** | **IO** IOStream 通用流（默认存在，核心方法 `println`/`readln`/`write`/`read`）/ **CIO** IO 的 Console 实现（+ 预置分叉 Console）/ **FIO** IO 的文件实现 / **SIO** IO 的字符串实现 / **Com** Comstream 计算流。IO 聚合 CIO/FIO/SIO | `IO::println` `IO::readln` `IO::write` `IO::read` `FIO::readFile` `SIO::format` |
| **Com 计算流** | 瞬时流的分支，处理各种瞬时计算：`abs/min/max/pow/sqrt/floor/ceil/round/sign/sin/cos/tan/log/exp` | `Com::abs(0-5).res`、`Com::pow(2, 10).res` |
| **Timestream 计时流** | 同时拥有多个计时器；默认**第一个计时器归线程所有，不允许归零**，`Time::fork()` 分叉出的允许归零；`now/sleep/start/fork/elapsed/reset` | `Time::start()`（线程首计时器）；`ALL t = Time::fork(); Time::reset(t.res);` |
| 裸函数调用 | `add(a, b)` — 全局搜索提供该方法的流 | `res add(a, b);`（spec 风格） |
| 默认返回 | 无显式 res/ref → 默认 `ref(无)`，if 视为假 | `ALL x = f(); if (x) {...}` → 走 else |
| 假设 | `need value/function/stream/Class ...;`（`need Stream`/`need Class` 可带 `{...}` body，原稿语法） | 未满足 → 拒绝运行 |
| **变量修饰** | `const int x = 10;` → Constantstream（只读，重复声明/修改拒绝，方法内与**顶层**均可）；`int x = 10;` → 作用域流；`thread int x = 10;` → 线程变量（线程作用域） | `const int PI = 3;` 改 PI → 拒绝「常量不能修改」 |
| 运算 | `+ - * / == != < > <= >=`、数字、字符串、变量；**复合赋值** `+= -= *= /= %=`（变量、`this::属性`、数组元素均可）；**自增自减** `i++` / `i--`；**数组下标** `a[i]` 读写 | `ALL y = x.res * 2 + 1;`、`this::n += 1;`、`for (int i = 0; i < n; i++) { a[i] = i; }` |
| 基本类型 | `int` `float` `double` `string` `char`（字段：`int n;` 类型 对象） | |
| **数组/Vector（Bio 类）** | Array/Vector 是 **Bio 代码实现的类**（非解释器内置），底层调 Solid 连续流；`new Array(n)`/`new Vector()` 创建；**数组字面量** `new type[n]` **类型通用**（基本类型与自定义类均可）；方法 `__init__/len/get/set/push/pop/clear/join` | `ALL a = new Array(3); a::set(0, 10); a::push(40); a::len().res; a::join("-").res`；`int[] a = new int[12];`、`ALL hs = new Hero[3];` |
| **Solid 连续流** | 连续存储 + 自动分配 + **移动头指针**；`new/len/get/set/push/pop/read/peek/head/resetHead/clear/join` | `ALL s = Solid::new().res; Solid::read(s).res`（头指针前进） |
| **Arrays 集合流** | 包含所有 Array/Vector 实例；new 一个 Array 默认插入（__init__ 里 `Arrays::add(this)`，Bio 代码可见）；`count/all/get/add/forget`；动态数组 `vector()` | `Arrays::count().res`、`ALL v = Arrays::vector(); v::push(10);` |
| **二进制库流** | `Stream 名 & "文件.so" {}`；库导出函数自动成为流方法；`&func(...)` 全局二进制调用 | `Stream m & "libm.so" {}` → `m::sin(1.0)`；`&pow(2,10)` |
| **Threads 线程** | `Threads::spawn("方法", 参数...)` / `yield` / `join` / `active` / `self` | 协作式用户线程（ucontext），线程执行裸方法调用 |
| **Taskm 任务管理** | `Taskm::add/interval/run/stop/active` | 自动轮转所有任务直到完成，`interval(毫秒)` 设轮转间隔 |
| **智能引用** | `&权限 跟随 真名`（权限 r/w/rw/m × 跟随 u/f/a）+ `Ref::read/write/move/target/perm`；引用变量声明 `<名字> &权限 跟随 [类型] [= 初值];`（原稿 realme 写法） | `&r u counter` 只读跟随程序级；`&r f x` 只读跟随方法；`&w a note` 写跟随作用域；`&m f x` 可移动 + `Ref::move(mv)` 取走目标；`count &w u int = 5;` |
| **作用域链** | 方法小 Stream → 线程/主线程作用域(area) → 程序级(Unistream)，各自带记忆流 | 线程 a 层隔离；u 层程序级共享 |

> 二进制函数调用约定：原型按 `double(*)(double,...)` 调用（最多 6 个参数，仅数值参数），字符串/指针参数暂不支持。
>
> Threads 为协作式线程：`yield()` 让出；线程内 `join` 未完成目标暂不支持。

## 内置演示

1. Hello World
2. 流分叉 + res/ref 请求模型（回应、拒绝、缺方法）
3. need 假设未满足 → 拒绝运行
4. Class 声明 + 传感器流读数
5. if / for / while 控制流（求和、阶乘、continue/break、else if、for(;;)）
6. 子流 CIO/FIO/SIO + 裸调用 + ref cause + 默认 ref(无)
7. Array 类（new Array + 对象方法）+ Threads 线程（阶乘线程 + 协作 countUp）
8. Taskm 任务管理器（自动轮转求和/2^n）
9. 智能引用 &权限 跟随 真名（r/w/rw × u/f/a）+ 线程作用域隔离
10. 多返回类型（int/string/int[]）+ res 多值 + Class 方法
11. new 语法分叉类流 + 自动 __init__ + this + 对象方法（Obj::call / h::getName）
12. 流内部裸调用 + this::属性 + Arrays 集合/Vector
13. IO 聚合 / Com 计算流 / 引用变量声明 / Ref::move / Time fork / res·cause 解包

## 未实现（编译器阶段再说）

- 编译到目标代码（本语言可以解释也可以编译，当前仅解释器）
- `Threadstream`（进程流）/ `Areastream` 等作为显式流类型的声明语法（当前 area 仅作为隐式作用域层存在）
