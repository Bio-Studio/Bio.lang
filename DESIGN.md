# BiuBiuBiu — Design

## The engine: streams

1. **Unistream** — the unified stream, an abstract concept every stream belongs to.
2. **Comstream** — the computation stream, a branch of transient streams that handles all kinds of instant computations.
3. **Remstream** — the memory stream; a program remembers things while it runs. By default it persists and recalls memory (users may override the persist and recall methods).
4. **Objstream** — the object stream, a branch of the memory stream dedicated to relations and data between objects. Every object is also a stream storing its own attributes (if an attribute is washed away, calling certain methods refuses outright).
5. **Threadstream** — the process/thread stream; a process usually contains a computation stream, a memory stream, a timing stream and an IO stream, and a process can split off several processes/threads.
6. **Timestream** — the timing stream; it can own several timers at once. By default the first timer belongs to the thread and may not be reset to zero; forked second/third timers may.
7. **IOStream** — the IO stream, present by default as an abstract parent; it carries no functionality itself — CIO (console), FIO (file) and SIO (string) are its real implementations.
8. **CustomStream** — custom streams; streams are not limited to the kinds above — they can be composed and combined with binary programs.
9. **Mainstream** — the main program stream; whenever a program is a main program, its `exec()` method is invoked directly.
10. **Functionstream** — the function stream; it supports forking several functions, directly under Unistream. In this language, a *function* means a shared, quick toolbox.
11. **Constantstream** — the constant stream; it supports forking several constants, under Unistream and Threadstream. In the terms of this language: public constants and private constants.
12. **Areastream** — the scope stream; it contains a memory stream.
13. **Taskmstream** — the task-manager stream that schedules automatically; it contains a Threadstream.
14. **Solidstream** — the contiguous stream; it stores data contiguously, auto-allocates and moves a head pointer. (The ordinary Array class and the Vector class are implemented in Bio code, not in the interpreter.)
15. **CIOStream** — the Console implementation of IOStream.
16. **FIOStream** — the File implementation of IOStream.
17. **SIOStream** — the String implementation of IOStream.

## Syntax

### Variable modifiers

```
const int x = 10; // adds to Constantstream
int x = 10;       // adds to the scope stream by default
thread int x = 10; // thread variable
```

### Base types

```
int
float
double
string
char
```

Every operation in BioLang is a request, and a request may naturally be refused. We express this with the following syntax:

```
ref "reason";  // refuse the request with a reason
```

Requests generally expect a response; respond with `res`, and unwrap the two
halves with the prefix operators `get` and `cause`:

```
res value;                    // respond with a value
ref "reason";                 // refuse with a reason
ALL result = add(a, b);       // ALL carries the response and the refusal reason
get result                    // the actual returned value
cause result                  // the reason the request was refused
```

### Stream syntax

Custom stream:

```
Stream SStream {
    void hello(); // signature
    int flag;     // custom field, usable when forking the stream
}
```

Custom class (a class is essentially a stream in BioLang):

```
Class CClass {    // Class here means forking the Class stream
    void __init__() {
        ...
    }
    int n;    // default object field
    int[] a;  // default object array
    /*
    or:
    type T;   // close to generics, but more flexible
    T n;
    T[] a;
    */
}
```

Creating an object:

```
CClass c = new CClass();
```

Calling a stream:

```
stream::hello();
```

Forking a stream:

```
SStream SSStream {
    void hello() {
        CIO::println("Hello World");
    }
}
```

Main program stream:

```
program main; // declares this is a main program

Main { // overwrites an already-existing stream directly
    void exec() { // exec() supports dynamic return values, since return values are accepted via the root stream Objstream
        CIO::print("Hello World!");
    }
}
```

Declaration syntax:

```
program main;
```

```
program utils; // declares this is a toolbox
```

### Assumption syntax

Assume a stream:

```
need Stream ... { ... } ; // first ... is the stream name, second ... what is needed; must combine with a toolbox
```

Assume a value:

```
need value ...; // the ... is the name of the value
```

Assume a class:

```
need Class ... {...};
```

Assume a function:

```
need function ...; // the ... is the name of the function
```

The third and second forms are essentially variants of the first assumption form. Assumption syntax is a special, standalone syntax.

### Smart references

`r`, `w`, `rw`, `m` (movable) \* `u` (unit/program-level), `f` (function/method), `a` (area/scope)

A reference is a **type** and it is generic: `&perm follow base` is the type,
with any base type (int/double/float/string/char/arrays/classes). The
permission is any stack of `r` (read) / `w` (write) / `m` (move):
`r, w, m, rw, rm, wm, rwm` (7), and the follow layer is `u` (program),
`f` (method), `a` (area) or `t` (thread) — 7 × 4 = **28 reference types**.
Declare it and point it at an lvalue expression with `&`:

```
&r   u int p = &x;        // read-only program-level int reference
&w   u int p = &a[0];     // write-only reference to an array element
&rw  u int p = &obj.f;    // read-write reference to an object field
&rm  u int p = &a[1];     // read + move pointer (p++ advances to a[2])
&rwm t int p = &a[1];     // read-write-move thread-layer reference
```

Read through `get p`, write through `p = v`, move the pointer with `p++` or
`Ref::move(p)` (m permission only). The `Ref` stream also exposes
`read/write/move/target/perm` as method forms.

## Removed syntax: bare binary calls

`&func(...)` — a global call that searched every loaded binary library for an
exported symbol — is **deprecated and removed**: the implicit symbol lookup is
too dangerous. It can silently bind a call to the wrong library or the wrong
function (or to arbitrary symbols in the process), offers no static
guarantees, and bypasses the stream model entirely. Always call exported
functions through their binary library stream:

```
m::pow(2, 10)   // ✓ qualified stream method
&pow(2, 10)     // ✗ removed — bare binary calls are no longer accepted
```

## Modes

This language can be interpreted as well as compiled.
