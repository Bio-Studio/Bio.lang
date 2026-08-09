#include "bio.h"
#include "platform.h"
#include <stdlib.h>

/* Entry point + demos */
/* ═══════════════ Demos ═══════════════ */
const char *DEMO1 =
    "program main;\n"
    "Main { void exec() { CIO::println(\"Hello World!\"); } }\n";

const char *DEMO2 =
    "program main;\n"
    "Stream Calc { int add(a int, b int); int div(a int, b int); }\n"
    "Calc MyCalc {\n"
    "    void add(a int, b int) { res a + b; }\n"
    "    void div(a int, b int) { ref \"refused: division by zero\"; }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = MyCalc::add(3, 4);\n"
    "        CIO::println(\"3 + 4 =\", get r);\n"
    "        ALL bad = MyCalc::div(1, 0);\n"
    "        CIO::println(\"div cause:\", cause bad);\n"
    "        ALL missing = MyCalc::sqrt(9);\n"
    "        CIO::println(\"missing method cause:\", cause missing);\n"
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
    "Main { void exec() { ALL v = T1::read(); CIO::println(\"sensor reading:\", get v); } }\n";

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
    "    void div(a int, b int) { ref \"refused: division by zero\"; }\n"
    "    void noReturn(a int) { CIO::println(\"(this method returns nothing)\"); }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL r = add(3, 4);            // bare call: global find add\n"
    "        CIO::println(\"bare add(3,4) =\", get r);\n"
    "        ALL bad = div(1, 0);          // bare call + ref refusal\n"
    "        CIO::println(\"div cause:\", cause bad);\n"
    "        ALL nr = MyCalc::noReturn(1); // no explicit return → default ref(none)\n"
    "        CIO::println(\"default return:\", nr);\n"
    "        if (nr) { CIO::println(\"if is true\"); }\n"
    "        else { CIO::println(\"if is false (default ref none)\"); }\n"
    "        // substreams: CIO / FIO / SIO\n"
    "        CIO::println(\"CIO console output\");\n"
    "        ALL s = SIO::format(\"%d + %d = %d\", 2, 3, 5);\n"
    "        CIO::println(\"SIO::format →\", get s);\n"
    "        ALL up = SIO::upper(\"hello\");\n"
    "        CIO::println(\"SIO::upper →\", get up);\n"
    "        FIO::writeFile(\"/tmp/bio_test.txt\", \"Hello BioLang!\");\n"
    "        ALL content = FIO::readFile(\"/tmp/bio_test.txt\");\n"
    "        CIO::println(\"FIO read back:\", get content);\n"
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
    "        CIO::println(\"after push:\", a, \" len:\", get a::len());\n"
    "        CIO::println(\"join(-):\", get a::join(\"-\"));\n"
    "        // Threads system\n"
    "        ALL t1 = get Threads::spawn(\"factorial\", 10);\n"
    "        ALL t2 = get Threads::spawn(\"countUp\", 5);\n"
    "        CIO::println(\"live threads:\", get Threads::active());\n"
    "        ALL r1 = Threads::join(t1);\n"
    "        CIO::println(\"thread\", t1, \" 10! =\", get r1);\n"
    "        ALL r2 = Threads::join(t2);\n"
    "        CIO::println(\"thread\", t2, \" countUp =\", get r2);\n"
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
    "        ALL t1 = get Taskm::add(\"jobA\", 5);\n"
    "        ALL t2 = get Taskm::add(\"jobB\", 4);\n"
    "        CIO::println(\"tasks:\", get Taskm::active());\n"
    "        Taskm::run();\n"
    "        CIO::println(\"tasks done:\", get Taskm::active());\n"
    "        CIO::println(\"jobA sum =\", get Threads::join(t1));\n"
    "        CIO::println(\"jobB 2^4 =\", get Threads::join(t2));\n"
    "    }\n"
    "}\n";

static const char *DEMO9 =
    "program main;\n"
    "Calc Worker {\n"
    "    void threadJob(n int) {\n"
    "        thread int local_note = 0;\n"
    "        &w a int wa = &local_note;\n"
    "        Ref::write(wa, n * 2);\n"
    "        &r a int ra = &local_note;\n"
    "        CIO::println(\"thread\", get Threads::self(), \" a layer =\", get Ref::read(ra));\n"
    "        res get Ref::read(ra);\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // smart ref: &perm follow type name = &expr (perm r/w/rw/m × follow u/f/a)\n"
    "        int counter = 0;\n"
    "        &w u int wr = &counter;\n"
    "        Ref::write(wr, 10);\n"
    "        &r u int rr = &counter;\n"
    "        CIO::println(\"u read counter =\", get Ref::read(rr));\n"
    "        CIO::println(\"read-only write →\", cause Ref::write(rr, 99));\n"
    "        int x = 42;\n"
    "        &r f int rf = &x;\n"
    "        CIO::println(\"f read x =\", get Ref::read(rf));\n"
    "        // thread scope isolation (a follow)\n"
    "        ALL t1 = get Threads::spawn(\"threadJob\", 21);\n"
    "        ALL t2 = get Threads::spawn(\"threadJob\", 22);\n"
    "        CIO::println(\"t1 =\", get Threads::join(t1), \" t2 =\", get Threads::join(t2));\n"
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
    "        CIO::println(\"add(3,4) =\", get r);\n"
    "        ALL g = M::greet(\"TAK\");\n"
    "        CIO::println(\"greet =\", get g);\n"
    "        ALL t = get M::triple(10);\n"
    "        CIO::println(\"triple(10) =\", t, \" 2nd:\", get t::get(1));\n"
    "        ALL h = Hero::getHp();\n"
    "        CIO::println(\"Hero::getHp() =\", get h);\n"
    "        ALL ts = Hero::titles();\n"
    "        CIO::println(\"Hero::titles() =\", get ts);\n"
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
    "    string getName() { res get Obj::get(this, \"name\"); }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        ALL h = new Hero(\"TAK\", 88);   // new: fork class stream + auto __init__\n"
    "        CIO::println(\"object:\", h);\n"
    "        CIO::println(\"name =\", h.name, \" hp =\", h.hp);   // direct attribute access (no unwrap)\n"
    "        ALL hp = Obj::call(h, \"getHp\");\n"
    "        CIO::println(\"Obj::call getHp() =\", get hp);\n"
    "        ALL nm = h::getName();\n"
    "        CIO::println(\"h::getName() =\", get nm);\n"
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
    "        CIO::println(\"count =\", get C::get(), \" doubleGet =\", get C::doubleGet());\n"
    "        // Arrays: new Array/Vector auto-register\n"
    "        ALL v = Arrays::vector();\n"
    "        v::push(10); v::push(20); v::push(30);\n"
    "        CIO::println(\"vector =\", v, \" len =\", get v::len());\n"
    "        ALL a = new Array(3);\n"
    "        CIO::println(\"new Array(3) =\", a);\n"
    "        CIO::println(\"Arrays count =\", get Arrays::count());\n"
    "        ALL xs = get Arrays::all();\n"
    "        CIO::println(\"Arrays::get(0) =\", get Arrays::get(0), \" all =\", get xs::len());\n"
    "        Arrays::forget(v);\n"
    "        CIO::println(\"after forget =\", get Arrays::count());\n"
    "    }\n"
    "}\n";

static const char *DEMO13 =
    "program main;\n"
    "Stream Calc {\n"
    "    int div(a int, b int);\n"
    "}\n"
    "Calc MyCalc {\n"
    "    int div(a int, b int) {\n"
    "        if (b == 0) { ref \"division by zero\"; }\n"
    "        res a / b;\n"
    "    }\n"
    "    int forwardDiv(a int, b int) {\n"
    "        ALL r = MyCalc::div(a, b);\n"
    "        res r;            // res r; respond with r's value\n"
    "    }\n"
    "    string forwardBad(a int, b int) {\n"
    "        ALL r = MyCalc::div(a, b);\n"
    "        ref r;            // ref r; r refused → forward its reason\n"
    "        res \"won't reach here\";\n"
    "    }\n"
    "}\n"
    "Main {\n"
    "    void exec() {\n"
    "        // IOStream is abstract; CIO/FIO/SIO implement\n"
    "        CIO::println(\"IO aggregate output\");\n"
    "        ALL s = SIO::format(\"%d + %d = %d\", 2, 3, 5);\n"
    "        CIO::println(\"SIO::format →\", get s);\n"
    "        ALL up = SIO::upper(\"hello\");\n"
    "        CIO::println(\"SIO::upper →\", get up);\n"
    "        // Comstream\n"
    "        ALL neg = 0 - 5;\n"
    "        CIO::println(\"Com::abs(0-5) =\", get Com::abs(neg), \" Com::sqrt(9) =\", get Com::sqrt(9));\n"
    "        ALL p = Com::pow(2, 10);\n"
    "        CIO::println(\"Com::pow(2,10) =\", get p);\n"
    "        // reference var decl (a reference is a type: &perm follow type name = &expr)\n"
    "        int count = 0;\n"
    "        &w u int rc = &count;\n"
    "        Ref::write(rc, 5);\n"
    "        CIO::println(\"after count decl Ref::read =\", get Ref::read(rc));\n"
    "        Ref::write(rc, 8);\n"
    "        CIO::println(\"after Ref::write(count,8) =\", get Ref::read(rc));\n"
    "        &r u int dry = &count;   // read-only ref\n"
    "        CIO::println(\"read-only write →\", cause Ref::write(dry, 1));\n"
    "        // movable perm m: moving pointer (like C's a++)\n"
    "        ALL arr = new int[3];\n"
    "        arr[0] = 10; arr[1] = 20; arr[2] = 30;\n"
    "        &m u int mv = &arr[1];\n"
    "        CIO::println(\"before move at [1] =\", get Ref::read(mv));\n"
    "        Ref::move(mv);\n"
    "        CIO::println(\"after Ref::move at [2] =\", get Ref::read(mv));\n"
    "        // Timestream: fork timer + first-timer guard\n"
    "        Time::start();           // thread default first timer\n"
    "        ALL t2 = Time::fork();   // forked 2nd timer, resettable\n"
    "        CIO::println(\"Time::reset(first timer) →\", cause Time::reset());\n"
    "        Time::reset(get t2);     // ok\n"
    "        CIO::println(\"Time::reset(forked timer) ok\");\n"
    "        // res r; / ref r; forwarding\n"
    "        ALL good = MyCalc::forwardDiv(10, 2);\n"
    "        CIO::println(\"forwardDiv(10,2) unwrap =\", get good);\n"
    "        ALL bad = MyCalc::forwardBad(1, 0);\n"
    "        CIO::println(\"forwardBad(1,0) cause =\", cause bad);\n"
    "    }\n"
    "}\n";

/* Read a whole file into an aalloc buffer; NULL on failure */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = aalloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { /* ignore */ }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static void print_usage(void) {
    printf("🧬 BioLang — interpret or compile .bio/.bl programs\n\n");
    printf("Usage:\n");
    printf("  bio <file>                    run (interpret) a script\n");
    printf("  bio shell run <file>          run a script (interpret)\n");
    printf("  bio shell build <file> [-o out]  compile a script → executable\n");
    printf("  bio -e <bytes[K|M|G]>    set interpreter memory limit (0 = unlimited, default 256M)\n");
    printf("  bio --tokens <file>      dump tokens (debug)\n");
    printf("  bio init <name>          create a project skeleton (src/ utils/ package.toml)\n");
    printf("  bio build [dir] [-o out]  build a project (bundle needs, compile)\n");
    printf("  bio build [dir] -s        build → standalone executable (default)\n");
    printf("  bio build [dir] -m [out.img|out.zip]  build → .img/.zip package (app + platform CLI + libs)\n");
    printf("  bio run [dir]            run a project (bundle needs, interpret)\n");
    printf("  bio install [dir]        install deps from package.toml\n");
    printf("  bio destroy [dir]        remove build artifacts\n");
    printf("  bio pack <out.img|.zip> [--entry NAME] <files...>\n");
    printf("                           package compiled products (raw .img / .zip)\n");
    printf("  bio unpack <pkg> [dir]   unpack a .img / .zip package\n");
    printf("  bio                       run built-in demos\n");
    printf("  bio -h | --help           show this help\n");
    printf("\n  env: BIOLANG_CONFIG → global config file (TOML, may contain repo=)\n");
    printf("       BIO_MEM_LIMIT → memory limit override (same syntax as -m)\n");
}

static size_t parse_size(const char *s) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end) {
        if (*end == 'K' || *end == 'k') v *= 1024ull;
        else if (*end == 'M' || *end == 'm') v *= 1024ull * 1024;
        else if (*end == 'G' || *end == 'g') v *= 1024ull * 1024 * 1024;
    }
    return (size_t)v;
}

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

static unsigned long long fnv1a(const char *s) {
    unsigned long long h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

typedef struct { char out[BIO_PATH_MAX]; int found; } PickCtx;

static void pick_cb(const char *path, int is_dir, void *ud) {
    PickCtx *pc = ud;
    if (!pc->found && !is_dir) {
        snprintf(pc->out, sizeof pc->out, "%s", path);
        pc->found = 1;
    }
}

/* Run a packaged product: .img extracts its entry directly (no compression),
 * .zip is unpacked first and its first file is executed. */
static int run_package(const char *pkg, int argc, char **argv, int start) {
    char tmp[BIO_PATH_MAX];
    const char *base = strrchr(pkg, '/');
    base = base ? base + 1 : pkg;
    if (ends_with(pkg, ".img")) {
        bio_mkdir_p("bin/.cache/img");
        snprintf(tmp, sizeof tmp, "bin/.cache/img/%s.entry", base);
        if (img_entry(pkg, tmp, sizeof tmp) != 0) {
            fprintf(stderr, "cannot read image %s\n", pkg);
            return 1;
        }
    } else {
        bio_mkdir_p("bin/.cache/zip");
        snprintf(tmp, sizeof tmp, "bin/.cache/zip/%s", base);
        if (zip_unpack(pkg, tmp) != 0) {
            fprintf(stderr, "cannot unpack %s\n", pkg);
            return 1;
        }
        char apppath[BIO_PATH_MAX + 32];
        snprintf(apppath, sizeof apppath, "%s/app", tmp);
        FILE *probe = fopen(apppath, "rb");
        if (probe) {
            fclose(probe);
            snprintf(tmp, sizeof tmp, "%s", apppath);
        } else {
            PickCtx pc = {{0}, 0};
            bio_walk_dir(tmp, pick_cb, &pc);
            if (!pc.found) {
                fprintf(stderr, "zip package has no files\n");
                return 1;
            }
            snprintf(tmp, sizeof tmp, "%s", pc.out);
        }
    }
    const char **av = aalloc(sizeof(char *) * (size_t)(argc - start + 2));
    int n = 0;
    av[n++] = tmp;
    for (int i = start; i < argc; i++) av[n++] = argv[i];
    av[n] = NULL;
    return bio_run(av) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *env_lim = getenv("BIO_MEM_LIMIT");
    if (env_lim && *env_lim) bio_set_mem_limit(parse_size(env_lim));
    /* `-e` sets the interpreter memory limit; `-m` belongs to `bio build`
     * (package mode). */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            bio_set_mem_limit(parse_size(argv[i + 1]));
            for (int j = i; j + 2 < argc; j++) argv[j] = argv[j + 2];
            argc -= 2;
            i--;
        }
    }
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage();
        return 0;
    }
    /* ── Project commands ── */
    if (argc > 1 && strcmp(argv[1], "init") == 0 && argc > 2)
        return project_init(argv[2]);
    if (argc > 1 && strcmp(argv[1], "build") == 0) {
        const char *dir = ".";
        const char *out = NULL;
        int pkg = 0, single = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out = argv[++i]; }
            else if (strcmp(argv[i], "-m") == 0) {
                /* Package mode: bundle app + platform CLI + libs into .img/.zip. */
                pkg = 1;
                if (i + 1 < argc && argv[i + 1][0] != '-') out = argv[++i];
            }
            else if (strcmp(argv[i], "-s") == 0) single = 1;   /* standalone executable */
            else if (argv[i][0] != '-' && strcmp(dir, ".") == 0) dir = argv[i];
            else {
                fprintf(stderr, "usage: bio build [dir] [-s | -m [out.img|out.zip]] [-o out]\n");
                return 1;
            }
        }
        if (pkg && single) {
            fprintf(stderr, "usage: -s (standalone) and -m (package) are mutually exclusive\n");
            return 1;
        }
        if (pkg && !out) {
            /* Default package name: bin/<project>.img */
            char def[BIO_PATH_MAX];
            snprintf(def, sizeof def, "bin/%s.img", project_name(dir));
            char *o = aalloc(strlen(def) + 1);
            strcpy(o, def);
            out = o;
        }
        return project_build(dir, out);
    }
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        if (argc > 2 && ends_with(argv[2], ".img")) return run_package(argv[2], argc, argv, 3);
        if (argc > 2 && ends_with(argv[2], ".zip")) return run_package(argv[2], argc, argv, 3);
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
    if (argc > 1 && strcmp(argv[1], "pack") == 0 && argc > 3) {
        const char *out = argv[2];
        const char *entry = NULL;
        const char **files = aalloc(sizeof(char *) * (size_t)argc);
        int n = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) { entry = argv[++i]; continue; }
            files[n++] = argv[i];
        }
        int rc = ends_with(out, ".img")
            ? img_create(out, entry, files, n)
            : zip_create(out, files, n);
        if (rc != 0) { fprintf(stderr, "pack failed: %s\n", out); return 1; }
        printf("packed %d file(s) → %s\n", n, out);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "unpack") == 0 && argc > 2) {
        const char *dir = argc > 3 ? argv[3] : ".";
        int rc = ends_with(argv[2], ".img")
            ? img_unpack(argv[2], dir)
            : zip_unpack(argv[2], dir);
        if (rc != 0) { fprintf(stderr, "unpack failed: %s\n", argv[2]); return 1; }
        printf("unpacked %s → %s\n", argv[2], dir);
        return 0;
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
    if (argc > 1 && strcmp(argv[1], "shell") == 0) {
        if (argc > 2 && strcmp(argv[2], "run") == 0 && argc > 3) {
            char *buf = read_whole_file(argv[3]);
            if (!buf) { fprintf(stderr, "cannot open file: %s\n", argv[3]); return 1; }
            run_source(buf);
            return 0;
        }
        if (argc > 2 && strcmp(argv[2], "build") == 0 && argc > 3) {
            /* shell build: compile a script into a standalone native executable */
            const char *file = argv[3];
            const char *out = NULL;
            if (argc > 4 && strcmp(argv[4], "-o") == 0 && argc > 5) out = argv[5];
            char *buf = read_whole_file(file);
            if (!buf) { fprintf(stderr, "cannot open file: %s\n", file); return 1; }
            /* Incremental cache: identical source content reuses the cached binary. */
            char cache[BIO_PATH_MAX];
            bio_mkdir_p("bin/.cache/compile");
            snprintf(cache, sizeof cache, "bin/.cache/compile/%016llx",
                     (unsigned long long)fnv1a(buf));
            char outname[BIO_PATH_MAX];
            if (!out) {
                const char *base = strrchr(file, '/');
                base = base ? base + 1 : file;
                snprintf(outname, sizeof outname, "bin/%s", base);
                char *dot = strrchr(outname, '.');
                if (dot && dot != outname &&
                    (strcmp(dot, ".bio") == 0 || strcmp(dot, ".bl") == 0))
                    *dot = 0;                       /* strip the source extension */
                bio_mkdir_p("bin");
                out = outname;
            }
            if (bio_copy_file(cache, out) == 0) {
                printf("cached %s → %s\n", file, out);
                return 0;
            }
            printf("compiling %s → %s\n", file, out);
            if (compile_program(buf, cache) != 0) return 1;
            if (bio_copy_file(cache, out) != 0) {
                fprintf(stderr, "cannot copy compiled product to %s\n", out);
                return 1;
            }
            printf("done: %s (standalone executable)\n", out);
            return 0;
        }
        fprintf(stderr, "usage: bio shell run <file> | bio shell build <file> [-o out]\n");
        return 1;
    }
    /* plain `bio <file>` runs a script */
    int fileidx = 1;
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
    printf("\n=== Demo 9: typed smart refs &perm follow base + moving pointer + thread scopes ===\n");
    run_source(DEMO9);
    printf("\n=== Demo 10: multi-return types + res multi-values ===\n");
    run_source(DEMO10);
    printf("\n=== Demo 11: new syntax, __init__, this, object methods ===\n");
    run_source(DEMO11);
    printf("\n=== Demo 12: bare calls, this::attrs, Arrays/Vector ===\n");
    run_source(DEMO12);
    printf("\n=== Demo 13: abstract IO / Com / typed refs / Ref::move pointer / Time fork ===\n");
    run_source(DEMO13);
    return 0;
}
