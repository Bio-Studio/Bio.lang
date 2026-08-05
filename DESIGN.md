# BioLang — Design

## The engine: streams

1. **Unistream** — the unified stream, an abstract concept every stream belongs to.
2. **Comstream** — the computation stream, a branch of transient streams that handles all kinds of instant computations.
3. **Remstream** — the memory stream; a program remembers things while it runs. By default it persists and recalls memory (users may override the persist and recall methods).
4. **Objstream** — the object stream, a branch of the memory stream dedicated to relations and data between objects. Every object is also a stream storing its own attributes (if an attribute is washed away, calling certain methods refuses outright).
5. **Threadstream** — the process/thread stream; a process usually contains a computation stream, a memory stream, a timing stream and an IO stream, and a process can split off several processes/threads.
6. **Timestream** — the timing stream; it can own several timers at once. By default the first timer belongs to the thread and may not be reset to zero; forked second/third timers may.
7. **IOStream** — the IO stream, present by default; it can read and output.
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
ref ...; // the ... may be any variable
```

Requests generally expect a response; our functions call respond and refuse explicitly, with the following syntax:

```
res add(a, b);      // the result of add(a,b)
cause add(a, b);    // the reason add(a,b) was refused
ALL result = add(a, b); // ALL is a structure containing both res and cause
res result;
cause result;
// the above and more flexible equivalents
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
        IO::println("Hello World");
    }
}
```

Main program stream:

```
program main; // declares this is a main program

Main { // overwrites an already-existing stream directly
    void exec() { // exec() supports dynamic return values, since return values are accepted via the root stream Objstream
        IO::print("Hello World!");
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

```
realme &r u int
realme &w u int
realme &r f int
realme &m f int
...
```

## Modes

This language can be interpreted as well as compiled.
