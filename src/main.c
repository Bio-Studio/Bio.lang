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
    "    void div(a int, b int) { ref cause \"refused: division by zero\"; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = MyCalc::add(3, 4);\n"
    "        CIO::println(\"3 + 4 =\", r.res);\n"
    "        ALL bad = MyCalc::div(1, 0);\n"
    "        CIO::println(\"div cause:\", bad.cause);\n"
    "        ALL missing = MyCalc::sqrt(9);\n"
    "        CIO::println(\"missing method cause:\", missing.cause);\n"
    "    }\n"
    "}\n";

const char *DEMO3 =
    "program utils;\n"
    "need value PI;\n"
    "need function sqrt;\n"
    "Main { void exec() { CIO::println(\"this won't run\"); } }\n";

const char *DEMO4 =
    "program bio;\n"
    "Stream Sensor { int read(); }\n"
    "Class Cell {\n"
    "    void __init__() { CIO::println(\"Cell init\"); }\n"
    "    int hp;\n"
    "    int[] a;\n"
    "}\n"
    "Sensor T1 { void read() { res 42; } }\n"
    "Main { void exec() { ALL v = T1::read(); CIO::println(\"sensor reading:\", v.res); } }\n";

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
    "        CIO::println(\"1..10 sum (while):\", sum);\n"
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
    "        if (n > 0) { CIO::println(\"n is positive\"); }\n"
    "        else if (n < 0) { CIO::println(\"n is negative\"); }\n"
    "        else { CIO::println(\"n is zero\"); }\n"
    "        ALL total = 0;\n"
    "        for (;;) {\n"
    "            total = total + 1;\n"
    "            if (total >= 3) { break; }\n"
    "        }\n"
    "        CIO::println(\"for(;;) count:\", total);\n"
    "    }\n"
    "}\n";

static const char *DEMO6 =
    "program main;\n"
    "Stream Calc { int add(a int, b int); int div(a int, b int); }\n"
    "Calc MyCalc {\n"
    "    void add(a int, b int) { res a + b; }\n"
    "    void div(a int, b int) { ref cause \"refused: division by zero\"; }\n"
    "    void noReturn(a int) { CIO::println(\"(this method returns nothing)\"); }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = add(3, 4);            // bare call: global find add\n"
    "        CIO::println(\"bare add(3,4) =\", r.res);\n"
    "        ALL bad = div(1, 0);          // bare call + ref cause\n"
    "        CIO::println(\"div cause:\", bad.cause);\n"
    "        ALL nr = MyCalc::noReturn(1); // no explicit return → default ref(none)\n"
    "        CIO::println(\"default return:\", nr);\n"
    "        if (nr) { CIO::println(\"if is true\"); }\n"
    "        else { CIO::println(\"if is false (default ref none)\"); }\n"
    "        // substreams: CIO / FIO / SIO\n"
    "        CIO::println(\"CIO console output\");\n"
    "        ALL s = SIO::format(\"%d + %d = %d\", 2, 3, 5);\n"
    "        CIO::println(\"SIO::format →\", s.res);\n"
    "        ALL up = SIO::upper(\"hello\");\n"
    "        CIO::println(\"SIO::upper →\", up.res);\n"
    "        FIO::writeFile(\"/tmp/bio_test.txt\", \"Hello BioLang!\");\n"
    "        ALL content = FIO::readFile(\"/tmp/bio_test.txt\");\n"
    "        CIO::println(\"FIO read back:\", content.res);\n"
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
    "        // Array stream\n"
    "        ALL a = new Array(3);\n"
    "        a::set(0, 10); a::set(1, 20); a::set(2, 30);\n"
    "        CIO::println(\"array:\", a);\n"
    "        a::push(40);\n"
    "        CIO::println(\"after push:\", a, \" len:\", a::len().res);\n"
    "        CIO::println(\"join(-):\", a::join(\"-\").res);\n"
    "        // Threads system\n"
    "        ALL t1 = Threads::spawn(\"factorial\", 10).res;\n"
    "        ALL t2 = Threads::spawn(\"countUp\", 5).res;\n"
    "        CIO::println(\"live threads:\", Threads::active().res);\n"
    "        ALL r1 = Threads::join(t1);\n"
    "        CIO::println(\"thread\", t1, \" 10! =\", r1.res);\n"
    "        ALL r2 = Threads::join(t2);\n"
    "        CIO::println(\"thread\", t2, \" countUp =\", r2.res);\n"
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
    "        CIO::println(\"tasks:\", Taskm::active().res);\n"
    "        Taskm::run();\n"
    "        CIO::println(\"tasks done:\", Taskm::active().res);\n"
    "        CIO::println(\"jobA sum =\", Threads::join(t1).res);\n"
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
    "        CIO::println(\"thread\", Threads::self().res, \" a layer =\", Ref::read(ra).res);\n"
    "        res Ref::read(ra).res;\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // smart ref &perm follow name (perm r/w/rw × follow u/m/a)\n"
    "        ALL wr = &w u counter;\n"
    "        Ref::write(wr, 10);\n"
    "        ALL rr = &r u counter;\n"
    "        CIO::println(\"u read counter =\", Ref::read(rr).res);\n"
    "        CIO::println(\"read-only write →\", Ref::write(rr, 99).cause);\n"
    "        ALL x = 42;\n"
    "        ALL rf = &r f x;\n"
    "        CIO::println(\"f read x =\", Ref::read(rf).res);\n"
    "        // thread scope isolation (a follow)\n"
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
    "    void __init__() { CIO::println(\"hero appears\"); }\n"
    "    int hp;\n"
    "    int getHp() { res 100; }\n"
    "    string[] titles() { res \"brave\", \"dragon slayer\", \"legend\"; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = M::add(3, 4);\n"
    "        CIO::println(\"add(3,4) =\", r.res);\n"
    "        ALL g = M::greet(\"TAK\");\n"
    "        CIO::println(\"greet =\", g.res);\n"
    "        ALL t = M::triple(10).res;\n"
    "        CIO::println(\"triple(10) =\", t, \" 2nd:\", t::get(1).res);\n"
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
    "        CIO::println(\"hero appears:\", name);\n"
    "    }\n"
    "    int getHp() { res 100; }\n"
    "    string getName() { res Obj::get(this, \"name\").res; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL h = new Hero(\"TAK\", 88);   // new: fork class stream + auto __init__\n"
    "        CIO::println(\"object:\", h);\n"
    "        CIO::println(\"name =\", h.name, \" hp =\", h.hp);   // direct attribute access (no unwrap)\n"
    "        ALL hp = Obj::call(h, \"getHp\");\n"
    "        CIO::println(\"Obj::call getHp() =\", hp.res);\n"
    "        ALL nm = h::getName();\n"
    "        CIO::println(\"h::getName() =\", nm.res);\n"
    "        ALL h2 = new Hero(\"Lily\", 66);\n"
    "        CIO::println(\"h2.hp =\", h2.hp, \" (independent)\");\n"
    "        CIO::println(\"h.hp still =\", h.hp);\n"
    "    }\n"
    "}\n";

static const char *DEMO12 =
    "program main;\n"
    "Stream Counter {\n"
    "    int count;\n"
    "    void reset() { this::count = 0; }\n"
    "    void bump() { this::count = count + 1; }   // internal bare read\n"
    "    int get() { res count; }                   // internal bare call/attr\n"
    "    int doubleGet() { res get() + get(); }     // internal bare call\n"
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
    "        // Arrays: new Array/Vector auto-register\n"
    "        ALL v = Arrays::vector();\n"
    "        v::push(10); v::push(20); v::push(30);\n"
    "        CIO::println(\"vector =\", v, \" len =\", v::len().res);\n"
    "        ALL a = new Array(3);\n"
    "        CIO::println(\"new Array(3) =\", a);\n"
    "        CIO::println(\"Arrays count =\", Arrays::count().res);\n"
    "        ALL xs = Arrays::all().res;\n"
    "        CIO::println(\"Arrays::get(0) =\", Arrays::get(0).res, \" all =\", xs::len().res);\n"
    "        Arrays::forget(v);\n"
    "        CIO::println(\"after forget =\", Arrays::count().res);\n"
    "    }\n"
    "}\n";

static const char *DEMO13 =
    "program main;\n"
    "Stream Calc {\n"
    "    int div(a int, b int);\n"
    "}\n"
    "Calc MyCalc {\n"
    "    int div(a int, b int) {\n"
    "        if (b == 0) { cause \"division by zero\"; }\n"
    "        res a / b;\n"
    "    }\n"
    "    int forwardDiv(a int, b int) {\n"
    "        ALL r = MyCalc::div(a, b);\n"
    "        res r;            // res r; unwrap: r ok → take its res\n"
    "    }\n"
    "    string forwardBad(a int, b int) {\n"
    "        ALL r = MyCalc::div(a, b);\n"
    "        cause r;          // cause r; unwrap: r refused → forward cause\n"
    "        res \"won't reach here\";\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // IOStream: aggregates CIO/FIO/SIO\n"
    "        IO::println(\"IO aggregate output\");\n"
    "        ALL s = IO::format(\"%d + %d = %d\", 2, 3, 5);\n"
    "        IO::println(\"IO::format →\", s.res);\n"
    "        ALL up = IO::upper(\"hello\");\n"
    "        IO::println(\"IO::upper →\", up.res);\n"
    "        // Comstream\n"
    "        ALL neg = 0 - 5;\n"
    "        IO::println(\"Com::abs(0-5) =\", Com::abs(neg).res, \" Com::sqrt(9) =\", Com::sqrt(9).res);\n"
    "        ALL p = Com::pow(2, 10);\n"
    "        IO::println(\"Com::pow(2,10) =\", p.res);\n"
    "        // reference var decl (realme &perm follow type)\n"
    "        count &w u int = 5;      // program-level(u) writable ref count, init 5 to u\n"
    "        ALL rc = &r u count;     // expr smart ref: read u-layer count\n"
    "        IO::println(\"after count decl Ref::read =\", Ref::read(rc).res);\n"
    "        Ref::write(count, 8);\n"
    "        IO::println(\"after Ref::write(count,8) =\", Ref::read(rc).res);\n"
    "        dry &r a int;            // no-init decl: same-name target, read → refuse\n"
    "        IO::println(\"uninit decl read →\", Ref::read(dry).cause);\n"
    "        // movable perm m + Ref::move\n"
    "        ALL x = 42;\n"
    "        ALL mv = &m f x;         // method-level movable ref\n"
    "        IO::println(\"before move x =\", Ref::read(mv).res);\n"
    "        ALL taken = Ref::move(mv);\n"
    "        IO::println(\"Ref::move took =\", taken.res);\n"
    "        IO::println(\"after move read x →\", Ref::read(mv).cause);\n"
    "        // Timestream: fork timer + first-timer guard\n"
    "        Time::start();           // thread default first timer\n"
    "        ALL t2 = Time::fork();   // forked 2nd timer, resettable\n"
    "        IO::println(\"Time::reset(first timer) →\", Time::reset().cause);\n"
    "        Time::reset(t2.res);     // ok\n"
    "        IO::println(\"Time::reset(forked timer) ok\");\n"
    "        // res r; / cause r; unwrap\n"
    "        ALL good = MyCalc::forwardDiv(10, 2);\n"
    "        IO::println(\"forwardDiv(10,2) unwrap =\", good.res);\n"
    "        ALL bad = MyCalc::forwardBad(1, 0);\n"
    "        IO::println(\"forwardBad(1,0) cause =\", bad.cause);\n"
    "    }\n"
    "}\n";

/* 读取整个文件到 aalloc 缓冲区；失败返回 NULL */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = aalloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { /* 忽略 */ }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static void print_usage(void) {
    printf("🧬 BioLang — interpret or compile .bio/.bl programs\n\n");
    printf("Usage:\n");
    printf("  bio <file>               run (interpret) a program\n");
    printf("  bio -r <file>            run (interpret) a program\n");
    printf("  bio -b <file> [-o out]   compile to a standalone native executable\n");
    printf("  bio --tokens <file>      dump tokens (debug)\n");
    printf("  bio init <name>          create a project skeleton (src/ utils/ package.toml)\n");
    printf("  bio build [dir] [-o out] build a project (bundle needs, compile)\n");
    printf("  bio run [dir]            run a project (bundle needs, interpret)\n");
    printf("  bio install [dir]        install deps from package.toml\n");
    printf("  bio destroy [dir]        remove build artifacts\n");
    printf("  bio                       run built-in demos\n");
    printf("  bio -h | --help           show this help\n");
    printf("\n  env: BIOLANG_CONFIG → global config file (TOML, may contain repo=)\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage();
        return 0;
    }
    /* ── 项目命令 ── */
    if (argc > 1 && strcmp(argv[1], "init") == 0 && argc > 2)
        return project_init(argv[2]);
    if (argc > 1 && strcmp(argv[1], "build") == 0) {
        const char *dir = ".";
        const char *out = NULL;
        if (argc > 2 && strcmp(argv[2], "-o") != 0) dir = argv[2];
        if (argc > 3 && strcmp(argv[argc-2], "-o") == 0) out = argv[argc-1];
        else if (argc > 2 && strcmp(argv[2], "-o") == 0 && argc > 3) { dir = "."; out = argv[3]; }
        return project_build(dir, out);
    }
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        const char *dir = argc > 2 ? argv[2] : ".";
        return project_run(dir);
    }
    if (argc > 1 && strcmp(argv[1], "install") == 0) {
        const char *dir = argc > 2 ? argv[2] : ".";
        return project_install(dir);
    }
    if (argc > 1 && strcmp(argv[1], "destroy") == 0) {
        const char *dir = argc > 2 ? argv[2] : ".";
        return project_destroy(dir);
    }
    if (argc > 1 && strcmp(argv[1], "--tokens") == 0 && argc > 2) {
        char *buf = read_whole_file(argv[2]);
        if (!buf) { fprintf(stderr, "cannot open file: %s\n", argv[2]); return 1; }
        int n; Tok *toks = tokenize(buf, &n);
        for (int i = 0; i < n; i++)
            printf("%s ", toks[i].kind == T_NUM ? "(num)" : toks[i].text);
        printf("\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "-b") == 0 && argc > 2) {
        /* -b：编译为自包含原生可执行文件 */
        const char *file = argv[2];
        const char *out = NULL;
        if (argc > 3 && strcmp(argv[3], "-o") == 0 && argc > 4) out = argv[4];
        char *buf = read_whole_file(file);
        if (!buf) { fprintf(stderr, "cannot open file: %s\n", file); return 1; }
        char outname[1024];
        if (!out) {
            snprintf(outname, sizeof outname, "%s", file);
            char *dot = strrchr(outname, '.');
            if (dot && dot != outname) *dot = 0;   /* 去扩展名 */
            out = outname;
        }
        printf("compiling %s → %s\n", file, out);
        if (compile_program(buf, out) != 0) return 1;
        printf("done: %s (standalone executable)\n", out);
        return 0;
    }
    /* -r 显式运行，或默认直接运行 */
    int fileidx = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) fileidx = 2;
    if (argc > fileidx) {
        char *buf = read_whole_file(argv[fileidx]);
        if (!buf) { fprintf(stderr, "cannot open file: %s\n", argv[fileidx]); return 1; }
        run_source(buf);
        return 0;
    }
    printf("🧬 BioLang interpreter\n\n");
    printf("=== Demo 1: Hello World ===\n");
    run_source(DEMO1);
    printf("\n=== Demo 2: stream fork + res/ref request model ===\n");
    run_source(DEMO2);
    printf("\n=== Demo 3: unmet need assumptions → refuse to run ===\n");
    run_source(DEMO3);
    printf("\n=== Demo 4: Class declaration + sensor stream ===\n");
    run_source(DEMO4);
    printf("\n=== Demo 5: if / for / while control flow ===\n");
    run_source(DEMO5);
    printf("\n=== Demo 6: CIO/FIO/SIO substreams + bare call + default ref ===\n");
    run_source(DEMO6);
    printf("\n=== Demo 7: Array stream + Threads ===\n");
    run_source(DEMO7);
    printf("\n=== Demo 8: Taskm scheduler (round-robin + interval) ===\n");
    run_source(DEMO8);
    printf("\n=== Demo 9: smart refs &perm follow name + thread scopes ===\n");
    run_source(DEMO9);
    printf("\n=== Demo 10: multi-return types + res multi-values ===\n");
    run_source(DEMO10);
    printf("\n=== Demo 11: new syntax, __init__, this, object methods ===\n");
    run_source(DEMO11);
    printf("\n=== Demo 12: bare calls, this::attrs, Arrays/Vector ===\n");
    run_source(DEMO12);
    printf("\n=== Demo 13: IO aggregation / Com / realme decl / Ref::move / Time fork ===\n");
    run_source(DEMO13);
    return 0;
}

