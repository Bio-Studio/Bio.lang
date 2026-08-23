# BiuBiuBiu — A Stream-Oriented Language (Rust: Interpreter + LLVM Compiler)

<p align="center">
  <img src="assets/badges.svg" alt="BiuBiuBiu: v0.4.0 · MIT · Rust · LLVM · 7-language docs" width="640">
</p>

BiuBiuBiu (formerly BioLang) is an **open-source programming language written in Rust**, running as both an **interpreter** (`bbb run`) and an **LLVM compiler** (`bbb llvm`). Its design is **stream-oriented**: every operation is a *request* that can be *responded* (`res`) or *refused* (`cause`). Write a `.bio` file and either interpret it with `bbb`, or compile it into a **standalone native executable**. MIT licensed.

```bio
program main;
Main {
    void exec() {
        CIO::println("Hello, BioLang!");
    }
}
```

## Quick highlights

- **Streams everywhere** — streams are first-class values that can be forked, passed as arguments, and composed.
- **Request/response model** — `res` responds, `ref` refuses; unwrap the actual value with `get X` and the refusal reason with `cause X`.
- **Objects & classes** — `Class` declarations, `new`, automatic `__init__`, `this`, object methods.
- **Interpret or compile** — `bio` interprets; `bio shell build` emits a self-contained native executable (no `bio` needed at runtime).
- **Concurrency** — cooperative threads (`Threads`) and a round-robin task manager (`Taskm`).
- **Binary interop** — link native `.so` libraries and call their exported functions as stream methods (`Stream m & "libm.so"` → `m::sin(0)`).
- **Smart references** — typed references `&perm follow base` (permission = any stack of `r/w/m`: r, w, m, rw, rm, wm, rwm × follow `u/f/a/t` = 28 types), generic over any base type; `p = &a[0]`, `get p`, and `p++` moves the pointer (m).

## Platform support

| Platform | Status |
|---|---|
| Linux (x86-64, aarch64) | ✅ Fully supported |
| macOS (Intel x86-64) | ⚠️ Works, but the ucontext-based thread backend triggers deprecation warnings |
| macOS (Apple Silicon) | ⚠️ Prebuilt release binaries work; building from source hits Apple's known-buggy ucontext |
| Windows (x86-32/64, arm64) | ⚠️ Experimental — Fiber-based thread backend + LoadLibrary shim implemented; `make bin` cross-builds working packages (community testing welcome) |

The codebase is clean C99 (builds warning-free with both gcc and clang); the
portability gaps are all in OS APIs, not the language core.

## Building with profiles

Pick a preset compile-time profile instead of passing flags by hand — handy for
building on different environments, and easy for the community to extend:

```bash
make profiles                    # list the available profiles
make                             # default: auto-detect platform + compiler
make PROFILE=linux-gcc           # Linux, GCC, -O2
make PROFILE=linux-clang         # Linux, clang, -O2
make PROFILE=macos               # macOS (clang, silences ucontext warnings)
make PROFILE=debug               # -O0 -g
make PROFILE=release             # -O3
make PROFILE=sanitize            # ASan + UBSan (dev)
make PROFILE=windows             # experimental MinGW-w64 (see Platform support)
make PROFILE=debug info          # show the resolved compiler/flags for a profile
```

A profile is a single file in [profiles/](profiles/) that sets `PROFILE_CC`,
`PROFILE_OPT`, `PROFILE_CFLAGS`, `PROFILE_LDFLAGS`, `PROFILE_AR`,
`PROFILE_PREFIX` and a one-line `PROFILE_DESC`. To add one, copy an existing
profile and tweak it — no other changes needed.

- `bio shell build` reuses the profile's compiler and link flags, so a compiled
  executable matches how `bio` itself was built.
- **Switching profiles requires `make clean`** — make tracks timestamps, not
  variable values.
- Command-line overrides still win: `make PROFILE=linux-gcc CC=clang` uses
  clang for `bio`, but `bio shell build` still uses the profile's compiler (documented
  behavior).

## Build & run

```bash
make              # build → ./bio (also builds libbio.a for the compiler)
make install      # install to ~/.local/bin/bio

bio               # run the 13 built-in demos
bio program.bio   # run a BioLang source file (interpret)
bio shell run program.bio # explicit run (interpret)
bio shell build program.bio [-o output]  # compile → self-contained native executable
bio --tokens x.bl # dump lexer tokens (debug)
bio -h            # help
```

### Project-based build & run

A project has `package.toml` (manifest) + `src/` (source, `main.bio` is the entry) + `utils/` (libraries) + `.biolang/deps/` (dependencies).

```bash
bio init <name>           # create a project skeleton
bio build [dir] -s        # build → standalone executable (default)
bio build [dir] -m        # build → .img package (app + platform CLI + libs)
bio build [dir] -m out.zip  # build → .zip package (format by extension)
bio build [dir] [-o out]  # build, explicit output path
bio run [dir]             # run a project (interpret)
bio install [dir]         # install package.toml dependencies
bio destroy [dir]         # remove build artifacts (.biolang/, app)
```

`-s` (standalone) produces a single self-contained executable; `-m` (package)
bundles the app together with the current platform's CLI and shared runtime
into a distributable `.img` or `.zip` package (see [Packaging](#packaging-img--zip)).

- **`need` bundling** — starting from the `main` entry, providers for every `need` are collected recursively (from `src/` + `utils/` + `.biolang/deps/`) until the closure stabilizes; **any `need` without a provider is an error**. A call by signature-stream name falls back to its implementation stream (`Calc::add` → implementation stream).
- **package.toml** — standard fields `name`/`version`/`repo` + `[dependencies]` (`name = { version=.., repo=.. }`, `repo` optional).
- **repo resolution order** — dependency's own `repo` → `repo` from the global config file pointed to by `BIOLANG_CONFIG` → system default `~/.biolang/config.toml`.
- **Dependency fetching** — supports git repos, HTTP downloads, and local paths.

Example `package.toml`:

```toml
name = "myapp"
version = "0.1.0"
[dependencies]
libfoo = { version = "1.0.0", repo = "/path/to/libfoo" }   # or a git/http URL
```

### Script compile (`bio shell build`)

`bio shell build program.bio` compiles a program into a **self-contained native executable** (source embedded + interpreter runtime linked via `libbio.a`). At runtime it needs neither `bio` nor the source. The default output path is `bin/<name>` (`example.bio` → `bin/example`); use `-o` to override.

```bash
bio shell build example.bio
./bin/example      # runs standalone, no bio required
```

### Packaging (`.img` / `.zip`)

Compiled products can be bundled into a single distributable package:

```bash
bio pack <out.img|.zip> [--entry NAME] <files...>
bio unpack <pkg> [dir]   # extract a package
bio run pkg.img          # run a package directly (executes its entry)
bio run pkg.zip
```

- **`.img`** — a custom **raw image** format (`BIOIMG1`, v2): a small directory
  header followed by the files' bytes written directly. No compression, fully
  seekable, extremely fast. **File permissions are stored**, so unpacked
  executables keep their executable bit.
- **`.zip`** — standard zip archive, written and read **natively with the
  STORE method** (no compression). No external `zip`/`unzip` tools required,
  works on every platform. When the system `unzip` is present it is used for
  unpacking (so legacy deflate archives still open).

`--entry NAME` marks which packed file is the runnable entry point
(a compiled executable from `bio shell build`); `bio run` executes it. For zip
packages without an explicit entry, the first file is executed.

```bash
bio shell build examples/01-hello.bio   # → bin/01-hello
bio pack hello.img --entry 01-hello bin/01-hello
bio run hello.img                       # runs the packaged entry
```

## Examples

The [`examples/`](examples/) directory contains runnable, heavily-commented programs that walk through the language feature by feature — a great way to learn what BioLang can do:

| Example | Teaches |
|---|---|
| [examples/01-hello.bio](examples/01-hello.bio) | Hello World, `program main`, `Main { void exec() }` |
| [examples/02-requests.bio](examples/02-requests.bio) | Request model: `res`/`ref`/`get`/`cause`, prefix unwrapping, forwarding |
| [examples/03-control-flow.bio](examples/03-control-flow.bio) | `if`/`else if`/`else`, `while`, `for`, `break`/`continue` |
| [examples/04-streams-fork.bio](examples/04-streams-fork.bio) | Signature streams, forked implementations, bare calls |
| [examples/05-io-substreams.bio](examples/05-io-substreams.bio) | CIO / FIO / SIO / IO aggregate: text & byte streams |
| [examples/06-classes-objects.bio](examples/06-classes-objects.bio) | `Class`, `new`, `__init__`, `this`, object methods, `Obj::set/get` |
| [examples/07-arrays.bio](examples/07-arrays.bio) | Array/Vector (Bio classes), Solid streams, the Arrays collection |
| [examples/08-multi-return.bio](examples/08-multi-return.bio) | Multiple return types, `res a, b, c` → array |
| [examples/09-threads.bio](examples/09-threads.bio) | Cooperative threads: spawn/yield/join/active/self |
| [examples/10-taskm.bio](examples/10-taskm.bio) | Task manager: add/interval/run/stop/active |
| [examples/11-smart-refs.bio](examples/11-smart-refs.bio) | Typed smart references (28 types), `get p` / `p = v` / `p++` moving pointer, const/thread variables |
| [examples/12-computation.bio](examples/12-computation.bio) | `Com` computation stream + `Time` timers |
| [examples/13-need.bio](examples/13-need.bio) | `need value/function/stream/Class` assumptions |
| [examples/14-binary-lib.bio](examples/14-binary-lib.bio) | Binary library streams (`Stream m & "libm.so"`) |
| [examples/project/](examples/project/) | A complete project: package.toml + src/ + utils/ (run with `bio build`/`bio run`) |

Run any example with:

```bash
bio examples/01-hello.bio
```

## Code structure (multi-module)

```
src/
├── bio.h        # public header: type definitions + module APIs
├── arena.c      # simple memory pool (aalloc/astrdup)
├── lexer.c      # lexing (tokenize)
├── parser.c     # parsing (AST construction, exposes parse_program_tokens)
├── value.c      # value / request-result model (res/ref)
├── builtin.c    # builtin streams (CIO/FIO/SIO/IO/Com/Time/Rem/Solid/Array/Ref/...)
├── bts.c        # Threads cooperative threads + Taskm task scheduler
├── interp.c     # interpreter (stream registry, eval, assumption checks, run_source)
├── compile.c    # compiler (bio shell build: embed source + link libbio.a)
└── main.c       # entry + CLI (-b/-r/--tokens) + built-in demos
```

## VSCode plugin

Located at `vscode/biolang-vscode/` (installable to `~/.vscode/extensions`).

- **Syntax highlighting** — `.bl` / `.bio` files (keywords incl. `cause`, types `int`/`float`/`double`/`string`/`char`, strings, numbers, calls, comments, operators, smart refs `&rw u int p = &x`).
- **Code snippets** — main skeleton, stream signatures (`a int` args), fork, class, res/ref/get/cause, need (incl. Class), sref smart refs, bins binary libs, spawn threads, taskm, if/while/for.
- **Completions** — `Stream::` method completion (builtin CIO/FIO/SIO/Array/Threads/Taskm/Ref/Console + streams declared in docs), and `&` smart-ref permission `r/w/rw` → follow `u/m/a` hints.
- **Run commands** — `BioLang: Run current file (bio)` (editor button + command palette) opens a **Webview interactive panel** — program output streams live, and the input box feeds stdin to `CIO`; `BioLang: Run built-in demos` runs them directly.
- Reload VS Code (or "Developer: Reload Window") after installing; the run commands need `bio` on PATH (`~/.local/bin/bio`).

## Implemented language features

| Feature | Syntax | Example |
|---|---|---|
| Main program | `program main;` + `Main { void exec() { ... } }` | `Main { void exec() { CIO::println("Hi"); } }` |
| Stream signature | `Stream S { int add(a int, b int); }` (args written `name type`) | `Stream Calc { int add(a int, b int); }` |
| Stream fork | `S Impl { void m(...) { ... } }` | `Calc MyCalc { void add(a int, b int) { res a + b; } }` |
| Class declaration | `Class C { void __init__() {...} int hp; int[] a; }` | classes are streams too; methods need no overwrite |
| **Field declaration** | class/stream fields, methods and fields in any order, comma-separated: `int x, y;` / `int[] a;` / `string s;` / `type T; T n; T[] a;` (generic style). `new` materializes defaults (numeric 0 / string "" / array []) | `Class Vector { int x, y; }` → read/write via `v.x`, `v::x`, `this::x` |
| **`new` instantiation** | `new Class(args...)` → fork class stream + auto `__init__(args...)`, returns an object (Result-wrapped); no special-casing — all classes (incl. builtin Array) share the same path | `ALL h = new Hero("TAK", 88);`, `ALL a = new Array(3);` → `h.hp`, `h::getName()` |
| **Object model** | inside object methods `this` = the object; `Obj::new/get/set/forget/call/class`; object-stream calls `obj::method()`. A **bare-name assignment** that hits a declared field writes the instance property (`x = x;` ≡ `this::x = x;`); reads prefer params/locals | `Obj::set(this, "hp", 88);`, `ALL nm = h::getName();`, `__init__(x int) { x = x; }` |
| **Streams/objects as args** | streams are first-class and can be method params with callable methods; reference variables use the typed reference form | `void show(cio CIO) { cio::println(...); }`, `v::show(CIO)`; `&rw u int p = &a[0];` |
| Request / respond / refuse | `res expr;` respond; `ref "reason";` refuse | `res a + b;` or `ref "division by zero";` |
| **Multiple return types** | methods declare `void/int/float/double/string/char` (optionally `[]`), uniform across signature/fork/Class/Main | `int add(a int, b int) { res a + b; }`, `string[] titles() { res "brave", "legend"; }` |
| **`res` multi-values** | `res a, b, c;` comma-separated → returns an array | `res 1, 2, 3;` → `[1, 2, 3]`; single `res x;` stays scalar |
| Request result | `ALL x = Stream::method(...);` | `ALL r = MyCalc::add(3, 4);` |
| Result unwrap | **`get X`** takes the actual returned value; **`cause X`** takes the refusal reason; `res r;` responds and `ref r;` forwards a refusal | `ALL n = get CIO::readInt();`, `ALL why = cause Calc::div(1,0);` |
| **Refusal propagation** | a refused Result passed as an argument refuses the enclosing request | `MyCalc::add(1, bad)` → refused |
| Control flow | `if/else if/else`, `while`, `for(;;)`, `break`, `continue` | truthiness: 0 / "" / empty refusal (`nothing`) = false; real values, objects and refusals with a real reason = true |
| **Substreams** | **IO** IOStream is an abstract parent (carries no functionality) / **CIO** console implementation (+ pre-forked Console) / **FIO** file implementation / **SIO** string implementation / **Com** computation stream | `CIO::println` `CIO::getln` `CIO::write` `CIO::read` `FIO::readFile` `SIO::format` |
| **Stream methods by kind** | **Text streams**: `println`/`print` (write text), `get`/`getln` (read text: one char / one line); **byte streams**: `write` (raw bytes), `read` (raw byte 0-255, EOF -1). CIO/SIO implement both (SIO uses an in-memory string buffer as its "file") | `CIO::getln()`, `SIO::read()`, `CIO::write("A")` |
| **Com computation stream** | branch of transient streams for instant computations: `abs/min/max/pow/sqrt/floor/ceil/round/sign/sin/cos/tan/log/exp` | `get Com::abs(0-5)`, `get Com::pow(2, 10)` |
| **Timestream** | hold several timers; the **first timer is owned by the thread and cannot be reset**, timers from `Time::fork()` can; `now/sleep/start/fork/elapsed/reset` | `Time::start()` (thread's first timer); `ALL t = Time::fork(); Time::reset(get t);` |
| Bare function calls | `add(a, b)` — search all streams for a provider | `res add(a, b);` (spec style) |
| Default return | no explicit res/ref → default `ref(nothing)`, `if` treats it as false | `ALL x = f(); if (x) {...}` → else |
| Assumptions | `need value/function/stream/Class ...;` (`need Stream`/`need Class` may carry `{...}` bodies) | unmet → refuses to run |
| **Variable modifiers** | `const int x = 10;` → Constantstream (read-only; redeclare/assign refused; works inside methods and at top level); `int x = 10;` → scope stream; `thread int x = 10;` → thread variable (thread-scoped) | `const int PI = 3;` changing PI → refused "constant is read-only" |
| Operators | `+ - * / == != < > <= >=`, numbers, strings, variables; **compound assignment** `+= -= *= /= %=` (variables, `this::props`, array elements); **`i++` / `i--`**; **array indexing** `a[i]` read/write | `ALL y = get x * 2 + 1;`, `this::n += 1;`, `for (int i = 0; i < n; i++) { a[i] = i; }` |
| Base types | `int` `float` `double` `string` `char` (field: `int n;` type name) | |
| **Array/Vector (Bio classes)** | Array/Vector are **classes implemented in Bio code** (not interpreter builtins) over the Solid stream; `new Array(n)` / `new Vector()`; **array literals** `new type[n]` are type-generic (base types and custom classes); methods `__init__/len/get/set/push/pop/clear/join` | `ALL a = new Array(3); a::set(0, 10); a::push(40); get a::len(); get a::join("-")`; `int[] a = new int[12];`, `ALL hs = new Hero[3];` |
| **Solid stream** | contiguous storage + auto-grow + **moving head pointer**; `new/len/get/set/push/pop/read/peek/head/resetHead/clear/join` | `ALL s = get Solid::new(); get Solid::read(s)` (head advances) |
| **Arrays collection** | holds every Array/Vector instance; a `new Array` registers itself (`Arrays::add(this)` in `__init__`, visible in Bio code); `count/all/get/add/forget`; dynamic array `vector()` | `get Arrays::count()`, `ALL v = Arrays::vector(); v::push(10);` |
| **Binary library streams** | `Stream name & "file.so" { ... }`; the body is a normal stream body (Bio methods/fields allowed); exported functions become stream methods automatically | `Stream m & "libm.so" { int doubleIt(x int) { res x * 2; } }` → `m::sin(1.0)`, `m::doubleIt(21)` |
| ~~Bare binary calls~~ | ~~`&func(...)`~~ — **removed/deprecated: too dangerous** (silent symbol search across every loaded library). Call through the binary library stream | `m::pow(2, 10)` |
| **Threads** | `Threads::spawn("method", args...)` / `yield` / `join` / `active` / `self` | cooperative user threads (ucontext), threads run bare method calls |
| **Taskm** | `Taskm::add/interval/run/stop/active` | auto round-robin of all tasks until done; `interval(ms)` sets the rotation interval |
| **Smart references** | reference type `&perm follow base` (permission stacks r/w/m: r, w, m, rw, rm, wm, rwm × follow u/f/a/t = 28 types), generic over int/double/float/string/char/arrays/classes; declaration `&rw u int p = &a[0];`; read `get p`, write `p = v`, move the pointer `p++` (m) | `&r u int p = &x;` read-only; `&w u int p = &a[0];` write-only; `&rwm t int p = &a[1]; p++;` read-write-move thread ref |
| **Scope chain** | method mini-stream → thread/main-thread scope (area) → program level (Unistream), each with its own memory stream | thread `a`-layer isolation; `u`-layer shared program-wide |

> Binary-function calling convention: prototypes called as `double(*)(double,...)` (at most 6 args, numeric only); string/pointer arguments are not yet supported.
>
> Threads are cooperative: `yield()` yields; joining an unfinished target from inside a thread is not yet supported.

## Built-in demos

1. Hello World
2. Stream fork + res/ref request model (responded, refused, missing method)
3. Unmet `need` assumptions → refuses to run
4. Class declaration + sensor stream readings
5. `if` / `for` / `while` control flow (sum, factorial, continue/break, else if, for(;;))
6. CIO/FIO/SIO substreams + bare calls + ref refusal + default ref(nothing)
7. Array class (`new Array` + object methods) + Threads (factorial thread + cooperative countUp)
8. Taskm scheduler (round-robin sum/2^n)
9. Smart refs `&perm follow base` (7 permission stacks × u/f/a/t, 28 typed references) + moving pointer + thread scope isolation
10. Multiple return types (int/string/int[]) + `res` multi-values + Class methods
11. `new` fork class stream + auto `__init__` + `this` + object methods (`Obj::call` / `h::getName`)
12. In-stream bare calls + `this::` props + Arrays collection/Vector
13. IO aggregate / Com computation stream / reference var decls / `Ref::move` / Time fork / res·cause unwrap

## Not yet implemented

- Declaring `Threadstream` (process streams) / `Areastream` as explicit stream types (currently `area` exists only as an implicit scope layer).
- The compiler currently produces a "source embedded + runtime linked" self-contained executable; true AST→machine-code / standalone-runtime compilation is future work.

## License

[MIT](LICENSE) © 2026 BioLang contributors
