# Where the project stands

Updated 2026-08-03. Read `BOOTSTRAP.md` first; it constrains everything below.

## Numbers

| | |
|---|---|
| stage0 (`src/*.c`, the throwaway bootstrap) | 46 696 B |
| **S2 — the shipped compiler, Qela compiled by itself** | **156 600 B** |
| S2 under xz -9 (proxy for upx --lzma) | 36 236 B, ~3.5% of the 1 MiB budget |
| stage1 sources | 6 233 lines of Qela |
| Emitted code vs `gcc -Os` on `bench/` | **355%** (M4 gate wants ≤150%) |

Everything is verified by `tools/bootstrap.sh`: S2 == S3 byte-for-byte, the
28-test corpus under S2, the embedded stdlib resolving outside the source tree,
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

### 1. Register allocation — the one real gap (M4)

Emitted code sits at 355% of `gcc -Os`. Instruction selection is already done
(direct locals, immediate and memory operands, fused compare-and-branch), so
what remains is register allocation. Two halves, worth splitting:

**1a. Register stack for expression temporaries.** Every intermediate value
currently goes through `push`/`pop`. Keeping the top few in scratch registers
(rcx, rsi, r8, r9) and spilling only past that depth needs no liveness
analysis and is local to `gen_expr` in `srcql/codegen.qela`. One session,
moderate risk, gets part of the way.

**1b. Promoting locals to registers.** mem2reg plus liveness plus linear scan,
with callee-saved registers for values crossing calls. This is the expensive
half and the one that would actually reach 150%. A separate session.

Note that ~40% of the current figure is bounds checking, which C does not do:
`sieve` is 400 bytes with checks and 236 without. Either measure with
`--no-bounds-checks` or raise the gate, but decide deliberately.

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
  sessions would have slipped past it.** This is the cheapest way to buy
  confidence and should come before any allocator work.
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
