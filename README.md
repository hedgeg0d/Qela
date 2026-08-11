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
157 KB, about 3.5% of its own budget.

## What it has

Fixed-width integers, pointers, arrays, slices and `str`, structs with
literals, enums with payloads and exhaustive `match`, `defer`, generics by
monomorphization, and `comptime` blocks evaluated during type checking.

Coroutines on their own stacks with `spawn`, and channels:

```qela
spawn producer(4, 10);
spawn consumer(4);
coro_run_all();

ch <- 42;
var v i64 = <-ch;
```

A conservative mark-sweep collector for programs whose lifetimes are not
stack-shaped; arenas remain the default.

DWARF line info behind `-g`, so gdb steps through `.qela` source and names
frames. `qela run` compiles and executes in one step. `qela fmt` formats over
the token stream, so comments survive and the output is idempotent.

## Building

```sh
make                  # builds stage0, the C bootstrap compiler
tools/bootstrap.sh    # stage0 -> S1a -> S2 -> S3, then ships S2
```

`tools/bootstrap.sh` is the real gate: it rebuilds the compiler with itself
twice and requires the last two to be byte-identical, then runs the test
corpus and end-to-end checks for the standard library, coroutines, channels,
the collector, `run` and `fmt`.

The compiler you use is `build/bootstrap/s2`. `./qela` is a throwaway
bootstrap that exists only to compile the first stage.

## Layout

```
src/        stage0: the bootstrap compiler in C. Frozen; never shipped.
srcql/      the real compiler, in Qela
std/        the standard library, also baked into the binary
tests/      corpus, run by tools/run-tests.sh
bench/      size benchmarks with C equivalents
docs/       BOOTSTRAP.md, STATUS.md
```

## Documentation

- `docs/BOOTSTRAP.md` — the subset the compiler's own sources may use, and why
- `docs/STATUS.md` — what works, what is left, with measurements
