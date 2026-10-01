<p align="center">
  <img src="logo_wide.png" alt="Qela" width="720">
</p>

<p align="center">
  <strong>A self-hosting systems language, compiler, runtime, and developer toolkit<br>packed into a single static binary under 1 MiB.</strong>
</p>

<p align="center">
  <a href="docs/GUIDE.md"><img alt="Documentation" src="https://img.shields.io/badge/docs-language_guide-6c63ff?style=flat-square"></a>
  <a href="docs/STATUS.md"><img alt="Compiler size: under 1 MiB" src="https://img.shields.io/badge/compiler-%3C_1_MiB-00a878?style=flat-square"></a>
  <a href="docs/BOOTSTRAP.md"><img alt="Self-hosting" src="https://img.shields.io/badge/bootstrap-self--hosting-f39c12?style=flat-square"></a>
  <a href="#three-native-targets"><img alt="Targets: x86-64, ARM64, RISC-V 64" src="https://img.shields.io/badge/targets-x86--64_%7C_ARM64_%7C_RISC--V_64-3273dc?style=flat-square"></a>
  <img alt="Version: alpha 0.1.0" src="https://img.shields.io/badge/version-alpha--0.1.0-e76f51?style=flat-square">
</p>

---

Qela turns source code directly into native Linux ELF executables. It needs no
libc, linker, assembler, VM, package of runtime files, or external standard
library. The compiler writes the executable itself—and carries the language's
standard library, interpreter, dynamic runtime, REPL, formatter, test runner,
documentation browser, and LSP inside the same binary.

The current local self-hosted x86-64 compiler is **792,688 bytes** (~774 KiB).
That is the entire distribution, not a compressed installer. See the latest
reproducible measurements in [Project status](docs/STATUS.md).

```qela
import "std/io.qela";

fn main() int {
	let answer = 6 * 7;
	write_str(STDOUT, "Hello from ${answer} tiny, self-hosted kilobytes.\n");
	return 0;
}
```

```console
$ qela hello.qela -o hello && ./hello
Hello from 42 tiny, self-hosted kilobytes.
$ ldd hello
	not a dynamic executable
```

## One megabyte. Very few compromises.

| | What ships in the binary |
|---|---|
| **Native compiler** | Direct x86-64, ARM64, and RISC-V 64 machine-code generation; static ELF, PIE, DWARF, relocatable `.o` files, and its own linker |
| **Two ways to run** | A native tree-walking interpreter with `qela irun`, plus interpreted and runtime-JITed `dynamic` functions inside compiled programs |
| **Dynamic when you want it** | Optional `~` values, parameters, fields, arrays, and returns alongside the statically typed systems core |
| **Batteries included** | 39 embedded, documented standard modules: files, networking, HTTP, JSON, CSV, hashing, processes, signals, collections, allocators, GC, and more |
| **Developer tools** | REPL, formatter, test runner, project builder, docs lookup, diagnostics, hover, and go-to-definition via the built-in LSP |
| **Low-level control** | Raw syscalls, pointers, volatile access, atomics, inline bytes, naked/interrupt functions, custom entry points, and fixed-base images |
| **C interop** | Import and export functions and globals through the native ABI; emit PIE-safe objects and link Qela and C in either direction |
| **Self-hosting** | The production compiler is written in Qela, rebuilds itself, and must reach a byte-identical S2/S3 fixed point |

Qela is intentionally small, but it is not a toy subset disguised as a systems
language. It has fixed-width integers and floats, arrays and slices, structs,
payload enums with exhaustive `match`, generics, overloads, closures, expression
macros, `comptime`, `defer`, default arguments, multiple assignment, string
interpolation, list/map comprehensions, `Opt`/`Res` with `?`, bounds-check
elision, coroutines, typed channels, and real OS threads.

[Explore the complete language guide →](docs/GUIDE.md)

## Choose your level

The same language can be used as a convenient tool, a native application
language, or a freestanding substrate.

```console
# Learn and experiment
qela repl
qela irun script.qela

# Compile and run a static native executable
qela run app.qela argument

# Build every .qela file in a project—no separate build system
qela .

# Cross-compile
qela app.qela --target arm64  -o app-arm64
qela app.qela --target riscv64 -o app-riscv64
```

At the high level, write comprehensions, tagged data, generics, and error
propagation:

```qela
import "std/vec.qela";

enum Shape { Circle(f64), Rect(f64, f64), Point, }

fn area(s Shape) f64 {
	match (s) {
		Circle(r)  => { return 3.141592653589793 * r * r; }
		Rect(w, h) => { return w * h; }
		Point      => { return 0.0; }
	}
}

fn even_squares() []i64 {
	let values = [1, 2, 3, 4, 5, 6];
	return [x * x for x in values if x % 2 == 0];
}
```

At the lowest level, own the first byte of the image and its entry point:

```qela
asm { 0x1badb002, 0, 0xe4524ffe }; // Multiboot header
entry start;

fn naked start() {
	asm(0xe9, $rel kmain);
}
```

The repository includes [MiniOS](examples/minios/README.md), an x86-64 kernel
with interrupts, paging, a keyboard driver, a shell, and ring-3 programs—all
written in Qela.

## Native, interpreted, or dynamic

Qela does not force one execution model on the whole program.

```console
qela app.qela             # ahead-of-time native executable
qela irun app.qela        # execute the typed AST; emit no binary
qela app.qela --interpreted
qela app.qela --jit       # compile function bodies on their first call
```

Compiled programs can mark individual functions `interpreted` or `dynamic`.
The dynamic path can recompile a function body to native code at first call and
even rebind behavior at runtime. Separately, the `~` type provides opt-in boxed
dynamic values within otherwise static code. These features are independent;
see [Interpreted and dynamic functions](docs/GUIDE.md#32-interpreted-and-dynamic-functions)
and [Variables](docs/GUIDE.md#5-variables).

## A standard library you can carry—and read

The stdlib is embedded, resolves in an empty directory, and remains ordinary
Qela source. Browse it in [`std/`](std/) or from anywhere with the compiler:

```console
qela doc std json
qela doc std json_parse
qela --dump-std json.qela
```

It covers raw syscalls, I/O, paths and filesystems, buffers, strings, formatting,
arenas, heap allocation, vectors, maps, optional/results, JSON, CSV, encoding,
SHA-256, math, sorting, randomness, time, TCP/HTTP, processes, signals,
coroutines, channels, OS threads, and a conservative mark-sweep collector.

[Standard library tour and API index →](docs/GUIDE.md#18-modules-and-the-standard-library)

## Three native targets

| Target | Native code | Static ELF | PIE | C ABI / objects | Self-host verified |
|---|:---:|:---:|:---:|:---:|:---:|
| x86-64 | ✓ | ✓ | ✓ | ✓ | ✓ |
| ARM64 | ✓ | ✓ | ✓ | ✓ | ✓ |
| RISC-V 64 | ✓ | ✓ | ✓ | ✓ | — |

The cross-target corpus and C FFI gates run under QEMU; ARM64 has also been
verified on real hardware. Exact results and known limitations live in
[Project status](docs/STATUS.md).

## C in either direction

Qela can emit a relocatable, PIE-safe object instead of an executable:

```console
qela -c lib.qela -o lib.o
gcc -o c-app c-app.c lib.o       # C calls Qela

qela -c app.qela -o app.o
gcc -o qela-app app.o impl.c     # Qela calls C
```

Functions, globals, scalar floats, strings, and naturally laid-out structs
cross through the native ABI. The details and current boundaries are in
[C interop](docs/GUIDE.md#qela--c--c-interop).

## Tooling without the toolchain

```console
qela test tests/*.qela    # expectations live beside each test
qela fmt source.qela      # token-based, comment-preserving, idempotent
qela --lsp                # JSON-RPC over stdio
qela run source.qela      # compile, execute, show panic backtraces
qela doc std name         # fuzzy-search embedded API documentation
qela absorb package/      # embed your own package into the compiler
```

Add `-g` for DWARF line information and source-level GDB stepping. A shebang is
accepted, `qela -` compiles stdin, and `$flag` directives can make build options
part of the source. Read [The compiler as a tool](docs/GUIDE.md#21-the-compiler-as-a-tool)
for the complete workflow.

## Build Qela with Qela

The bootstrap starts with a C compiler and binutils on Linux. Python is used to
generate the embedded stdlib and test the LSP; the complete portability gate
also uses cross-C toolchains and QEMU for ARM64 and RISC-V 64:

```console
make          # build the small, throwaway C bootstrap compiler
make build    # stage0 → S1a → S2 → S3; verify S2 == S3 and run the gates
make install  # install S2 as /usr/local/bin/qela
```

The compiler to use from the tree is `build/bootstrap/s2`; the root `./qela`
exists only to start the bootstrap. The process and deliberately restricted
self-hosting subset are documented in [Bootstrap](docs/BOOTSTRAP.md).

## Repository map

| Path | Purpose |
|---|---|
| [`srcql/`](srcql/) | The production compiler, written in Qela |
| [`std/`](std/) | Embedded standard library and runtime |
| [`tests/`](tests/) | One executable specification per feature |
| [`examples/`](examples/) | FizzBuzz, a Lisp interpreter, Flappy Bird, and MiniOS |
| [`src/`](src/) | Frozen C bootstrap; never shipped |
| [`docs/`](docs/) | Guide, internals, assembly reference, status, and roadmap |

Start with the [Guide](docs/GUIDE.md), inspect a feature in [`tests/`](tests/),
or read the compiler that compiles itself in [`srcql/`](srcql/).

---

<p align="center">
  <img src="logo_square.png" alt="Qela logo" width="96"><br>
  <strong>Qela</strong><br>
  <sub>The most language per byte.</sub>
</p>
