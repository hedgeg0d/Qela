# Where the project stands

Updated 2026-08-03. Read `BOOTSTRAP.md` first; it constrains everything below.

## Numbers

| | |
|---|---|
| stage0 (`src/*.c`, the throwaway bootstrap) | 46 696 B |
| **S2 — the shipped compiler, Qela compiled by itself** | **159 368 B** |
| S2 under xz -9 (proxy for upx --lzma) | 38 700 B, ~3.7% of the 1 MiB budget |
| stage1 sources | 6 043 lines of Qela |
| Emitted code vs `gcc -Os` on `bench/` | **278%**, or **238%** without bounds checks (M4 gate wants ≤150%) |

Everything is verified by `tools/bootstrap.sh`: S2 == S3 byte-for-byte, the
29-test corpus under S2, the embedded stdlib resolving outside the source tree,
coroutines, channels, the collector, and `run`/`fmt`.

## Done

**Language.** i8–u64, bool, int/uint/usize; pointers, arrays, slices, `str`;
structs with literals and forward declarations; enums with payloads and
exhaustive `match`; `defer`; the full operator set with compound assignment;
`as`, `sizeof`, character literals, `true`/`false`; locals zeroed at their
declaration; modules via `import`; `comptime` blocks; generics by
monomorphization; `syscall`; `main(argc, argv)`; bounds checks.

**Concurrency.** `spawn f(...)`, `coro_yield`, `coro_run_all` on separate
stacks; buffered channels with `ch <- v` and `<-ch`, blocking through the
scheduler, with deadlock detection rather than a spin.

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

### 1. Emitted code size (M4)

278% of `gcc -Os`, down from 355%. Register allocation is **done**:
`srcql/regalloc.qela` promotes scalar locals whose address is never taken into
rbx and r12–r15 for their whole live range. Ranges come from a linear walk of
the tree, loops stretch every range that touches them, and registers go to the
most-referenced candidate first, weighted by loop depth. A function saves only
the registers it uses, into reserved slots at the top of its own frame.

Instruction selection now covers: direct locals as register or memory operands,
commutative operand swapping, in-place updates (`v op= e` and `v = v op e`),
immediate stores through a computed address, `lea` for indexing with a promoted
index register, comparing a load in place, and short-form branches — the unit is
emitted repeatedly, each pass shortening the branches that turned out to be
within a byte, until nothing more shrinks.

What is left, in order of what it would buy:

- **Bounds checks are ~25% of the figure** and C does not do them: `sieve` is
  304 bytes with checks and 232 without. Each check is `cmp` plus a `jae` to the
  panic stub at the end of the code, which is always too far for a short branch.
  A per-function trampoline would make most of those two bytes instead of six.
- **Loop-invariant addresses.** `lea flags(%rip),%rax` is re-emitted on every
  access to a global array inside a loop. Folding the global's absolute address
  into the addressing mode (`movb $1, disp32(,%r12,1)`) removes both the `lea`
  and the add — the data fixup has to learn to patch an absolute displacement.
- **Common subexpressions.** Nothing is reused between statements.

Reaching 150% with checks on is unlikely; without them it is in range. Decide
deliberately which number the gate should measure.

### 2. LSP in the same binary

Needs a JSON reader and writer from scratch, framed stdio, a compiler that
survives errors instead of calling `error_at` and exiting, and a position
index for hover and go-to-definition. `Node.pos` and `Node.ty` already carry
what is needed. Its own session.

### 3. ARM64 backend

The most expensive item, and the only one that would push the budget. Worth
doing only after 1b, so the second backend inherits a real allocator.

### 4. Smaller

- `tools/torture.py` still generates only straight-line scalar expressions —
  no calls, branches, structs or enums. **Every bug found in the last several
  sessions would have slipped past it**, including the two the register
  allocator would have been most likely to introduce. Still the cheapest way to
  buy confidence.
- Unbuffered channels (true rendezvous); channels currently carry `i64`, with
  pointers passed by cast.
- `genblob.py --min` to strip comments and indentation from the embedded
  library. Measured: saves 416 bytes packed, and costs `--dump-std` its
  readability. Not worth it at 3.5% of budget.

## Rules that keep holding

1. Nothing goes into `src/*.c` except a bug fix backed by a test. A feature
   that is not in stage1 does not exist, because stage0 never ships.
2. No external toolchain in the build path — no assembler, no linker, no
   objcopy. We write the ELF ourselves.
3. Emission follows source order everywhere, or `S2 == S3` breaks.
4. A module counts as done when it compiles *and runs* on a minimal example
   under S2, not when its text is written.
