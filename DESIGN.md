# Biolang

## 引擎

流

1. Unistream ---- 统一流，一个抽象概念
2. Comstream ---- 计算流，瞬时流的一个分支，用来处理各种瞬时计算。
3. Remstream ---- 记忆流，程序运行时记住一些东西，默认拥有方法持久化和读取记忆（用户可以覆盖持久化和读取记忆方法）。
4. Objstream ---- 对象流，记忆流的一个分支，专门用来处理各种对象之间的关系和数据，每个对象也是一个流，储存了对象的各种属性（如果属性被冲走，那么调用某些方法就会直接发出拒绝）。
5. Threadstream ---- 进程流，一个进程通常包含计算流、记忆流、时间流、IO流，进程流可以切出多个进程。
6. Timestream ---- 计时流，可以同时拥有多个计时器，默认第一个计时器归线程所有，计时器还可以归零，但第一个并不允许，分叉出的第二个三个就允许。
7. IOStream ---- IO流，默认存在，可以读取和输出。
8. CustomStream ---- 自定义流，流并不是只有上面几种，而是可以组合和与二进制程序结合的。
9. Mainstream ---- 主程序流，只要一个程序是主程序就会直接调用这个流的exec()方法。
10. Functionstream ---- 函数流，支持分叉多个函数，这直属Unistream。在本语言中Function的意思是共用的快速的工具箱。
11. Constantstream ---- 常量流，支持分叉多个常量，这可属Unistream和Threadstream。术语分别叫公共常量、私有常量。
12. Areastream ---- 作用域流，包含记忆流。
13. Taskmstream ---- 自动进行任务调度的管理器流，包含Threadstream
14. Solidstream ----　连续流，自动存储连续的数据，可以自动分配移动头指针。 (普通数组类和Vector类都是在Bio代码里实现的，而非解释器里)
15. CIOStream ---- IOStream的Console实现
16. FIOStream ---- IOStream的文件实现
17. SIOStream ---- SIOStream的字符串实现。

## 语法

操作

定义变量修饰

```
const int x = 10; // Constantstream里加入东西。
int x = 10; // 默认在作用域流里加入东西。
thread int x = 10; // 线程变量。
```

基础类型

```
int
float
double
string
char
```

Biolang里的所有操作都是请求，请求自然是可以拒绝的，我们用以下语法表示：

```
ref ...; // 这里的...可以是任何变量
```

你的请求一般都是有回应的，我们的函数要显示调用回应和拒绝，以以下语法表示:

```
res add(a, b); // add(a,b)的结果
cause add(a, b); // add(a,b)拒绝的原因
ALL result = add(a, b); // ALL是一种包含res和cause的结构。
res result;
cause result;
// 和上面的差不多但更灵活
```

流语法

自定义流

```
Stream SStream {
    void hello(); // 签名
    int flag; // 分叉流时可以自定义。
}

```

自定义类（类在Biolang本质也是一种流）

```
Class CClass{ // 这里的Class是分叉Class流的意思
    void __init__() {
        ...
    }
    int n; // 默认对象类型
    int[] a; // 默认对象泛型
    /*
    或:
    type T; // 接近于泛型，但更灵活。
    T n;
    T[] a;
    */
}
```

创建对象

```
CClass c = new CClass();
```

调用流

```
stream::hello();
```

分叉流

```
SStream SSStream {
    void hello() {
        IO::println("Hello World");
    }
}
```

主程序流

```

program main; // 声明这是个主程序

Main { // 这种方式是直接覆盖一个已经存在的流
    void exec() { // exec()支持动态返回结果值，因为他们接受返回值用的是所有对象的初始流Objastream。
        IO::print("Hello World!");
    }
}
```

声明语法

```
program main;
```

```
program utils; // 声明这是个工具箱。
```

假设语法

假设流

```

need Stream ... { ... } ; // 第一个空是stream的名称，第二个空是需要的东西，这种语法必须与工具箱结合。

```

假设量

```
need value ...; // 第一个空是量的名称。
```

假设类

```
need Class ... {...};
```

假设函数

```
need function ...; // 第一个空是函数的名称
```

第三个和第二个本质上都是第一个假设流的变体。

假设语法是一种特殊的独立的语法。

智能引用

r, w, rw, m(可以移动) \* u (整体), f (方法), a （作用域）

```
realme &r u int
realme &w u int
realme &r f int
realme &m f int
...
```

## 方式

本语言可以解释也可以编译。
