# Qela

A self-hosting compiled language whose entire compiler is one static binary
under a megabyte. No libc, no linker, no assembler, no runtime to install —
the compiler writes its own ELF files and carries its standard library inside
itself.

```qela
import "std/io.qela";

fn main() int {
	write_str(STDOUT, "hello\n");
	return 0;
}
```

```
$ qela hello.qela -o hello && ./hello
hello
$ ldd hello
	not a dynamic executable
```

The design goal is the most language per byte. The compiler is currently
261 032 B — 60 196 B packed with `upx --lzma`, about 5.8% of its own
1 MiB budget.

## What it has

Fixed-width integers, `f32`/`f64` floating point (aliases `float` and
`double`), pointers, arrays, slices and `str`, structs with literals, enums
with payloads and exhaustive `match`, `defer`, and `comptime` blocks
evaluated during type checking. `assert(cond, "msg")` and `panic("msg")`
for the crash paths, and a panic backtrace under `qela run`.

A float is raw IEEE-754 bits in an ordinary general-purpose register; SSE
registers appear only for the instant of an operation, so the calling
convention and register allocation are untouched. Floats parse, print and
interpolate through pure integer arithmetic, because stage0 has no float —
the bootstrap subset stays float-free.

Generics by monomorphization, over functions and over types, with the type
argument inferred from the call:

```qela
struct Pair(T) { a T, b T, }

fn first(comptime T: type, p *Pair(T)) T { return p.a; }

var p Pair(i64);
var x i64 = first(&p);   // T is i64, nobody had to say so
```

Coroutines on their own stacks with `spawn`, and channels of any type:

```qela
var ch Chan(Msg);
chan_init(&ch, 8);

spawn producer(4);
spawn consumer(4);
coro_run_all();

ch <- m;
var v Msg = <-ch;
```

A conservative mark-sweep collector for programs whose lifetimes are not
stack-shaped; arenas remain the default. `std/rand.qela` is a deterministic
xorshift64* — `rand_init`, `rand_u64`, `rand_range` — seeded with a fixed
constant so unseeded programs reproduce. `std/vec.qela` is a generic
growable vector — `vec_push`, `vec_get`, and `vec_view` turns it into a
slice, so `for x in vec_view(&v)` is bounds-checked iteration.

The compiler is also its own build system and test runner. `qela .` merges
every `.qela` file in a directory into one program, no imports needed between
project files. `qela test file.qela` — or `qela test tests/*.qela` for a
whole corpus at once — compiles, runs and checks the `// expect-exit:`,
`// expect-out:` and `// expect-compile-error` comments at the top of each
file.

Strings interpolate: `"n = ${n}"` renders integers as decimal, strings in
place and pointers as hex, with the expression evaluated at runtime — a
string literal is compiled into a chain of formatting calls, so the feature
is stdlib, not magic. `for x in a` iterates an array, slice or string by
value, and on a fixed array the compiler proves the bounds away: the most
natural loop is also the smallest. `*volatile T` pointers keep MMIO reads
and writes honest, and `fn naked` emits a function with no prologue or
epilogue — the body is bare bytes ending in its own `ret` or `iretq`, for
interrupt handlers. `macro sq(x) = x * x;` are parse-time
expression macros whose expansion is a tree, not text: `sq(2 + 1)` needs
no parentheses, and the expansion is type-checked and bounds-checked like
any other code.

The image itself is controllable from source: top-level `asm { ... }`
blocks emit as the binary's first bytes, `entry name;` picks the ELF entry
and suppresses the call-main stub, and `$name` / `$rel name` inside asm
operands embed a symbol's absolute address or a rel32 slot — so a multiboot
header and a naked kernel entry are plain source, no linker script. `docs/ASM.md`
is the reference: common encodings as one-`let` rows, each verified on real
hardware.

DWARF line info behind `-g`, so gdb steps through `.qela` source and names
frames. `qela run` compiles and executes in one step. `qela fmt` formats over
the token stream, so comments survive and the output is idempotent. A
shebang first line is skipped, so scripts run as `#!/usr/bin/env qela run`;
`qela -` reads the source from stdin. `qela repl` compiles each line as an
expression and prints its value. `qela --lsp` is a language server in the
same binary: diagnostics, hover and go-to-definition over JSON-RPC.

`examples/lisp/` is a Lisp interpreter written in Qela — lexer, reader,
evaluator with closures and macros, REPL — that self-tests through
`qela test . tests.lisp`.

## Building

```sh
make                  # builds stage0, the C bootstrap compiler
make build            # the real compiler: stage0 -> S1a -> S2 -> S3, ships S2
make install          # installs build/bootstrap/s2 to /usr/local/bin/qela
```

`tools/bootstrap.sh` is the real gate: it rebuilds the compiler with itself
twice and requires the last two to be byte-identical, then runs the test
corpus and end-to-end checks for the standard library, coroutines, channels,
the collector, `run` and `fmt`, stdin and shebang, the panic backtrace,
interpolation and the repl, `qela test` and the lisp example.

The compiler you use is `build/bootstrap/s2`. `./qela` is a throwaway
bootstrap that exists only to compile the first stage.

## Layout

```
src/        stage0: the bootstrap compiler in C. Frozen; never shipped.
srcql/      the real compiler, in Qela
std/        the standard library, also baked into the binary
examples/   programs written in Qela (a lisp interpreter)
tests/      corpus, run by tools/run-tests.sh or `qela test tests/*.qela`
bench/      size benchmarks with C equivalents
docs/       BOOTSTRAP.md, STATUS.md
```

## Documentation

- `docs/BOOTSTRAP.md` — the subset the compiler's own sources may use, and why
- `docs/STATUS.md` — what works, what is left, with measurements
- `docs/ASM.md` — the x86 encodings Qela's `asm` covers, as one-`let` rows
