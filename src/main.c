#include "bio.h"

/* 入口 + 演示 */
/* ═══════════════ 演示 ═══════════════ */
const char *DEMO1 =
    "program main;\n"
    "Main { void exec() { IO::println(\"Hello World!\"); } }\n";

const char *DEMO2 =
    "program main;\n"
    "Stream Calc { int add(a int, b int); int div(a int, b int); }\n"
    "Calc MyCalc {\n"
    "    void add(a int, b int) { res a + b; }\n"
    "    void div(a int, b int) { ref cause \"拒绝：除数不能为 0\"; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = MyCalc::add(3, 4);\n"
    "        CIO::println(\"3 + 4 =\", r.res);\n"
    "        ALL bad = MyCalc::div(1, 0);\n"
    "        CIO::println(\"div 的拒绝原因:\", bad.cause);\n"
    "        ALL missing = MyCalc::sqrt(9);\n"
    "        CIO::println(\"缺失方法的拒绝:\", missing.cause);\n"
    "    }\n"
    "}\n";

const char *DEMO3 =
    "program utils;\n"
    "need value PI;\n"
    "need function sqrt;\n"
    "Main { void exec() { CIO::println(\"不会执行到这里\"); } }\n";

const char *DEMO4 =
    "program bio;\n"
    "Stream Sensor { int read(); }\n"
    "Class Cell {\n"
    "    void __init__() { CIO::println(\"Cell 初始化\"); }\n"
    "    int hp;\n"
    "    int[] a;\n"
    "}\n"
    "Sensor T1 { void read() { res 42; } }\n"
    "Main { void exec() { ALL v = T1::read(); CIO::println(\"传感器读数:\", v.res); } }\n";

const char *DEMO5 =
    "program main;\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL sum = 0;\n"
    "        ALL i = 1;\n"
    "        while (i <= 10) {\n"
    "            sum = sum + i;\n"
    "            i = i + 1;\n"
    "        }\n"
    "        CIO::println(\"1..10 求和(while):\", sum);\n"
    "        ALL fac = 1;\n"
    "        for (ALL k = 1; k <= 5; k = k + 1;) {\n"
    "            fac = fac * k;\n"
    "        }\n"
    "        CIO::println(\"5! (for):\", fac);\n"
    "        for (ALL j = 1; j <= 10; j = j + 1;) {\n"
    "            if (j == 4) { continue; }\n"
    "            if (j > 7) { break; }\n"
    "            CIO::println(\"j =\", j);\n"
    "        }\n"
    "        ALL n = 3;\n"
    "        if (n > 0) { CIO::println(\"n 是正数\"); }\n"
    "        else if (n < 0) { CIO::println(\"n 是负数\"); }\n"
    "        else { CIO::println(\"n 是零\"); }\n"
    "        ALL total = 0;\n"
    "        for (;;) {\n"
    "            total = total + 1;\n"
    "            if (total >= 3) { break; }\n"
    "        }\n"
    "        CIO::println(\"for(;;) 计数:\", total);\n"
    "    }\n"
    "}\n";

static const char *DEMO6 =
    "program main;\n"
    "Stream Calc { int add(a int, b int); int div(a int, b int); }\n"
    "Calc MyCalc {\n"
    "    void add(a int, b int) { res a + b; }\n"
    "    void div(a int, b int) { ref cause \"拒绝：除数不能为 0\"; }\n"
    "    void noReturn(a int) { CIO::println(\"（这个方法没有显式返回）\"); }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = add(3, 4);            // 裸调用：全局找 add\n"
    "        CIO::println(\"裸调用 add(3,4) =\", r.res);\n"
    "        ALL bad = div(1, 0);          // 裸调用 + ref cause\n"
    "        CIO::println(\"div 拒绝原因:\", bad.cause);\n"
    "        ALL nr = MyCalc::noReturn(1); // 无显式返回 → 默认 ref(无)\n"
    "        CIO::println(\"默认返回:\", nr);\n"
    "        if (nr) { CIO::println(\"if 认为真\"); }\n"
    "        else { CIO::println(\"if 认为假（默认 ref 无）\"); }\n"
    "        // 子流：CIO / FIO / SIO\n"
    "        CIO::println(\"CIO 控制台输出\");\n"
    "        ALL s = SIO::format(\"%d + %d = %d\", 2, 3, 5);\n"
    "        CIO::println(\"SIO::format →\", s.res);\n"
    "        ALL up = SIO::upper(\"hello\");\n"
    "        CIO::println(\"SIO::upper →\", up.res);\n"
    "        FIO::writeFile(\"/tmp/bio_test.txt\", \"你好 BioLang!\");\n"
    "        ALL content = FIO::readFile(\"/tmp/bio_test.txt\");\n"
    "        CIO::println(\"FIO 读回:\", content.res);\n"
    "    }\n"
    "}\n";

static const char *DEMO7 =
    "program main;\n"
    "Calc Worker {\n"
    "    void factorial(n int) {\n"
    "        ALL f = 1;\n"
    "        ALL i = 1;\n"
    "        while (i <= n) { f = f * i; i = i + 1; }\n"
    "        res f;\n"
    "    }\n"
    "    void countUp(n int) {\n"
    "        ALL i = 0;\n"
    "        while (i < n) { i = i + 1; Threads::yield(); }\n"
    "        res i;\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // Array 数组流\n"
    "        ALL a = new Array(3);\n"
    "        a::set(0, 10); a::set(1, 20); a::set(2, 30);\n"
    "        CIO::println(\"数组:\", a);\n"
    "        a::push(40);\n"
    "        CIO::println(\"push 后:\", a, \" 长度:\", a::len().res);\n"
    "        CIO::println(\"join(-):\", a::join(\"-\").res);\n"
    "        // Threads 线程系统\n"
    "        ALL t1 = Threads::spawn(\"factorial\", 10).res;\n"
    "        ALL t2 = Threads::spawn(\"countUp\", 5).res;\n"
    "        CIO::println(\"存活线程:\", Threads::active().res);\n"
    "        ALL r1 = Threads::join(t1);\n"
    "        CIO::println(\"线程\", t1, \" 10! =\", r1.res);\n"
    "        ALL r2 = Threads::join(t2);\n"
    "        CIO::println(\"线程\", t2, \" countUp =\", r2.res);\n"
    "    }\n"
    "}\n";

static const char *DEMO8 =
    "program main;\n"
    "Calc Worker {\n"
    "    void jobA(n int) {\n"
    "        ALL s = 0;\n"
    "        ALL i = 1;\n"
    "        while (i <= n) { s = s + i; i = i + 1; Threads::yield(); }\n"
    "        res s;\n"
    "    }\n"
    "    void jobB(n int) {\n"
    "        ALL p = 1;\n"
    "        ALL i = 1;\n"
    "        while (i <= n) { p = p * 2; i = i + 1; Threads::yield(); }\n"
    "        res p;\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        Taskm::interval(1);\n"
    "        ALL t1 = Taskm::add(\"jobA\", 5).res;\n"
    "        ALL t2 = Taskm::add(\"jobB\", 4).res;\n"
    "        CIO::println(\"任务数:\", Taskm::active().res);\n"
    "        Taskm::run();\n"
    "        CIO::println(\"任务完成数:\", Taskm::active().res);\n"
    "        CIO::println(\"jobA 求和 =\", Threads::join(t1).res);\n"
    "        CIO::println(\"jobB 2^4 =\", Threads::join(t2).res);\n"
    "    }\n"
    "}\n";

static const char *DEMO9 =
    "program main;\n"
    "Calc Worker {\n"
    "    void threadJob(n int) {\n"
    "        ALL wa = &w a local_note;\n"
    "        Ref::write(wa, n * 2);\n"
    "        ALL ra = &r a local_note;\n"
    "        CIO::println(\"线程\", Threads::self().res, \" a 层 =\", Ref::read(ra).res);\n"
    "        res Ref::read(ra).res;\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // 智能引用 &权限 跟随 真名（权限 r/w/rw × 跟随 u/m/a）\n"
    "        ALL wr = &w u counter;\n"
    "        Ref::write(wr, 10);\n"
    "        ALL rr = &r u counter;\n"
    "        CIO::println(\"u 读 counter =\", Ref::read(rr).res);\n"
    "        CIO::println(\"只读写 →\", Ref::write(rr, 99).cause);\n"
    "        ALL x = 42;\n"
    "        ALL rf = &r f x;\n"
    "        CIO::println(\"f 读 x =\", Ref::read(rf).res);\n"
    "        // 线程作用域隔离（a 跟随）\n"
    "        ALL t1 = Threads::spawn(\"threadJob\", 21).res;\n"
    "        ALL t2 = Threads::spawn(\"threadJob\", 22).res;\n"
    "        CIO::println(\"t1 =\", Threads::join(t1).res, \" t2 =\", Threads::join(t2).res);\n"
    "    }\n"
    "}\n";

static const char *DEMO10 =
    "program main;\n"
    "Stream Math {\n"
    "    int add(a int, b int);\n"
    "    string greet(name string);\n"
    "    int[] triple(a int);\n"
    "}\n"
    "Math M {\n"
    "    int add(a int, b int) { res a + b; }\n"
    "    string greet(name string) { res name; }\n"
    "    int[] triple(a int) { res a, a * 2, a * 3; }\n"
    "}\n"
    "Class Hero {\n"
    "    void __init__() { CIO::println(\"英雄登场\"); }\n"
    "    int hp;\n"
    "    int getHp() { res 100; }\n"
    "    string[] titles() { res \"勇者\", \"屠龙者\", \"传说\"; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = M::add(3, 4);\n"
    "        CIO::println(\"add(3,4) =\", r.res);\n"
    "        ALL g = M::greet(\"TAK\");\n"
    "        CIO::println(\"greet =\", g.res);\n"
    "        ALL t = M::triple(10).res;\n"
    "        CIO::println(\"triple(10) =\", t, \" 第 2 个:\", t::get(1).res);\n"
    "        ALL h = Hero::getHp();\n"
    "        CIO::println(\"Hero::getHp() =\", h.res);\n"
    "        ALL ts = Hero::titles();\n"
    "        CIO::println(\"Hero::titles() =\", ts.res);\n"
    "    }\n"
    "}\n";

static const char *DEMO11 =
    "program main;\n"
    "Class Hero {\n"
    "    void __init__(name string, hp int) {\n"
    "        Obj::set(this, \"name\", name);\n"
    "        Obj::set(this, \"hp\", hp);\n"
    "        CIO::println(\"英雄登场:\", name);\n"
    "    }\n"
    "    int getHp() { res 100; }\n"
    "    string getName() { res Obj::get(this, \"name\").res; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL h = new Hero(\"TAK\", 88);   // new 语法：分叉类流 + 自动调 __init__\n"
    "        CIO::println(\"对象:\", h);\n"
    "        CIO::println(\"name =\", h.name, \" hp =\", h.hp);   // 属性直接访问（不解包）\n"
    "        ALL hp = Obj::call(h, \"getHp\");\n"
    "        CIO::println(\"Obj::call getHp() =\", hp.res);\n"
    "        ALL nm = h::getName();\n"
    "        CIO::println(\"h::getName() =\", nm.res);\n"
    "        ALL h2 = new Hero(\"Lily\", 66);\n"
    "        CIO::println(\"h2.hp =\", h2.hp, \"（实例独立）\");\n"
    "        CIO::println(\"h.hp 仍 =\", h.hp);\n"
    "    }\n"
    "}\n";

static const char *DEMO12 =
    "program main;\n"
    "Stream Counter {\n"
    "    int count;\n"
    "    void reset() { this::count = 0; }\n"
    "    void bump() { this::count = count + 1; }   // 内部裸读属性\n"
    "    int get() { res count; }                   // 内部裸调用/裸属性\n"
    "    int doubleGet() { res get() + get(); }     // 内部裸调用方法\n"
    "}\n"
    "Counter C {\n"
    "    void reset() { this::count = 0; }\n"
    "    void bump() { this::count = count + 1; }\n"
    "    int get() { res count; }\n"
    "    int doubleGet() { res get() + get(); }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        C::reset();\n"
    "        C::bump(); C::bump(); C::bump();\n"
    "        CIO::println(\"count =\", C::get().res, \" doubleGet =\", C::doubleGet().res);\n"
    "        // Arrays：new 的 Array/Vector 自动注册\n"
    "        ALL v = Arrays::vector();\n"
    "        v::push(10); v::push(20); v::push(30);\n"
    "        CIO::println(\"vector =\", v, \" len =\", v::len().res);\n"
    "        ALL a = new Array(3);\n"
    "        CIO::println(\"new Array(3) =\", a);\n"
    "        CIO::println(\"Arrays 实例数 =\", Arrays::count().res);\n"
    "        ALL xs = Arrays::all().res;\n"
    "        CIO::println(\"Arrays::get(0) =\", Arrays::get(0).res, \" all =\", xs::len().res);\n"
    "        Arrays::forget(v);\n"
    "        CIO::println(\"forget 后 =\", Arrays::count().res);\n"
    "    }\n"
    "}\n";

static const char *DEMO13 =
    "program main;\n"
    "Stream Calc {\n"
    "    int div(a int, b int);\n"
    "}\n"
    "Calc MyCalc {\n"
    "    int div(a int, b int) {\n"
    "        if (b == 0) { cause \"除数不能为 0\"; }\n"
    "        res a / b;\n"
    "    }\n"
    "    int forwardDiv(a int, b int) {\n"
    "        ALL r = MyCalc::div(a, b);\n"
    "        res r;            // res r; 解包：r 成功 → 取其 res\n"
    "    }\n"
    "    string forwardBad(a int, b int) {\n"
    "        ALL r = MyCalc::div(a, b);\n"
    "        cause r;          // cause r; 解包：r 被拒 → 转发拒绝原因\n"
    "        res \"不会到这里\";\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // IOStream：聚合 CIO/FIO/SIO\n"
    "        IO::println(\"IO 聚合输出\");\n"
    "        ALL s = IO::format(\"%d + %d = %d\", 2, 3, 5);\n"
    "        IO::println(\"IO::format →\", s.res);\n"
    "        ALL up = IO::upper(\"hello\");\n"
    "        IO::println(\"IO::upper →\", up.res);\n"
    "        // Comstream 计算流\n"
    "        ALL neg = 0 - 5;\n"
    "        IO::println(\"Com::abs(0-5) =\", Com::abs(neg).res, \" Com::sqrt(9) =\", Com::sqrt(9).res);\n"
    "        ALL p = Com::pow(2, 10);\n"
    "        IO::println(\"Com::pow(2,10) =\", p.res);\n"
    "        // 引用变量声明（原稿 realme &权限 跟随 类型）\n"
    "        count &w u int = 5;      // 程序级(u)可写引用 count，初值 5 写入 u 层\n"
    "        ALL rc = &r u count;     // 表达式智能引用：读 u 层 count\n"
    "        IO::println(\"count 声明后 Ref::read =\", Ref::read(rc).res);\n"
    "        Ref::write(count, 8);\n"
    "        IO::println(\"Ref::write(count,8) 后 =\", Ref::read(rc).res);\n"
    "        dry &r a int;            // 无初值声明：目标同名(dry)，直接读 → 拒绝\n"
    "        IO::println(\"无初值声明读 →\", Ref::read(dry).cause);\n"
    "        // 可移动权限 m + Ref::move\n"
    "        ALL x = 42;\n"
    "        ALL mv = &m f x;         // 方法级可移动引用\n"
    "        IO::println(\"move 前 x =\", Ref::read(mv).res);\n"
    "        ALL taken = Ref::move(mv);\n"
    "        IO::println(\"Ref::move 取走 =\", taken.res);\n"
    "        IO::println(\"移动后再读 x →\", Ref::read(mv).cause);\n"
    "        // Timestream：fork 分叉计时器 + 首计时器保护\n"
    "        Time::start();           // 线程默认首个计时器\n"
    "        ALL t2 = Time::fork();   // 分叉出的第二个计时器，允许归零\n"
    "        IO::println(\"Time::reset(首计时器) →\", Time::reset().cause);\n"
    "        Time::reset(t2.res);     // 允许\n"
    "        IO::println(\"Time::reset(分叉计时器) 成功\");\n"
    "        // res r; / cause r; 解包\n"
    "        ALL good = MyCalc::forwardDiv(10, 2);\n"
    "        IO::println(\"forwardDiv(10,2) 解包 =\", good.res);\n"
    "        ALL bad = MyCalc::forwardBad(1, 0);\n"
    "        IO::println(\"forwardBad(1,0) 拒绝原因 =\", bad.cause);\n"
    "    }\n"
    "}\n";

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--tokens") == 0 && argc > 2) {
        FILE *f = fopen(argv[2], "r");
        if (!f) return 1;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = aalloc(sz + 1);
        if (fread(buf, 1, sz, f) != (size_t)sz) {}
        buf[sz] = 0; fclose(f);
        int n; Tok *toks = tokenize(buf, &n);
        for (int i = 0; i < n; i++)
            printf("%s ", toks[i].kind == T_NUM ? "(num)" : toks[i].text);
        printf("\n");
        return 0;
    }
    printf("🧬 Biolang 解释器原型 (C)\n\n");
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { fprintf(stderr, "cannot open file: %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = aalloc(sz + 1);
        if (fread(buf, 1, sz, f) != (size_t)sz) { /* 忽略 */ }
        buf[sz] = 0;
        fclose(f);
        run_source(buf);
        return 0;
    }
    printf("═══ 演示 1：Hello World ═══\n");
    run_source(DEMO1);
    printf("\n═══ 演示 2：流分叉 + res/ref 请求模型 ═══\n");
    run_source(DEMO2);
    printf("\n═══ 演示 3：need 假设未满足 → 拒绝运行 ═══\n");
    run_source(DEMO3);
    printf("\n═══ 演示 4：Class 声明 + 传感器流 ═══\n");
    run_source(DEMO4);
    printf("\n═══ 演示 5：if / for / while 控制流 ═══\n");
    run_source(DEMO5);
    printf("\n═══ 演示 6：子流 CIO/FIO/SIO + 裸调用 + 默认 ref(无) ═══\n");
    run_source(DEMO6);
    printf("\n═══ 演示 7：Array 数组流 + Threads 线程系统 ═══\n");
    run_source(DEMO7);
    printf("\n═══ 演示 8：Taskm 任务管理器（自动轮转 + 间隔）═══\n");
    run_source(DEMO8);
    printf("\n═══ 演示 9：智能引用 &权限 跟随 真名 + 线程作用域 ═══\n");
    run_source(DEMO9);
    printf("\n═══ 演示 10：多返回类型（int/string/int[]）+ res 多值 ═══\n");
    run_source(DEMO10);
    printf("\n═══ 演示 11：new 语法分叉类流 + 自动 __init__ + this/对象方法 ═══\n");
    run_source(DEMO11);
    printf("\n═══ 演示 12：流内部裸调用 + this::属性 + Arrays 集合/Vector ═══\n");
    run_source(DEMO12);
    printf("\n═══ 演示 13：IO 聚合 / Com 计算流 / 引用变量声明 / Ref::move / Time fork / res·cause 解包 ═══\n");
    run_source(DEMO13);
    return 0;
}

