# BioLang examples

Runnable, heavily-commented programs that walk through BioLang feature by feature.
Each file is self-contained — run any of them with:

```bash
bio examples/01-hello.bio          # interpret
bio shell build examples/01-hello.bio -o hello   # compile to a standalone executable
```

## Reading order

| # | File | Teaches |
|---|------|---------|
| 01 | [01-hello.bio](01-hello.bio) | The basic skeleton: `program main`, `Main { void exec() }`, `CIO::println` |
| 02 | [02-requests.bio](02-requests.bio) | The request model: `res` / `ref` / `get` / `cause`, `ALL`, prefix unwrapping |
| 03 | [03-control-flow.bio](03-control-flow.bio) | `if` / `else if` / `else`, `while`, `for`, `break` / `continue` |
| 04 | [04-streams-fork.bio](04-streams-fork.bio) | Signature streams, forked implementations, fields, bare calls, streams as arguments |
| 05 | [05-io-substreams.bio](05-io-substreams.bio) | CIO / FIO / SIO / IO: text streams vs byte streams |
| 06 | [06-classes-objects.bio](06-classes-objects.bio) | `Class`, `new`, `__init__`, `this`, object methods, `Obj::set/get` |
| 07 | [07-arrays.bio](07-arrays.bio) | Array/Vector (Bio classes), Solid streams, the `Arrays` collection, array literals |
| 08 | [08-multi-return.bio](08-multi-return.bio) | Multiple return types, `res a, b, c` → array |
| 09 | [09-threads.bio](09-threads.bio) | Cooperative threads: `spawn` / `yield` / `join` / `active` / `self` |
| 10 | [10-taskm.bio](10-taskm.bio) | Task manager: round-robin scheduling of tasks |
| 11 | [11-smart-refs.bio](11-smart-refs.bio) | Typed smart references `&perm follow base`, `get p` / `p = v` / `p++` moving pointer, const/thread variables |
| 12 | [12-computation.bio](12-computation.bio) | `Com` computation stream + `Time` timers |
| 13 | [13-need.bio](13-need.bio) | Assumptions: `need value/function/stream/Class` |
| 14 | [14-binary-lib.bio](14-binary-lib.bio) | Calling native C libraries (`Stream m & "libm.so.6"`) |
| 15 | [15-annotations.bio](15-annotations.bio) | Annotations: `@unfork`, `@onlyread`, `@read`/`@write` |
| [16-phonebooth.bio](16-phonebooth.bio) | Phone-booth methods: `@call` (per-thread booth), `@ucall` (global booth), recursion refused |
| proj | [project/](project/) | A complete project: `package.toml` + `src/` + `utils/` |

## Suggested learning path

1. Start with **01** and **02** — they cover the two ideas everything else builds on:
   the program skeleton and the request/response model.
2. **03–05** give you control flow, streams, and IO — enough to write real programs.
3. **06–08** introduce classes, objects, and arrays.
4. **09–12** are the more advanced builtin systems: threads, scheduling,
   references, math, and time.
5. **13** shows how a program declares what it depends on, and **14** shows how
   to call into native C code.
6. Finish with the **[project/](project/)** example to see how a multi-file
   project is organized and built:

```bash
cd examples/project
bio run        # interpret the bundled project
bio build      # compile it into a standalone ./app executable
```

## Conventions

- Every file starts with a comment block explaining what it teaches.
- Inline comments explain each construct.
- The trailing comment block shows the **expected output**.

> The exact "refused" messages shown by the interpreter may vary slightly
> between versions; the structural output (numbers, strings, control flow) is
> what matters.
