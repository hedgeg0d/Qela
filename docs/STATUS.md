# Where the project stands

Updated 2026-08-03. Read `BOOTSTRAP.md` first; it constrains everything below.

## Numbers

| | |
|---|---|
| stage0 (`src/*.c`, the throwaway bootstrap) | 46 696 B |
| **S2 — the shipped compiler, Qela compiled by itself** | **161 832 B** |
| S2 under xz -9 (proxy for upx --lzma) | 38 676 B, ~3.7% of the 1 MiB budget |
| stage1 sources | 6 653 lines of Qela |
| Emitted code vs `gcc -Os` on `bench/` | **226%**, or **193%** without bounds checks (M4 gate wants ≤150%) |

Everything is verified by `tools/bootstrap.sh`: S2 == S3 byte-for-byte, the
31-test corpus under S2, the embedded stdlib resolving outside the source tree,
coroutines, channels, the collector, and `run`/`fmt`.

## Done

**Language.** i8–u64, bool, int/uint/usize; pointers, arrays, slices, `str`;
structs with literals and forward declarations; enums with payloads and
exhaustive `match`; parameterized types `struct Pair(T)` and `enum Opt(T)`,
instantiated on use and cached so identical arguments name the same type;
`defer`; the full operator set with compound assignment;
`as`, `sizeof`, character literals, `true`/`false`; locals zeroed at their
declaration; modules via `import`; `comptime` blocks; generics by
monomorphization, with the type argument inferred from the value arguments
when it is not written out; `syscall`; `main(argc, argv)`; bounds checks.

**Concurrency.** `spawn f(...)`, `coro_yield`, `coro_run_all` on separate
stacks; `Chan(T)`, buffered channels of any element type. `ch <- v` and `<-ch`
infer that type, block through the scheduler, and report a deadlock rather than
spinning when nothing can run.

**Memory.** Arena by default; `std/gc.qela` is a conservative mark-sweep
collector rooted in the callee-saved registers, the stack and the data segment.

**Tooling.** Diagnostics with a caret; own ELF writer; DWARF line info and a
symbol table behind `-g` (gdb steps through `.qela` and names frames);
`qela run`; `qela fmt`, which formats over the token stream so comments
survive and which is idempotent; `--dump-std` for the embedded library;
`tools/torture.py` for randomized differential testing.

**Self-hosting.** stage0 is frozen and never ships. The whole compiler lives in
`srcql/` and `std/`, written in Qela, and the standard library is carried
inside the binary — a program importing `std/io.qela` compiles in an empty
directory with nothing but the compiler present.

## Not done

Item 1 is mostly done and kept here for what remains of it; 2 and 3 have not
been started.

### 1. Emitted code size (M4)

226% of `gcc -Os`, down from 355%; 193% with bounds checks off, which is the
number comparable to what gcc emits. `fib` is already at 153%.

Done, in `srcql/regalloc.qela` and `srcql/codegen.qela`:

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

What is left, in order of what it would buy:

- **Redundant bounds checks.** `while (i < N) { a[i] ... }` emits a compare the
  loop condition already made. Eliminating it needs to prove `0 <= i < N` at the
  access. The upper half is the loop condition; the lower half needs a
  whole-function pass showing every write to `i` keeps it non-negative.
  **Careful:** deriving that from `a + b` with both non-negative assumes the
  addition does not wrap, and Qela defines wrapping. An unsound elision here
  corrupts memory rather than producing a wrong number, so it needs either an
  overflow argument or a narrower rule.
- **Loop-invariant addresses.** The address of a global array is recomputed on
  every access inside a loop.
- **Common subexpressions.** Nothing is reused between statements.
- **Expression temporaries** still go through `push`/`pop`. That is already the
  smallest encoding; replacing it with registers costs bytes, so it is a speed
  optimization, not a size one.

### 2. LSP in the same binary

Needs a JSON reader and writer from scratch, framed stdio, a compiler that
survives errors instead of calling `error_at` and exiting, and a position
index for hover and go-to-definition. `Node.pos` and `Node.ty` already carry
what is needed. Its own session.

### 3. ARM64 backend

The most expensive item, and the only one that would push the budget. Worth
doing only once the x86 backend has stopped moving, so the second one inherits
a settled shape rather than a moving target.

### 4. Smaller

- `tools/torture.py` still generates only straight-line scalar expressions —
  no calls, branches, structs or enums. **Every bug found in the last several
  sessions would have slipped past it**, including the two the register
  allocator would have been most likely to introduce. Still the cheapest way to
  buy confidence.
- Unbuffered channels (true rendezvous).
- Parameterized types take at most two parameters, and there is no way to
  constrain one. Both limits are in `srcql/generic.qela`.
- `genblob.py --min` to strip comments and indentation from the embedded
  library. Measured: saves 416 bytes packed, and costs `--dump-std` its
  readability. Not worth it at 3.7% of budget.

## Rules that keep holding

1. Nothing goes into `src/*.c` except a bug fix backed by a test. A feature
   that is not in stage1 does not exist, because stage0 never ships.
2. No external toolchain in the build path — no assembler, no linker, no
   objcopy. We write the ELF ourselves.
3. Emission follows source order everywhere, or `S2 == S3` breaks.
4. A module counts as done when it compiles *and runs* on a minimal example
   under S2, not when its text is written.
