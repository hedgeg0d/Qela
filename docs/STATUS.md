# Where the project stands

Updated 2026-08-03. Read `BOOTSTRAP.md` first; it constrains everything below.

## Numbers

| | |
|---|---|
| stage0 (`src/*.c`, the throwaway bootstrap) | 46 696 B |
| **S2 — the shipped compiler, Qela compiled by itself** | **222 288 B** |
| S2 under `upx --lzma` (measured 2026-08-03) | 51 388 B, ~4.9% of the 1 MiB budget |
| S2 under xz -9 (proxy for upx) | 53 924 B |
| stage1 sources | 9 100 lines of Qela |
| Emitted code vs `gcc -Os` on `bench/` | **231%**, or **193%** without bounds checks (M4 gate wants ≤150%) |

Everything is verified by `tools/bootstrap.sh`: S2 == S3 byte-for-byte, the
56-test corpus under S2, the embedded stdlib resolving outside the source tree,
coroutines, channels, the collector, `run`/`fmt`, stdin compilation, the panic
backtrace, and a scripted language server conversation.

## Done

**Language.** i8–u64, bool, int/uint/usize; pointers, arrays, slices, `str`;
structs with literals and forward declarations; enums with payloads and
exhaustive `match`; parameterized types `struct Pair(T)` and `enum Opt(T)`,
instantiated on use and cached so identical arguments name the same type;
`defer`; the full operator set with compound assignment;
`as`, `sizeof`, character literals, `true`/`false`; locals zeroed at their
declaration; modules via `import`; `comptime` blocks; generics by
monomorphization, with the type argument inferred from the value arguments

**String interpolation** (`"n = ${n}"`). The lexer splits the string into
literal parts and `$` `{` expr `}` runs (strings, chars and comments inside
the expression are brace-counted correctly, nested interpolation included);
the parser collects an `ND_INTERP` and `type.qela` rewrites it into a chain
of by-value `fmt_*` calls on a fresh `Buf`, chosen per part type: strings as
themselves, integers as decimal, pointers as hex. The chain allocates its
own buffer, so two interpolated strings alive at once never alias. The
helpers live in `std/fmt.qela` and are auto-imported at the next top-level
splice point, so no `import` is needed; `tools/genblob.py` now rejects a
`$`+`{` in std sources, which would break the blob literal itself. Global
initializers and assert messages stay literal-only. Formatter round-trips
the split string verbatim from the token texts, idempotently.

**`for x in a`**. Iterates an array, slice or string by value. The desugar
in `parse.qela` builds the exact `for (i = 0; i < a.len; i += 1)` shape with
a hidden index (named with a `$`, which no identifier can contain); on a
fixed array `a.len` folds to a literal during typing, so the bounds elision
drops the check — the most natural loop is also the smallest. The
collection is evaluated per iteration, like the range form's bound.

**`qela repl`**. Each line is compiled in a forked child as
`write_str(STDOUT, "${line}")`, so integer, string and pointer expressions
all render through the interpolation machinery; the child exits after one
line, so a compile's arena leak never accumulates and a compile error costs
only the child. Stateless: every line is a fresh program. `read_line` in
`std/io.qela`, `sys_isatty` in `std/sys.qela` (prompts only on a terminal).

**Import path normalization.** `dir/../std/x.qela` normalizes to
`std/x.qela`, so the auto-import of `std/fmt.qela` dedups against the
compiler's own `../std/...` imports in the file table.

**`std/rand.qela`.** Deterministic xorshift64*: `rand_init(seed)`,
`rand_u64()`, `rand_range(lo, hi)`. The default seed is fixed, so an
unseeded program is reproducible; the top bit is masked so `%` in
`rand_range` never sees a negative operand. The repl imports it, so
`rand_range(1, 7)` is a one-liner there.

**`*volatile T`.** A pointer whose loads and stores may not be elided or
reordered — MMIO registers, DMA buffers. The qualifier is part of the
type (`same_type` distinguishes it), so dropping it requires an explicit
cast; codegen always emits the access, never folds it.

**`fn naked`.** A function with no prologue, no epilogue and no implicit
ret: the body is bare bytes, so ISR entries and syscall stubs end in their
own `iretq`/`ret` from `asm`. The contract is enforced at parse time — no
parameters, locals, returns, defers or calls (only `asm` and `syscall`).
An `asm` operand may be any comptime constant — `let OPCODE = 0x48;
asm(OPCODE, ...)` or `asm(0x48 | 0x80)` — folded at type time, so opcodes
have names without a single new keyword.

**Expression macros.** `macro sq(x) = x * x;` — the body is parsed once
with the parameters as placeholder locals, and every call clones it with
the argument trees substituted. Substitution is trees, not text: no
parentheses around arguments, no comma problems, and the expansion goes
through the ordinary type check and bounds checks. A call clones the
argument per use, so an argument evaluates as many times as the body names
it (the classic macro contract). Macros resolve at parse time, so they must
be defined before use; up to six parameters, expression bodies only. No new
node kinds: the expansion is pure parse-time.
when it is not written out; `syscall`; `asm(byte, byte, ...)` for raw machine
code with the result convention that whatever's left in `RAX` is the
expression's value, same as `syscall`; `assert(cond, "msg")` and
`panic("msg")`, stage1-only builtins whose message must be a string literal
(one panic stub per distinct message, deduplicated); more than 6 function
parameters, the 7th and later spilled to the stack per SysV (one-word
parameters only -- a `str`/8-16-byte struct that would overflow the register
budget is still rejected); `main(argc, argv)`; bounds checks that now write a
real `index out of bounds` message instead of bytes from the start of rodata.

**Concurrency.** `spawn f(...)`, `coro_yield`, `coro_run_all` on separate
stacks; `Chan(T)`, buffered channels of any element type, and rendezvous at
`chan_init(&ch, 0)` where the sender hands the value across directly. `ch <- v`
and `<-ch` infer that type. Waiting is polling over the scheduler. A deadlock
is when every coroutine that could run is parked inside a wait and no channel
has changed since — `chan_nblocked` counts the parked, `chan_epoch` advances on
every channel state change including rendezvous handoffs, and both staying
still long enough for everyone to have had a turn is reported rather than spun
on. The corpus now covers coroutines, channels, the collector, deadlock
reporting and a busy-consumer regression under S2.

**Memory.** Arena by default; `std/gc.qela` is a conservative mark-sweep
collector rooted in the callee-saved registers, the stack and the data segment.

**Tooling.** Diagnostics with a caret; own ELF writer; DWARF line info and a
symbol table behind `-g` (gdb steps through `.qela` and names frames);
`qela run`, which also compiles a program from stdin as `qela run -` and gives
every panic a **backtrace**: a per-function table in rodata and a raw-bytes
walker at the end of the image print `in <name>` for the panicking function
and every caller (the trampoline that reaches the panic stub is a call, so its
return address names the frame; the walk stops at main because the startup
header zeroes rbp). Plain compiles take `--backtrace` too, but default to the
deterministic, flag-free output the bootstrap gate compares; **`qela .` builds
a directory as one project**: every `.qela` file in it is merged into a single
program (entry `main.qela`, or the sole file, merged last; the rest in sorted
order), so functions call across files without imports, while imports inside
the files still work and one already merged by path is skipped — the compiler
is its own build system; `qela fmt`, which formats over the token stream so
comments survive and which is idempotent; `--dump-std` for the embedded
library; a shebang first line (`#!...`) is skipped by the lexer, so scripts
run as `#!/usr/bin/env qela run`; `tools/torture.py` for randomized
differential testing. **`qela --lsp`** is a
language server in the same binary: JSON-RPC over framed stdio, hand-written
JSON, full-document sync, diagnostics, hover with types and signatures, and
go-to-definition for locals, globals, functions and fields. A compile runs in
a forked child, so the compiler dying on its first error takes the session
with it — the child, not the server. `tools/lsp-test.py` scripts a full
conversation against it.

**Self-hosting.** stage0 is frozen and never ships. The whole compiler lives in
`srcql/` and `std/`, written in Qela, and the standard library is carried
inside the binary — a program importing `std/io.qela` compiles in an empty
directory with nothing but the compiler present.

**`qela test`** turns the compiler into a test runner. A file (or a whole
directory project, merged like `qela .`) declares its expectations in leading
comments — `// expect-exit: N`, one `// expect-out:` line per expected output
line, compared in order, and `// expect-compile-error` for tests the compiler
must reject. The compiler compiles, runs the binary with any remaining
arguments (stdout and stderr captured through a temp file), compares, and
prints `ok name` / `FAIL name: detail`, exiting with the number of failures.
The corpus files run unchanged: `qela test tests/assertfail.qela`. The lisp
example self-tests through it: `qela test . tests.lisp`, where the checks are
written in lisp itself. `examples/lisp/tests.lisp` carries ~60 checks; the
interpreter gained `and`/`or` (short-circuiting) and `equal?`/`string=?`
builtins to support them.

## Not done

Item 1 is mostly done and kept here for what remains of it; 2 has not been
started.

### 1. Emitted code size (M4)

231% of `gcc -Os`, 193% with bounds checks off, which is the number comparable
to what gcc emits. `fib` sits at 153%. The bounds-check panic message
(`index out of bounds`) costs +21 bytes of rodata in every binary that
actually has a bounds check — a program without arrays carries no panic
machinery at all, so `fib` and `loop` are byte-identical to before the
message existed.

Done, in `srcql/regalloc.qela`, `srcql/codegen.qela` and `srcql/bounds.qela`:

- **Register promotion.** Scalar locals whose address is never taken live in a
  register for their whole live range. Ranges come from a linear walk of the
  tree; loops stretch every range that touches them, `defer` stretches to the
  end of the function. Registers go to the most-referenced candidate first,
  weighted by loop depth.
- **Leaf functions cost nothing to enter.** A function that makes no calls takes
  r10, r11 and whichever of r8/r9 its own parameters do not occupy, so it saves
  and restores nothing; with no frame slots left it also drops `push rbp` and
  `leave` entirely.
- **Instruction selection.** Direct locals as register or memory operands,
  commutative operand swapping, in-place updates including whole accumulator
  chains (`s = s + a - b` becomes two updates on the register), evaluation
  straight into a destination register, immediate stores through a computed
  address, `lea` for indexing with a promoted index, constant indexes folded and
  checked at compile time, comparing a load in place, masks as zero-extending
  moves, accumulator short forms.
- **Short branches.** The unit is emitted repeatedly, each pass shortening the
  branches that turned out to be within a byte. Bounds checks jump to a
  trampoline at the end of their own function so they relax too.
- **Absolute addressing.** Globals and string literals are `mov reg, imm32`
  rather than a RIP-relative `lea`; the image lives below 4 GiB.
- **Redundant bounds checks.** `srcql/bounds.qela` drops the check on an array
  access the loop already proves in bounds: the exact shape `for (i = c; i < K;
  i += 1) { a[i] }` and `i = c; while (i < K) { ... a[i] ...; i += 1; }`, with
  `c >= 0`, `K` a literal equal to the array's length, no other write to `i`
  and its address never taken. The lower half is induction: the increment only
  runs while `i < K <= 2^63-1`, so `i + 1` cannot wrap. Tests
  `tests/boundsloop.qela` and `tests/boundsoob.qela` pin the soundness both
  ways. `sieve` went 317% -> 297%.

Both closed off, measured rather than assumed (see above):

- **Loop-invariant addresses.** Built, tested, bootstrap-green, then reverted:
  net negative. Absolute addressing already makes a global's base address a
  single 5-byte `mov`, and any new pass module costs ~2.4 KB compiled into S2
  regardless of hit rate here, which swallowed the whole local saving.
  Self-compiled size went *up*, 185 800 -> 188 344 B.
- **Common subexpressions.** Surveyed before building: ~12 same-line repeated
  subexpressions across ~4300 lines of stage1, worth maybe 40-50 bytes total.
  Same fixed-cost wall as above; not built.
- **Expression temporaries** still go through `push`/`pop`. That is already the
  smallest encoding; replacing it with registers costs bytes, so it is a speed
  optimization, not a size one.

x86 codegen has stopped moving on the remaining levers. Next size lever is
ARM64 (below), or accept the ratio and spend budget on wow/byte elsewhere.

### 2. ARM64 backend

The most expensive item, and the only one that would push the budget. Worth
doing only once the x86 backend has stopped moving, so the second one inherits
a settled shape rather than a moving target.

### 3. Smaller

- `tools/torture.py` now generates full programs — calls with up to four
  parameters and recursion, `if`/`else`, bounded `while` loops, 16/24/32-byte
  structs passed and returned by value and by reference, fixed-size arrays,
  enums with payloads and exhaustive `match` — and evaluates them in a Python
  model that mirrors x86 semantics exactly. Clean across 2 500 generated
  programs under both stage0 and S2.
- Parameterized types take at most two parameters, and there is no way to
  constrain one. Both limits are in `srcql/generic.qela`.
- `genblob.py --min` to strip comments and indentation from the embedded
  library. Measured: saves 416 bytes packed, and costs `--dump-std` its
  readability. Not worth it at 4.1% of budget.

## Rules that keep holding

1. Nothing goes into `src/*.c` except a bug fix backed by a test. A feature
   that is not in stage1 does not exist, because stage0 never ships.
2. No external toolchain in the build path — no assembler, no linker, no
   objcopy. We write the ELF ourselves.
3. Emission follows source order everywhere, or `S2 == S3` breaks.
4. A module counts as done when it compiles *and runs* on a minimal example
   under S2, not when its text is written.
