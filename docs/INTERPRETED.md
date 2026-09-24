# `--interpreted`: self-embedding for runtime metaprogramming

Planning notes from a design conversation. Not implemented yet. Read
`docs/BOOTSTRAP.md` and `docs/STATUS.md` first for the compiler's current
shape; read `srcql/interp.qela`'s own comments for what the tree-walking
interpreter already does (`qela irun`).

## Goal

A `qela foo.qela --interpreted -o foo` build mode whose output binary can, at
its own runtime, compile and run arbitrary new Qela source it discovers
later — a config file, a plugin, anything. The motivating use case: a Qela
program reading a Qela config file and getting a real language for it, for
free, rather than inventing a config format.

This is metaprogramming (`eval`-style), not just "run this one fixed
program without a native compile" — that narrower thing already exists as
`qela irun`. The output of `--interpreted` needs the whole compiler
available to itself at runtime: lexer, parser, type checker, and something
that executes the result (interpreter and/or JIT).

## Constraint this has to respect

`qela` itself has a hard 1 MiB budget (see `docs/BOOTSTRAP.md`) and ships as one
freestanding, statically-linked, zero-dependency binary — confirmed via
`file`/`ldd` on the current build (`not a dynamic executable`, no section
header, no external deps). Currently: **501,488 bytes, 47.8% of budget.**

The project's existing rule — "no precompiled blobs, everything ships as
Qela source" (`std_blob.qela`, rebuilt by `tools/genblob.py`) — is about
`qela` itself never going stale relative to its own source. It does **not**
apply to files `--interpreted` produces; those aren't `qela`, and can embed
whatever they need. This was confirmed explicitly in the design discussion:
blobs are banned in `qela`, not in its output.

## Two approaches considered and rejected

**A. Embed a copy of the needed compiler subset as Qela source text inside
`qela` itself**, then have `--interpreted` builds recompile it (via the
normal frontend, same as `std_blob.qela` today) into every output binary.

Real numbers, measured this session (see "Size measurements" below): this
is *survivable* — even the maximal version (whole compiler minus
`lsp.qela` and the non-host backends, minified + gzip'd) only grows `qela`
to ~56% of budget. But it means `qela` permanently carries an extra
~40–90 KB blob of its own source, forever, for a feature most builds never
use. And it needs new machinery: a minifier, `$if (TARGET == ...)`
resolution ahead of time, and a trimmed top-level `parse()` variant that
skips codegen-prep passes (`regalloc`/`opt`/`bounds`) the interpreter
doesn't need.

**B. Mark a byte range inside `qela`'s own compiled image (asm labels
around the interpreter+frontend functions) and copy those bytes directly
into the output**, skipping recompilation entirely.

Rejected on inspection. `qela` is `-no-pie`/static — every call, string
reference, and global access inside that byte range is a fixed absolute
address computed for `qela`'s *own* full memory layout. Copying the bytes
elsewhere only works if the range is provably **closed** (nothing inside
it calls anything outside it) and pinned to the exact same base address in
every output — fragile, and the failure mode is silent corruption in
someone else's shipped binary, not a compile error caught at `qela`'s own
build/test time. Also arch-locked (can't cross-compile `--interpreted -t
arm64` from an x86_64 host, since there's no arm64 machine code to extract
from an x86_64 `qela`), and forecloses PIE/ASLR forever for anything
touched by the scheme.

## Chosen approach: self-copy from disk, not from inside `qela`

The actual point (surfaced late in the design conversation, and the right
answer): **`qela` does not need to contain a copy of itself at all.** At
`--interpreted` build time, `qela` is a running process — it can read its
*own currently-executing binary file* off disk and embed those bytes into
the output. `qela`'s own size never changes. Zero bytes added, ever, for
this feature.

### Resolving "where am I"

In order:

1. `readlink("/proc/self/exe")` — reliable on any normal Linux, no extra
   setup, works even if invoked via a relative path or through `$PATH`
   (unlike `argv[0]`, which is not trustworthy for this).
2. Fall back to the `$QELAPATH` environment variable if `/proc/self/exe` is
   unavailable (rare: unusual container/chroot setups without `/proc`
   mounted).
3. If neither resolves to a readable file: hard error with an actionable
   hint, e.g. `error: --interpreted needs its own binary; re-run with
   QELAPATH=$(which qela)`. Don't guess silently.

### What the output binary carries

- The raw (or compressed — see below) bytes of the `qela` binary that built
  it, as an embedded blob.
- A small stub that, when `eval`/config-loading is invoked at runtime:
  materializes those bytes via `memfd_create()` (anonymous, memory-backed
  fd, no disk write, no temp-file cleanup) and `fexecve()`s it — a real,
  complete, **unmodified** `qela` process, loaded by the kernel's own ELF
  loader. No custom loader, no address-layout surgery, no closedness
  analysis needed: this sidesteps every risk in approach B by construction.
- Communication with that child process is via argv / stdin / stdout /
  pipes / a tmpfile — e.g. `qela irun -` fed the config source over a pipe,
  its stdout captured. Standard subprocess IPC, nothing new to invent.

### The trade-off this approach accepts

The embedded compiler runs as a **separate process**, its own address
space. This is fine — good, even — for "load a config file, get data
back." It does **not** give eval'd code direct access to the host
program's own functions or live state (no shared memory, no direct calls
back into the parent) — that would need genuine in-process interpretation
(the model the interpreter-backed `repl` already uses: see
`docs/TASKS.md`'s 2026-08-08 repl entry) instead of this subprocess model.
Fork+exec per call also has real (sub-millisecond, but nonzero) overhead —
irrelevant for "read a config once at startup," possibly relevant for
"call eval() in a hot loop."

A frozen-in-time property worth naming explicitly, not a bug: a
`--interpreted` binary built today embeds *today's* `qela`. Upgrading the
`qela` on disk tomorrow doesn't change already-shipped binaries — good for
reproducibility, means compiler bugfixes need a rebuild to reach shipped
artifacts, same as any statically linked dependency.

## Size measurements (this session, real numbers not estimates)

Baseline: `qela` (S2) = 501,488 bytes, 47.8% of 1 MiB.

Self-copy approach — cost lands entirely on the **output file**, `qela`
itself is unaffected:

| what's embedded in the output | size |
|---|---|
| raw `qela` binary, uncompressed | 501,488 |
| + gzip -9 | 137,941 |
| + xz -9 | 111,420 |

For comparison, what approach A (recompile-from-source-blob-in-qela) would
have cost **`qela` itself** (kept here so a future session doesn't
re-derive it): whole compiler minus `lsp.qela`/`arm64_emit.qela`/
`riscv_emit.qela`, `$if (TARGET == "x86_64")` pre-resolved, comments and
whitespace stripped, then compressed — raw 570,741 → minified 402,838 →
+gzip-9 87,687 (`qela` would land at 589,175, 56.2%) → +xz-9 73,176
(574,664, 54.8%). Interpreter-only (no codegen/JIT, just eval) subset of
that: minified 212,293, +gzip-9 48,020 (`qela` at 549,508, 52.4%).

Measurement method for anyone re-deriving these: transitive `import` walk
from `srcql/interp.qela` restricted to `srcql/*.qela` gives the frontend
file set; a hand-written Python minifier (string/char-literal aware, strips
`//` and `/* */` comments and collapses whitespace outside literals) and a
`$if (TARGET ...)`-block resolver (brace-matched, evaluates the
`=="x86_64"`/`!="x86_64"` condition and keeps only the taken branch) were
used, then `gzip.compress(..., 9)` / `lzma.compress(..., preset=9)` as
compression proxies. A real self-hosted compressor (needed if approach A
is ever revisited) would land between the "minified only" and "gzip"
numbers — budget for that, don't assume gzip-level ratios from a from-
scratch LZSS.

## Open questions / next steps for implementation

1. **Read-self plumbing.** `sys_readlink` doesn't exist in `std/sys.qela`
   yet — add it (same pattern as the other raw-syscall wrappers there).
   `memfd_create` and `fexecve` likewise need wrappers.
2. **Where the stub lives.** The embedded-copy machinery (resolve path,
   embed blob, materialize + fexecve at runtime) is new code in
   `main.qela`'s `--interpreted` build path and a small runtime stub
   compiled into the output — scope this as its own module
   (`srcql/selfembed.qela`?) rather than growing `main.qela` further.
3. **Compression choice.** Given the numbers above, gzip-level compression
   on the embedded copy is worth it (501 KB → 138 KB) but needs a
   self-hosted decompressor in the *output* binary (not `qela` — this one
   *is* allowed to be sized freely, per the no-blobs-in-qela-only rule,
   but it still has to exist and be correct). Decide: ship uncompressed
   first (simpler, prove the mechanism end-to-end) and add compression as
   a follow-up once `memfd_create`+`fexecve` round-tripping is verified
   working.
4. **CLI surface.** What does the embedded `qela` get invoked with?
   Probably `irun -` (reads source on stdin) for "eval this snippet and
   give me the result" — decide the calling convention (stdin/stdout text
   protocol? exit code + stdout only? something structured?) before
   wiring up the stub.
5. **Whether to also support in-process interpretation as a separate,
   composable feature.** This document is about the subprocess/self-copy
   model specifically. If deep scripting (eval'd code calling back into
   the host program) is ever wanted, that's the `repl`'s in-process
   interpreter model applied to a shipped binary instead of a REPL session
   — a different, larger feature, out of scope here but worth linking once
   it exists.
6. **Test plan.** At minimum: a `--interpreted` build of a trivial program
   that reads a "config.qela" via the embedded-self mechanism and prints a
   value from it, run under `tools/bootstrap.sh` or a dedicated script,
   the same way the repl's interpolation test was added
   (`tools/bootstrap.sh`'s "interpolation and repl" step) as a permanent
   regression check.
