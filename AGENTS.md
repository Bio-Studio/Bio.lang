# BioLang — Contributor Guide

A stream-oriented programming language implemented in C: an interpreter plus a
source-embedding compiler. Every program is built from streams (Unistream,
Comstream, Remstream, Objstream, Threadstream, Timestream, IOStream, ...).
Full design: `DESIGN.md`. Full docs: `README.md`.

## Commands

### Build

```bash
make                      # build ./bio (default profile: auto-detect)
make PROFILE=debug        # pick a profile (debug/release/sanitize/linux-gcc/...)
make profiles             # list available profiles
make PROFILE=x info       # show resolved toolchain for profile x
make clean                # REQUIRED when switching profiles (timestamps, not values)
make install              # install to ~/.local/bin/bio
```

### Run / verify

```bash
bio                       # run the 13 built-in demos
bio program.bio           # interpret a source file
bio -r program.bio        # explicit run (interpret)
bio -b program.bio [-o out]  # compile to self-contained native executable (needs libbio.a)
bio --tokens x.bl         # dump lexer tokens (debug)
bio -h
```

Verification workflow: `make`, then run the relevant `examples/NN-*.bio`
files — they double as regression tests. `examples/project/` tests
project build/run/`need` bundling end to end. Do not skip verification.

### Project CLI

`bio init|build|run|install|destroy` operate on projects (`package.toml` +
`src/` + `utils/`).

## Code structure

```
src/bio.h       public header: types + module APIs
src/arena.c     memory pool (aalloc/astrdup)
src/lexer.c     tokenizer
src/parser.c    AST construction (parse_program_tokens)
src/value.c     value / request-result model (res/ref)
src/builtin.c   builtin streams (CIO/FIO/SIO/IO/Com/Time/Rem/Solid/Array/Ref/...)
src/bts.c       cooperative threads + taskm scheduler
src/interp.c    interpreter (stream registry, eval, assumption checks, run_source)
src/compile.c   compiler (bio -b: embed source + link libbio.a)
src/main.c      entry + CLI + built-in demos
src/platform.c  platform shims (fs/process/clock/HOME — cross-platform)
examples/       runnable teaching examples, 01..14 + examples/project/ (full project)
profiles/*.mk   build profiles (PROFILE_CC/PROFILE_AR/PROFILE_LDFLAGS/PROFILE_OPT)
vscode/         VSCode extension source (syntax highlight, snippets, completions)
```

## Architecture essentials

- **Request/response model**: every operation is a request that may be refused.
  `res`/`cause`/`ref` keywords; `ALL result = add(a,b)` carries both.
- **Streams**: signature streams declare methods; `SStream S { ... }` forks an
  implementation. Calls are `Stream::method()`. Classes are streams
  (`Class CClass { void __init__() }`, `new CClass()`).
- **`need` assumptions**: `need Stream/Class/function/value ...` — provider
  resolution starts from the main entry, collected recursively from
  `src/` + `utils/` + `.biolang/deps/`; a `need` without a provider is an ERROR.
- **Smart refs**: `realme &r u int` (permissions r/w/rw/m × scope u/f/a).
- **Compile mode**: `bio -b` embeds the source into a self-contained binary;
  the compiler requires `libbio.a` (plain `make` builds it).
- **Modes**: interpreted and compiled — same semantics, both supported.

## Conventions

- C code: C11-ish, modular single-purpose files, snake_case, `-Wall -Wextra`
  clean. New modules: add the `.c` to `LIB` in `Makefile` and expose APIs in
  `src/bio.h` (or `src/platform.h` for platform shims).
- User-facing strings and comments are **English** (i18n pass done; keep it that way).
- Git commits: conventional style (`feat:`/`docs:`/`chore:`/`i18n:`/`fix:`),
  one logical change per commit, message in Chinese or English.
- Don't commit the built `bio` binary or build artifacts (gitignored).
  Build artifacts from `bio build` live in `.biolang/`.

## Gotchas

- Switching `PROFILE` requires `make clean` (make tracks timestamps, not values).
- `bio -b` requires `libbio.a`; if the binary is missing, run plain `make`.
- The compiler reuses the profile's CC/LDFLAGS via `-DBIO_CC='...'` string
  literals — keep `PROFILE_*` values single-quote safe.
- Dependency repo resolution: own `repo` → `BIOLANG_CONFIG` file → `~/.biolang/config.toml`.
- VSCode plugin lives in `vscode/biolang-vscode/`, installable to
  `~/.vscode/extensions` (versioned there, e.g. bio.biolang-0.15.0).
