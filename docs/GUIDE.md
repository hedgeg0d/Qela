# Qela: a learning guide

This guide teaches you to program in Qela from zero. Everything here runs on
the shipped compiler, S2 (`build/bootstrap/s2`). Each example is a complete
program you can copy, compile and run.

Qela is a compiled systems language for x86-64 Linux:

- **No libc, no linker, no assembler.** The compiler writes its own ELF
  files. A Qela binary is a static executable and nothing else.
- **Self-hosting.** The compiler is written in Qela and compiles itself. The
  whole thing — compiler plus standard library — is one static binary under
  1 MiB.
- **Small on purpose.** Every feature is judged by wow-effect per byte. That
  is why there is no `interface`, no exceptions, and why the standard library
  is a handful of small files. Floats are the exception that proves the rule:
  `f32`/`f64` cost no calling convention — a float is raw bits in an ordinary
  register, with SSE only at the instant of an operation.
- **Memory is an arena by default**, with an optional conservative garbage
  collector for programs whose lifetimes are not stack-shaped.
- **Concurrency is cooperative**: coroutines on their own stacks, plus typed
  channels.

The quickest way to read this guide is to keep a terminal open and run the
examples as you go.

## Contents

1. [Getting the compiler](#1-getting-the-compiler)
2. [A first program](#2-a-first-program)
3. [Running programs](#3-running-programs)
4. [Types](#4-types)
5. [Variables](#5-variables)
6. [Operators](#6-operators)
7. [Control flow](#7-control-flow)
8. [Strings](#8-strings)
9. [Functions](#9-functions)
10. [Pointers and memory](#10-pointers-and-memory)
11. [Arrays and slices](#11-arrays-and-slices)
12. [Structs](#12-structs)
13. [Enums and match](#13-enums-and-match)
14. [Generics](#14-generics)
15. [Expression macros](#15-expression-macros)
16. [Comptime](#16-comptime)
17. [assert, panic, and bounds checks](#17-assert-panic-and-bounds-checks)
18. [Modules and the standard library](#18-modules-and-the-standard-library)
19. [Coroutines and channels](#19-coroutines-and-channels)
20. [The compiler as a tool](#20-the-compiler-as-a-tool)
21. [Raw machine code](#21-raw-machine-code)
22. [Where to look next](#22-where-to-look-next)
23. [Compiler flags reference](#23-compiler-flags-reference)

---

## 1. Getting the compiler

```sh
make          # build the bootstrap compiler (stage0, C)
make build    # the real compiler: stage0 -> S1a -> S2 -> S3, ships S2
make install  # optional: install to /usr/local/bin/qela
```

You use `build/bootstrap/s2`. There is also a throwaway `./qela` at the repo
root, but it is only there to build the first stage — treat it as a build
artifact, not the compiler. Check it works:

```sh
$ ./build/bootstrap/s2 examples/fizzbuzz.qela -o /tmp/fb && /tmp/fb
1
2
Fizz
...
```

If `make install` was run, the command is just `qela`. Everywhere below I
write `qela`; substitute `./build/bootstrap/s2` if you did not install.

## 2. A first program

The minimal program is a function named `main` that returns an int — the
process exit code:

```qela
fn main() int {
	return 42;
}
```

```sh
$ qela min.qela -o min && ./min; echo $?
42
```

Printing requires talking to the kernel directly. The `syscall` builtin wraps
the x86-64 syscall instruction; Linux syscall 1 is `write`. The classic
no-import hello world:

```qela
fn main() int {
	let msg = "Hello, Qela!\n";
	syscall(1, 1, msg.ptr, msg.len);   // write(STDOUT, msg, len)
	return 0;
}
```

`syscall` takes a syscall number followed by one to seven arguments (integers
or pointers). What is left in `rax` afterwards is the expression's value.

The comfortable way is the standard library, which wraps all of this:

```qela
import "std/io.qela";

fn main() int {
	write_str(STDOUT, "Hello, Qela!\n");
	return 0;
}
```

`import` pulls a standard module by path. The stdlib is baked into the
compiler binary, so this works in an empty directory with nothing but the
compiler present.

## 3. Running programs

```sh
qela file.qela [-o out]     # compile to a static binary (default: input name)
qela run file.qela [args]   # compile to a temp file, run it, print backtraces on panic
qela -                      # compile a program from stdin
qela . [dir]                # merge every .qela in a directory into one program
qela repl                   # one-liner REPL: type an expression, see its value
qela test file.qela         # run file, check its // expect-* comments (section 20)
qela fmt file.qela          # reformat over the token stream, idempotent
qela --lsp                  # language server: diagnostics, hover, go-to-def
qela --dump-std <module>    # print a standard module's embedded source
```

Flags: `-g` (DWARF for gdb), `--backtrace`, `--no-bounds-checks`,
`--no-warn`, `--color=auto|always|never`, `-o <file>`.

`qela .` is the compiler as its own build system: every `.qela` file in a
directory is merged into one program, so functions call across files with no
imports. The entry is `main.qela` (or the sole file). This is how
`examples/lisp/` is organised.

The REPL is handy for experiments:

```sh
$ qela repl
qela> 2 + 3 * 4
14
qela> "n = ${40 + 2}"
n = 42
```

Each REPL line is a fresh program compiled in a forked child, so it is
stateless and never accumulates anything.

## 4. Types

### Integers

Signed and unsigned, fixed width:

| type | bits | type | bits |
|---|---|---|---|
| `i8` | 8 | `u8` | 8 |
| `i16` | 16 | `u16` | 16 |
| `i32` | 32 | `u32` | 32 |
| `i64` | 64 | `u64` | 64 |

Plus the machine-word aliases `int`, `uint`, `usize` — 64-bit here. Integer
literals are typed by their value and context; there is no suffix. Values
widen implicitly through the ordinary arithmetic rules, but a narrowing cast
is explicit — unless the value is a constant that provably fits (section 6):

```qela
var b u8 = 200;
var w i32 = 70000;
var n i16 = w as i16;      // 70000 -> 4464, wraps
```

Integer arithmetic wraps on overflow — the language does not check it, by
design (the compiler itself wants the wrapping).

### bool

`true` and `false`. Comparisons and `&&`/`||` (which short-circuit) produce a
`bool`. There is no numeric conversion except through `as`:

```qela
var ok bool = (a < b) && (c != d);
var one i64 = ok as i64;   // 0 or 1
```

### str

A `{ptr, len}` pair into memory — a string is **not** a NUL-terminated blob
and not a character array. It has a `.len` (in bytes) and a `.ptr` (`*u8`).
Indexing yields the byte:

```qela
var s str = "abc";
var n i64 = s.len;         // 3
var c u8 = s[0];           // 'a' == 97
var p *u8 = s.ptr;
```

`str` costs two argument registers (ptr + len), which matters once you have
many parameters (section 9).

### Compound types

Pointers (`*T`), arrays (`[N]T`), slices (`[]T`), structs, enums — all in
later sections. There is no `void` value type (a function that returns nothing
simply omits the return type), no classes. There *is* floating point — next.

### f32 and f64

`f32` and `f64` (aliases `float` and `double`) are IEEE-754. A literal is
float when it has a `.` or an exponent: `1.5`, `2.0`, `1e3`, `1.5e2`,
`-0.25`. A digit is required on both sides of the dot, so `.5` and `1.` are
not literals.

```qela
var a f64 = 1.5;
var b f64 = a * 2.0;        // 3
var c f32 = 2.5;
var d f64 = 1e3;            // 1000
var e f64 = 1.5e2;          // 150
```

`+ - * /`, unary minus and all comparisons work on floats. `as` casts between
a float and an integer (float-to-int truncates) and between `f32` and `f64`:

```qela
var h i64 = 7;
var f f64 = h as f64;       // int -> float
var back i64 = f as i64;    // float -> int, truncates
var wide f64 = c as f64;    // f32 -> f64
var half f32 = a as f32;    // f64 -> f32
```

Inside, a float is raw bits in an ordinary register; SSE registers appear only
for the moment of an operation, so parameters, returns, struct fields and
arrays work like any 4- or 8-byte value. Interpolation prints floats
(`"${a}"` → `1.5`). Floats are not comptime constants, subnormal literals
flush to zero, and printing rounds half-up rather than half-to-even — fine for
games and numerics, not a libc.


## 5. Variables

```qela
var x int;          // zeroed at declaration: x == 0, always
var y int = 5;      // explicit type, initial value
var z = 5;          // type inferred: int
let w = 5;          // same as `var w = 5`; `let` just requires an initializer
```

The zeroing rule is deliberate and unlike C: a local declared inside a loop
starts clean **every** iteration. A bare `var` is never uninitialized memory.

Globals work the same way:

```qela
var counter int = 0;
var flags u8 = 0xf0;
let limit = 5 * 2;   // global let: initializer must be a constant expression
var zeroed i32;      // globals are zeroed too
```

Globals live in the data segment. Assignment and everything else work
identically; there are no getters or setters, just memory.

Shadowing an outer name in an inner block is legal and emits a warning
(disabled with `--no-warn`).

## 6. Operators

| category | operators |
|---|---|
| arithmetic | `+ - * / %` |
| comparison | `== != < <= > >=` |
| logical | `&&` `\|\|` `!` (short-circuit) |
| bitwise | `& \| ^ ~ << >>` |
| compound | `+= -= *= /= %= &= \|= ^= <<= >>=` |
| others | `as` (cast), `sizeof(T)`, `&` and `*` (section 10), `<-` (section 19) |

```qela
var s int = 0;
s = s + a - b;          // s = s + a - b
s += 10;                // compound forms for every binary op
var mask int = ~0x0f;
var shifted int = 1 << 8;
```

Division and remainder by a constant zero are compile errors; by a runtime
zero they do whatever the CPU does. `as` casts between any two integer types,
between integers and pointers, and between integers and floats
(section 4):

```qela
var p *u8 = 0 as *u8;
var addr i64 = p as i64;
```

Implicit conversions between integers happen only when the value cannot
change: **widening** (`i32` → `i64`, and `u8`/`u16`/`u32` → `i64`, which all
fit). A signed → unsigned widening (`i32` → `u64`), a signedness change at the
same width (`i64` → `u64`), and any narrowing (`i64` → `i32`) need an explicit
`as` — except a constant that fits, which is compile-time-checkable and stays
implicit:

```qela
var x i64 = some_u8;     // ok: u8 -> i64, value always preserved
var y u64 = 5;           // ok: signed constant that fits
var z u8 = 'A';          // ok: constant that fits
var v u32 = 300;         // ok: constant that fits
var w i64 = some_u64;    // error: u64 -> i64 needs `as i64`
var u u64 = some_i64;    // error: i64 -> u64 needs `as u64`
some_u64 + some_i64;     // error: mixing u64 and i64 needs a cast
```

Why: a narrowing cast can truncate, and a signed → unsigned cast reinterprets
a negative value as a huge positive one. Both are classic silent-corruption
bugs, so the compiler refuses them unless you write the cast — or the value is
a constant that provably fits.

`sizeof(T)` gives the size of a type in bytes:

```qela
if (sizeof(i64) != 8) { ... }
var n i64 = sizeof(Point);   // structs too, section 12
```

A character literal is just an 8-bit integer with nice syntax:

```qela
var nl u8 = '\n';
var q u8 = '\'';   // escaped quote
if ('A' == 65) { ... }
```

Escapes: `\n \t \r \0 \\ \' \"`.

There is no string concatenation operator. Build strings with a `Buf`
(section 18) or interpolation (section 8).

## 7. Control flow

### if / else

```qela
if (x > 0) {
	return 1;
} else if (x == 0) {
	return 0;
} else {
	return -1;
}
```

The condition must be a `bool` — there is no truthiness. There is no
`switch`; use `match` (section 13).

### while

```qela
var i int = 0;
while (i < 10) {
	i = i + 1;
}
```

### for over ranges

`for i in lo..hi` iterates `i` from `lo` to `hi` exclusive:

```qela
var sum int = 0;
for i in 0..5 {
	sum = sum + i;    // 0+1+2+3+4 = 10
}
```

The bound is re-evaluated each iteration (so `0..n` where `n` grows is
infinite in the same way C's loop is).

### for over collections

`for x in coll` iterates an array, slice or string by value:

```qela
var arr [4]i64;
for x in arr { ... }         // x is i64
var sl []i64 = arr[0..2];
for x in sl { ... }
var s str = "xy";
for c in s { ... }           // c is u8
```

On a fixed-size array the compiler proves the bounds away and emits no check;
on a slice or string the dynamic length keeps the check. The most natural
loop is also the smallest.

`break` and `continue` work in `for` and `while`.

### defer

A `defer` runs when its block exits — normally, via `return`, `break` or
`continue` — in LIFO order. This is the RAII substitute:

```qela
fn f() {
	var fh i64 = open_something();
	defer sys_close(fh);        // runs at every exit
	...
}
```

Deferred bodies may not `return`, `break` or `continue` (the compiler rejects
it).

## 8. Strings

`str` is a `{ptr, len}` pair; the bytes it points at are usually a string
literal or the interior of a buffer.

```qela
var s str = "hello";
var n i64 = s.len;
var c u8 = s.ptr[2];       // 'l'
for x in s { ... }         // bytes
```

String literals support the full escape set; an interpolated string may
contain any mix of literal text and `${...}` runs.

### Interpolation

The killer convenience: a string literal containing `${expr}` is rewritten
by the compiler into a chain of formatting calls. Integers render as decimal,
strings in place, pointers as hex.

```qela
import "std/io.qela";

fn main() int {
	var n i64 = 42;
	var s str = "hi";
	write_str(STDOUT, "n = ${n}, s = ${s}, sum = ${1 + 2}\n");
	return 0;
}
```

prints `n = 42, s = hi, sum = 3`. The expression is evaluated at runtime, and
the chain allocates its own buffer, so two interpolated strings alive at once
never alias. Any expression works, including a call: `"${fib(10)}"`.

Interpolation is a stdlib feature (it lowers to `fmt_*` calls in
`std/fmt.qela`) — it auto-imports, so no `import` is needed for it.

## 9. Functions

```qela
fn add(a int, b int) int {
	return a + b;
}

fn mark(v int) {
	// no return type: returns nothing
}
```

Calling is direct — the compiler emits a `call` and the callee's code is
ordinary machine code.

Parameter rules worth knowing:

- **Up to 6 parameters sit in the argument registers.** The 7th and later
  spill to the stack. **14 parameters is the hard cap**; the 15th is a
  compile error.
- A `str` (or an 8–16 byte struct) costs two argument registers, so six of
  them do not always fit.
- Arrays must be passed by pointer or as a slice; an array cannot be returned
  by value.

```qela
fn add6(a int, b int, c int, d int, e int, f int) int {
	return a + b + c + d + e + f;
}

fn sum_words(a int, b int, c int, d int, e int, f int,
             g int, h int, i int) int {          // 7th+ spill
	return a + b + c + d + e + f + g + h + i;
}
```

Recursion works, of course — the compiler itself is recursive.

### main

`main` returns the exit code. It may take no parameters:

```qela
fn main() int { return 0; }
```

or the C-style argv, as raw C strings:

```qela
import "std/io.qela";
import "std/str.qela";

fn main(argc int, argv **u8) int {
	var i int = 0;
	while (i < argc) {
		write_str(STDOUT, str_from_cstr(argv[i]));
		write_str(STDOUT, "\n");
		i = i + 1;
	}
	return 0;
}
```

## 10. Pointers and memory

Pointers are first-class. `&x` takes an address, `*p` dereferences, and the
arrow is spelled as a dot:

```qela
fn set(p *int, v int) {
	*p = v;
}

fn main() int {
	var x int = 5;
	var p *int = &x;
	*p = 40;
	set(&x, x + 2);
	return x;              // 42
}
```

Field access through a pointer is `p.x` (no `->`). Pointer arithmetic is
element-sized: on `*i64`, `p + 1` advances eight bytes.

```qela
var x int = 1;
var p *int = &x;
let q = p + 3;             // 12 bytes past x
var b u8 = 0;
var pb *u8 = &b;
let qb = pb + 4;           // 4 bytes past b
```

Casts between integers and pointers are explicit and complete: `p as i64`,
`0 as *u8`, `arena_alloc(16, 8) as *Point`.

### *volatile

`*volatile T` is a pointer whose loads and stores may never be elided or
reordered — MMIO registers, DMA buffers. It is part of the type, so dropping
it requires a cast. The compiler always emits the access and never folds it:

```qela
var status *volatile u8 = 0xfe000000 as *volatile u8;
var v u8 = *status;     // a real load, every time
```

### Allocation

There is no `malloc`/`free` in the language. The default allocator is an
arena: `arena_alloc(size, align)` in `std/arena.qela` hands out blocks and
never frees anything — the program's lifetime is the arena's. Great for
compilers, interpreters and short-lived programs; this is what the Qela
compiler itself uses.

```qela
import "std/arena.qela";

fn make_point(x int, y int) *Point {
	var p *Point = arena_alloc(sizeof(Point), 8) as *Point;
	p.x = x;
	p.y = y;
	return p;
}
```

For programs whose lifetimes are not stack-shaped, `std/gc.qela` is a
conservative mark-sweep collector — see section 18.

## 11. Arrays and slices

### Fixed arrays

```qela
var a [10]int;
for i in 0..10 {
	a[i] = i + 1;
}
```

Array length must be a positive compile-time constant. Indexing is bounds
checked at runtime; a bad index aborts with `index out of bounds` (exit 134).
The check can be disabled program-wide with `--no-bounds-checks`.

### Slices

A slice `[]T` is a `{ptr, len}` view into an array (or a string's bytes) with
no ownership. You get one by slicing:

```qela
var a [6]int;
var all []int = a[..];      // whole array
var mid []int = a[2..4];    // elements 2,3
var head []int = a[..3];    // elements 0,1,2
var tail []int = a[3..];    // elements 3..end
```

Slices have `.len` and index with the same checked `[]`; nested slices
re-slice. `for x in sl` iterates a slice by value:

```qela
fn sum(xs []int) int {
	var t int = 0;
	for x in xs { t = t + x; }
	return t;
}
```

Passing a slice is the idiomatic way to hand an array (or a piece of one) to
a function.

## 12. Structs

```qela
struct Point {
	x i32,
	y i32,
}

struct Line {
	a Point,
	b Point,
	tag u8,
}
```

Fields are comma-separated (trailing comma required by convention). Access is
`.` through values and pointers alike:

```qela
fn shift(p *Point, dx i32) {
	p.x += dx;       // no -> ; p.x works on a pointer
}
```

### Struct literals

```qela
var p Point = Point{x: 3, y: 4};
var q Point = Point{y: 10, x: 1};   // field order does not matter
var z Point = Point{x: 7};          // omitted fields are zero
var e Point = Point{};              // all zero
```

Literals work as call arguments, return values, in nested aggregates, and in
assignment:

```qela
var r Rect = Rect{
	lo: Point{x: 0, y: 0},
	hi: Point{x: 5, y: 6},
	tag: 'A',
};
p = Point{x: 20, y: 0};
```

A struct can contain itself only through a pointer (a by-value self-reference
is a compile error). Structs larger than 16 bytes are passed by pointer under
the hood; you do not have to do anything — but remember it when counting
argument registers.

Forward declarations are allowed so two structs can point at each other:

```qela
struct A;
struct B { a *A, }
struct A { b *B, }
```

## 13. Enums and match

Enums are tagged unions: each variant may carry a payload. A variant without
a payload is a plain tag.

```qela
enum Shape {
	Circle(int),
	Rect(int, int),
	Point,
}
```

Construction names the type:

```qela
var s Shape = Shape.Circle(5);
s = Shape.Rect(3, 4);
s = Shape.Point;
```

`match` is exhaustive — the compiler refuses a match that does not cover every
variant. Each arm binds the payload:

```qela
fn area(s Shape) int {
	match (s) {
		Circle(r) => { return r * r; }
		Rect(w, h) => { return w * h; }
		Point => { return 0; }
	}
}
```

The `_` arm catches what you do not want to name:

```qela
match (op) {
	Add(a, b) => { return a + b; }
	_ => { return -1; }
}
```

An enum has no implicit equality and cannot be compared with `==`; compare
by matching. Up to 64 variants.

## 14. Generics

Generics are monomorphized: every concrete instantiation is a copy of the
code, compiled once per type argument. A generic parameter is declared as a
comptime type and the argument is usually inferred:

```qela
fn swap(comptime T: type, a *T, b *T) {
	var tmp T = *a;
	*a = *b;
	*b = tmp;
}

var arr [4]u8;
swap(u8, &arr[0], &arr[1]);     // T = u8, explicit

fn first(comptime T: type, p *Pair(T)) T { return p.a; }
var p Pair(i64);
var x i64 = first(&p);          // T = i64, nobody had to say so
```

Parameterized structs and enums:

```qela
struct Pair(T) { a T, b T, }
struct Node(T) { next *Node(T), v T, }      // self-reference is fine
struct Map(K, V) { k K, v V, }
enum Opt(T) { None, Some(T), }
```

```qela
var p Pair(u8) = mk(u8, 3, 4);
var o Opt(i64) = Opt(i64).Some(9);
match (o) {
	Some(v) => { ... }
	None => { ... }
}
```

Each distinct instantiation is a distinct type; identical arguments name the
same type. `sizeof` follows the argument: `sizeof(Pair(u8)) == 2`,
`sizeof(Pair(i64)) == 16`.

## 15. Expression macros

`macro` gives you parse-time expression substitution — a tree splice, not
text:

```qela
macro sq(x) = x * x;
macro lerp(a, b, t) = a + (b - a) * t;
macro sh(x, k) = x << k;

var n i64 = 5;
var a i64 = sq(n);          // n * n
var b i64 = sq(2 + 1);      // no parentheses needed — trees, not text
```

A macro argument is re-evaluated for every use the body names it, and the
expansion goes through the ordinary type check and bounds checks. Up to six
parameters, expression bodies only. Macros resolve at parse time, so they must
be defined before use.

## 16. Comptime

`comptime { ... }` runs a block of ordinary Qela at compile time and the block
value becomes a constant. Loops, recursion, conditionals, even function calls:

```qela
fn fib_ct(n int) int {
	if (n < 2) { return n; }
	return fib_ct(n - 1) + fib_ct(n - 2);
}

fn main() int {
	let a = comptime { return 5 * 2; };          // 10
	let b = comptime { return fib_ct(8); };      // 21, compiled in
	let c = comptime {
		var sum int = 0;
		for i in 0..5 { sum += i; }
		return sum;                              // 10
	};
	return a + b + c;
}
```

Useful for table generation, sizes, and anything you want to compute once and
not ship. A comptime block that tries to produce a string is an error.

## 17. assert, panic, and bounds checks

Stage1-only builtins (they are not in the bootstrap compiler, but the shipped
S2 has them):

```qela
assert(cond, "message");
panic("message");
```

`panic` prints the message, then — under `qela run` — a backtrace naming the
panicking function and its callers, and exits 134. The message must be a
string literal.

Bounds checks are always on by default: any out-of-range array, slice or
string index aborts with `index out of bounds`. The redundant-check elision
(compiler proves a loop index stays in range) means the most natural loops
carry no check at all.

## 18. Modules and the standard library

`import "std/name.qela"` pulls a module. Because the library is embedded in
the compiler, imports resolve with nothing on disk. A program importing
`std/io.qela` compiles in an empty directory.

The standard library is deliberately small — each module is a few dozen to a
couple of hundred lines of Qela and you can read the source with
`qela --dump-std io.qela` or in `std/`.

### The ones you will use most

| task | reach for |
|---|---|
| print a string | `write_str(STDOUT, "..." )` from `std/sys.qela` |
| print a number / anything | string interpolation `"x = ${x}"` (auto-imports `std/fmt.qela`) |
| print to stderr | `eprint("...")` from `std/sys.qela` |
| read a whole file | `read_file(path)` → `str` |
| read a line from stdin | `read_line()` → `str`, `""` at EOF |
| build a string / binary data | `Buf` + `buf_u8` / `buf_bytes` / `buf_str` |
| growable array of anything | `Vec(T)`: `vec_init`, `vec_push`, `vec_get`, `vec_view` |
| dictionary | `Map`: `map_put`, `map_get` (keys `str`, values `*u8`) |
| list of pointers | `List`: `list_push`, `list_get` |
| random number | `rand_range(lo, hi)` in `[lo, hi)` |
| sort `i64`s | `sort_i64(slice, 0, n-1)` |
| allocate | `arena_alloc(size, align)` — never frees |
| object lifetimes that are not stack-shaped | `gc_alloc`, `gc_collect` |

### sys — raw syscalls

`std/sys.qela`. Constants for syscall numbers (`SYS_READ`, `SYS_WRITE`,
`SYS_EXIT`, ...), `STDOUT`/`STDERR`, and thin wrappers:

```qela
sys_write(fd i64, buf *u8, n i64) i64
sys_read(fd i64, buf *u8, n i64) i64
sys_open_read(path *u8) i64
sys_open_exec(path *u8) i64     // O_WRONLY|O_CREAT|O_TRUNC, 0755
sys_close(fd i64) i64
sys_exit(code i64)
sys_mmap_anon(n i64) i64
sys_fork() i64
sys_execve(path *u8, argv *u8, envp *u8) i64
sys_wait4(pid i64, status *u8) i64
sys_unlink(path *u8) i64
sys_getenv(name str) str
write_str(fd i64, s str)        // the everyday print helper
```

### io — files, stdin, directories

`std/io.qela`. `write_str` and `eprint` come from `sys`; the rest is here:

```qela
die(msg str)                    // eprint + newline + exit 1
file_exists(path str) bool
read_file(path str) str         // whole file; exits on error
read_stdin() str                // whole stdin
read_line() str                 // one line, newline included; "" at EOF
write_file(path str, data *u8, n i64)
is_dir(path str) bool
read_dir(path str, out *Buf)    // appends .qela filenames, one per line
```

A `str` from `read_file` is arena-owned and lives until the program ends.

### str — string helpers

`std/str.qela`:

```qela
str_eq(a str, b str) bool
str_len_cstr(p *u8) i64
str_from_cstr(p *u8) str
str_slice(s str, lo i64, hi i64) str
str_find(s str, c u8) i64       // index or -1
str_dup(s str) str              // arena copy, NUL-terminated
cstr(s str) *u8                 // NUL-terminated copy, for syscalls
```

### buf — growable byte buffer

`std/buf.qela`. The workhorse for building strings and files:

```qela
struct Buf { p *u8, n i64, cap i64, }

buf_grow(b *Buf, need i64)
buf_str(b *Buf) str             // the buffer as a str (not a copy)
buf_u8 / buf_u16 / buf_u32 / buf_u64
buf_bytes(b *Buf, data *u8, n i64)
buf_patch32(b *Buf, at i64, v i64)
```

Pattern:

```qela
import "std/buf.qela";
import "std/io.qela";

fn main() int {
	var b Buf;
	buf_u8(&b, 'H');
	buf_bytes(&b, "ello".ptr, 4);
	var s str = buf_str(&b);
	write_str(STDOUT, "${s} ${s.len}\n");   // Hello 5
	return 0;
}
```

### fmt — number formatting

`std/fmt.qela`. Mostly the interpolation machinery, but the pieces are public:

```qela
fmt_i64(b *Buf, v i64)
fmt_hex(b *Buf, v i64)
fmt_str(b *Buf, s str)
eprint_i64(v i64)
```

### arena — the default allocator

`std/arena.qela`: `arena_alloc(size i64, align i64) *u8`. Never frees.

### vec — generic growable vector

`std/vec.qela`. `Vec(T)` with `vec_init`, `vec_push`, `vec_get`
(out-of-range reads return a zeroed element), `vec_pop`, `vec_clear`, and
`vec_view` — the slice view that lets a vector participate in checked
iteration:

```qela
import "std/io.qela";
import "std/vec.qela";

fn main() int {
	var v Vec(i64);
	vec_init(&v);
	var i i64 = 0;
	while (i < 20) {
		vec_push(&v, i * 3);
		i = i + 1;
	}
	var t i64 = 0;
	for x in vec_view(&v) { t = t + x; }
	write_str(STDOUT, "${t}\n");        // 570
	return 0;
}
```

### list — growable pointer array

`std/list.qela`. Elements are opaque `*u8`; the caller casts:

```qela
list_push(l *List, p *u8)
list_get(l *List, i i64) *u8   // 0 on out of range
list_len(l *List) i64
```

### map — str -> *u8

`std/map.qela`. Insertion-order iteration, O(N) lookup (N is tiny in a
compiler):

```qela
map_put(m *Map, key str, val *u8)   // update in place, no duplicate keys
map_get(m *Map, key str) *u8        // 0 when absent
map_len(m *Map) i64
map_key(m *Map, i i64) str          // iteration
map_val(m *Map, i i64) *u8
```

### math, sort, rand

```qela
abs(v i64) i64 / min(a, b) / max(a, b)     // std/math.qela
sort_i64(s []i64, lo i64, hi i64)          // std/sort.qela, Hoare quicksort
rand_init(seed i64)                        // std/rand.qela, xorshift64*
rand_u64() i64                             // deterministic unless seeded
rand_range(lo i64, hi i64) i64             // [lo, hi); degenerate -> lo
```

### gc — conservative mark-sweep collector

`std/gc.qela`, for programs whose lifetimes are not stack-shaped. It roots
the callee-saved registers, the stack and the data segment, so it needs no
cooperation from the type checker:

```qela
import "std/gc.qela";

struct Node { val i64, next *Node, }

var head *Node;

fn push(v i64) {
	var n *Node = gc_alloc(sizeof(Node)) as *Node;
	n.val = v;
	n.next = head;
	head = n;
}

gc_collect();               // mark + sweep
gc_live_bytes() i64         // bytes still live
```

Arenas remain the default; reach for `gc` only when object lifetimes are
genuinely graph-shaped.

## 19. Coroutines and channels

Concurrency is cooperative: coroutines run on their own stacks and hand
control to each other explicitly. There is no preemption, no locks, no data
races from the scheduler.

### Coroutines

```qela
import "std/coro.qela";
import "std/fmt.qela";

var log Buf;

fn worker(id i64, rounds i64) {
	var i i64 = 0;
	while (i < rounds) {
		fmt_i64(&log, id);
		coro_yield();
		i = i + 1;
	}
}

fn main() int {
	spawn worker(1, 3);      // up to 5 arguments
	spawn worker(2, 3);
	spawn worker(3, 2);
	coro_run_all();          // round-robin until everyone is done
	syscall(1, 1, log.p, log.n);   // the log is "12312312"
	return 0;
}
```

`spawn f(args)` starts `f` on a fresh 64 KiB stack; `coro_yield()` hands
control to the next runnable coroutine; `coro_run_all()` runs the herd to
completion. The main context is slot 0, so the program is itself a coroutine.

### Channels

`std/chan.qela`. `Chan(T)` is a typed, buffered channel. Send is `ch <- v`,
receive is `<-ch`; the element type is read off the channel.

```qela
import "std/chan.qela";

var ch Chan(i64);
var got i64 = 0;

fn producer(n i64, base i64) {
	var i i64 = 0;
	while (i < n) { ch <- base + i; i = i + 1; }
}

fn consumer(n i64) {
	var i i64 = 0;
	while (i < n) { got = got + <-ch; i = i + 1; }
}

fn main() int {
	chan_init(&ch, 2);       // two-slot buffer
	spawn producer(4, 10);
	spawn consumer(4);
	coro_run_all();
	if (got != 46) { return 1; }   // 10+11+12+13
	return 0;
}
```

Waiting is polling over the scheduler: a blocked send or receive yields until
the channel changes. `chan_init(&ch, 0)` is a rendezvous — the sender hands
the value directly to the receiver and neither proceeds until they meet.
`chan_close(&ch)` closes a channel; a receive on a drained closed channel
returns a zeroed element (pair it with `chan_is_closed` when zero is also a
valid value).

**Deadlock** — every coroutine parked and no channel changed for long enough
that everyone had its turn — is detected and reported rather than spun on:

```sh
qela: deadlock, every coroutine is blocked on a channel receive
```

## 20. The compiler as a tool

### qela test — the built-in test runner

A corpus file declares its expectations in leading comments:

```qela
// expect-exit: 42
// expect-out: hello
// expect-out: world
// expect-compile-error
```

```sh
qela test tests/array.qela        # one file
qela test tests/*.qela            # the whole corpus
qela test dir.qela -- 5 6         # pass arguments after --
```

The compiler compiles, runs, compares exit code and stdout lines in order,
and prints `ok name` / `FAIL name: detail`. `tests/` is a full corpus of
these, one file per feature.

### qela fmt

Formats over the token stream, so comments survive and the output is
idempotent. Run it on a messy file and commit the result.

### qela repl

Each line compiles as `write_str(STDOUT, "${line}")` in a forked child:
integer, string and pointer expressions all render through interpolation.
Stateless — every line is a fresh program.

### qela --lsp

A language server in the same binary: JSON-RPC over stdio, full-document
sync, diagnostics, hover with types and signatures, go-to-definition for
locals, globals, functions and fields. `tools/lsp-test.py` scripts a
conversation against it.

### Debugging

- `-g` emits DWARF line info and a symbol table, so `gdb` steps through the
  `.qela` source and names frames.
- `--backtrace` prints the function call chain on panic even for plain
  compiles; `qela run` does it by default.
- `--dump-std <module>` prints any standard module's source — the whole
  library is 15 small files, worth reading.

### qela . — projects

Merge a directory of `.qela` files into one program (entry `main.qela`).
Functions call across files with no imports; imports inside files still work,
and a file already merged by path is skipped. The compiler is its own build
system.

## 21. Raw machine code

Qela's low-level escape hatches, for kernels, bootloaders and bare-metal.

### asm

`asm(byte, ...)` splats raw x86-64 bytes into the code. Operands are comptime
constants; a value wider than a byte is a byte string written most-significant
byte first:

```qela
asm(0x48c7c0, 60, 0, 0, 0);   // mov rax, 60
asm(0x48c7c7, 42, 0, 0, 0);   // mov rdi, 42
asm(0x0f, 0x05);              // syscall
```

`$name` inside an operand embeds a symbol's absolute address and `$rel name`
a rel32 slot, patched by the same machinery that patches call targets. See
`docs/ASM.md` — common encodings as one-`let` rows, each verified on real
hardware.

### fn naked

A function with no prologue, no epilogue and no implicit `ret` — the body is
bare bytes ending in its own `ret` or `iretq`. For ISR entries and syscall
stubs. It cannot take parameters, have locals, return, defer or call; only
`asm` and `syscall`:

```qela
fn naked weird_entry() int {
	asm(0x48, 0xc7, 0xc0, 42, 0, 0, 0);   // mov rax, 42
	asm(0xc3);                             // ret
}
```

### The image itself

Top-level `asm { ... }` blocks emit as the binary's first bytes (a multiboot
header fits in the first 8 KiB), and `entry name;` picks the ELF entry and
suppresses the call-main stub — a kernel entry is plain source:

```qela
asm { 0x1badb002, 0, 0xe4524ffe };   // multiboot header

var stack_top [256]u8;

entry start;

fn naked start() {
	asm(0x48, 0xbc, $stack_top);      // mov rsp, stack_top
	asm(0xe9, $rel kmain);            // jmp kmain
}
```

## 22. Where to look next

- **`tests/`** — one file per feature, with the expected behavior in the
  leading comments. The best reference: `tests/structlit.qela`,
  `tests/enum.qela`, `tests/generictype.qela`, `tests/chan.qela`,
  `tests/vec.qela`, `tests/topasm.qela`.
- **`examples/lisp/`** — a complete Lisp interpreter in Qela: lexer, reader,
  evaluator with closures and macros, REPL. Runs `qela test . tests.lisp`.
- **`examples/fizzbuzz.qela`** — the annotated tour of a small real program.
- **`docs/ASM.md`** — raw x86-64 encodings.
- **`docs/STATUS.md`** — what works and what is left, with measurements.
- **`docs/BOOTSTRAP.md`** — the subset the compiler's own sources must stay
  inside. Relevant only if you start hacking on the compiler.
- The standard library itself, `std/` (or `qela --dump-std`): 15 small
  files, all of it readable Qela.

## 23. Compiler flags reference

The compiler is one binary; the subcommands (`run`, `test`, `fmt`, `repl`,
`.`), `--lsp` and `--dump-std` are described in sections 3 and 20. This is
the rest of the command line, flag by flag.

### Output

| flag | meaning |
|---|---|
| *(no input flag)* | compile `file.qela` to a static executable |
| `-o <file>` | output path; default is the input name without `.qela` |
| `-` | read the source from stdin (as `<stdin>`) |

```sh
qela hello.qela -o hello && ./hello
echo 'fn main() int { return 7; }' | qela - -o /tmp/seven && /tmp/seven
```

### Debugging

| flag | meaning |
|---|---|
| `-g` | DWARF line info and a symbol table: `gdb` steps through the `.qela` source and names frames. Adds real ELF section headers, so `objdump -d` disassembles too — plain compiles ship no section headers at all and objdump finds nothing to show |
| `--backtrace` | print the function call chain on panic. Off by default on plain compiles (the deterministic output the bootstrap gate compares); `qela run` always sets it |

```sh
qela crash.qela -g          # then: gdb ./crash
qela crash.qela --backtrace # panic names the frames
qela run crash.qela         # same, always on
```

### Runtime checks

| flag | meaning |
|---|---|
| `--no-bounds-checks` | drop every runtime index check from the output. Smaller and faster, and a bad index becomes memory garbage instead of an abort — use only after the checked build is proven |

### Diagnostics

| flag | meaning |
|---|---|
| `--no-warn` | suppress warnings (unused variable, shadowed name) |
| `--color=auto\|always\|never` | coloured diagnostics; `auto` is a tty and `NO_COLOR` unset |
| `-h`, `--help` | the full usage text |

```sh
qela w.qela --no-warn         # build with warnings silenced
qela w.qela --color=never     # plain output for a pipe or a log
qela -h
```

### Worth remembering

- The default build is the *smallest* one: no section headers, no debug
  info. Add `-g` only when you need gdb or objdump.
- These flags belong to the plain-compile path, which `qela .` also uses, so
  `qela . -o out` works. `qela run` and `qela test` do **not** take them:
  every argument after the input file goes to the program, not the compiler
  (`qela run a.qela --no-bounds-checks` would hand the flag to `a` itself).
- `--dump-std <module>` prints a standard module's source — the best way to
  see exactly what a std function does.
