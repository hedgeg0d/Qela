# Runtime metaprogramming: per-block `interpreted`/`dynamic`, `eval`, and the global flags

Planning notes from a design conversation. Not implemented yet. Read
`docs/BOOTSTRAP.md` and `docs/STATUS.md` first for the compiler's current
shape; read `srcql/interp.qela`'s own comments for what the tree-walking
interpreter already does (`qela irun`).

This revises an earlier, narrower version of this document (a whole-program
`--interpreted` subprocess model). That model is kept below as one piece of
the design (the global flags), but the primary design is now finer-grained:
individual functions opt into being interpreted or JIT'd, ordinary AOT code
calls them exactly like any other function, and a small runtime API
(`parse_ast`/`run_ast`/`eval`) lets a normal compiled program reach for the
compiler explicitly when it wants to.

## Goal

1. Normal `qela foo.qela -o foo` stays exactly what it is today: full AOT,
   nothing embedded, no size or behavior change. This is not optional —
   it's the default for every program that doesn't ask for the feature.
2. A function can be marked, at the source level, as living outside the
   normal AOT world:

   ```qela
   fn interpreted patch_pricing(order Order) i64 { ... }
   fn dynamic     hot_path(x i64) i64 { ... }
   ```

   `interpreted` — the function's AST is what runs, walked by the tree
   interpreter, every call. `dynamic` — the function's AST gets compiled to
   real machine code (in RAM, at or before first call) and calls run at
   native speed after that. Both are called from ordinary AOT code with
   ordinary call syntax; the caller does not need to know or care which
   kind of function it's calling.

3. `qela` only embeds anything into the output if the program actually asks
   for this — at least one `interpreted`/`dynamic` function, or a call to
   `parse_ast`/`run_ast`/`eval` anywhere in the source. A program that
   doesn't use any of this compiles exactly as it does today, byte for
   byte.
4. A runtime API, available once the feature is triggered:

   ```qela
   fn parse_ast(src str) *Ast          // text -> AST, no execution
   fn run_ast(a *Ast) i64              // execute an AST (interpreted or JIT'd)
   fn eval(src str) i64                // parse_ast + run_ast, the common case
   ```

   `*Ast` is deliberately a separate step from `eval` (not folded away) so a
   program can inspect and rewrite the tree before running it — filter what
   it's willing to execute, splice in values, cache a parsed form and run it
   repeatedly with different bindings, etc. `eval` is sugar over the other
   two for the case that doesn't need any of that.
5. Two global build flags that skip the per-function annotation entirely:
   `--interpreted` treats every function as `interpreted`; a JIT
   equivalent (name below) treats every function as `dynamic`. Both output
   only the embedded compiler plus the program — no native code from the
   user's own functions baked in ahead of time.

## The security angle — this is the point, not an afterthought

Point 3 is a real security boundary, not just a size optimization: **a
function the source never marks `interpreted`/`dynamic`, and a program that
never calls `eval`, gets nothing embedded, no interpreter, no JIT, no
runtime code path that can execute anything but what was compiled in at
build time.** Ordinary AOT code is immune to whatever runs through `eval`
by construction, not by convention.

This matters concretely for the scenario that motivated bringing it up: a
program that lets untrusted text from any source
generate a patch and `eval`s it to self-heal after a crash is exactly the
kind of thing this feature is *for* — and exactly the kind of thing that
should never be able to reach code the author didn't intend to be
patchable. Proposed control, on top of point 3's default-frozen posture:

```qela
fn frozen never_patch_this(...) T { ... }
```

A `frozen` function is never affected by `--interpreted`/the JIT flag, even
if the whole build is compiled with one of them — those flags upgrade
*unmarked* functions to interpreted/dynamic, they do not touch `frozen`
ones. This gives a real dial: everything patchable by default except an
explicitly walled-off core (auth checks, the eval sandboxing logic itself,
anything that shouldn't be self-modifying no matter what flag someone
builds with). Whether `frozen` is a real third annotation or just "the
default, and the two global flags only upgrade `interpreted`/`dynamic`
functions that are *already* marked, never bare ones" is an open question
— see below.

## The architectural fork this design surfaces

The original version of this document (kept for its size data and the
rejected alternatives, both still valid) proposed a **subprocess** model:
`qela` embeds a copy of itself, and at runtime the output `fexecve`s that
copy as a *separate process*, talking to it over pipes.

That model is right for point 5 (the whole-program global flags) — if
*everything* is interpreted or JIT'd, the output can just *be* a
specialized `qela irun`/JIT invocation, no mixing with anything else.

It is **wrong** for points 2–4. A `dynamic hot_path()` called from ordinary
AOT code in a loop cannot be a fork+exec or even a persistent-worker+IPC
round trip per call — that's slower than the tree interpreter it's
supposed to be faster than, and it can't share pointers/structs with the
caller at all, which breaks the "call it like any other function" promise
point 2 makes. Per-function `interpreted`/`dynamic`, and `parse_ast`/
`run_ast`/`eval` as directly-callable functions, need the compiler's
machinery **in the same process**, linked in like any other code.

So there are two different mechanisms for two different parts of the
feature:

- **Points 2–4 (per-function, `eval` as a callable):** needs the frontend
  (+ interpreter, + codegen if any function is `dynamic`) compiled *into
  the same output binary* as ordinary linked-in native code. This is
  "Approach A" from the original design pass (recompile from source),
  not the subprocess trick.
- **Point 5 (global flags):** the subprocess/self-copy model still applies
  cleanly here, or can reuse whatever in-process machinery points 2–4 end
  up building — worth deciding once 2–4 exist, rather than building the
  subprocess path first and then finding it doesn't generalize.

### Unresolved: how does `qela` get the in-process compiler without a source blob inside itself?

This is the open problem for the next session, not a solved one. Two
candidates, neither fully worked out:

1. **`qela` reads `srcql/*.qela` off disk at build time** (like `qela .`
   does today, like `tools/bootstrap.sh` already requires for self-hosting)
   when it needs to compile the interpreter/frontend/codegen subset into
   someone's output. Simplest, reuses everything already built and tested.
   Cost: producing an `interpreted`/`dynamic`-using output requires the
   `srcql/` source tree to be present *at build time*, on the machine
   running `qela` — the *output* is still fully standalone/freestanding at
   *its own* runtime (the compiler subset is normal linked native code by
   then), but `qela` itself is no longer usable for this feature as a lone
   downloaded binary with no source tree nearby. Whether that's acceptable
   depends on how `qela` is meant to be distributed/used — worth deciding
   explicitly rather than assuming either way.
2. **Reuse the self-copy trick, but as a build-time codegen helper, not a
   runtime one.** `qela`, when it needs this, materializes a copy of
   itself via `/proc/self/exe` (exactly as before) and asks that copy to
   compile the needed subset (feeding it embedded *inline* source — which
   still has to come from somewhere, so this doesn't obviously avoid
   problem 1 either) to a relocatable object (`-c`, already supported),
   then links that object into the final output with its own ELF writer.
   The output ends up with no subprocess machinery at its own runtime
   either way. This does not yet have a clean answer for where the
   interp/frontend/codegen *source text* comes from without either a
   blob inside `qela` or a source tree on disk — it may turn out to
   collapse into option 1 with extra steps. Flagged here so the next
   session doesn't have to re-derive this dead end from scratch.

Whichever of these is chosen, the earlier "Approach A" cost data (below)
is the relevant sizing — but note it was computed as *cost to `qela`
itself*; if the resolution is "read `srcql/` off disk," `qela`'s own size
is untouched (0 bytes), and the cost question disappears entirely except
as "does the machine building this have the source tree."

## Call-site mechanics (new — not in the original pass)

For AOT code to call an `interpreted`/`dynamic` function with ordinary call
syntax, codegen needs a new call shape: instead of a direct `call` to a
fixed address, the call site goes through a small per-function trampoline
that:

- for `interpreted`: marshals the arguments and invokes the tree
  interpreter against that function's (embedded, pre-parsed/typed) AST,
  same as how the interpreter-backed `repl` invokes a function today
  (`ip_call` in `srcql/interp.qela`) — this part is not new, it already
  exists and works.
- for `dynamic`: on first call, hands the function's AST to the embedded
  codegen, `mmap`s a `PROT_READ|PROT_WRITE` region, writes the compiled
  code and fixups, `mprotect`s it `PROT_READ|PROT_EXEC`, and rewrites the
  trampoline (or a function pointer it indirects through) to jump straight
  there from then on. Every call after the first is a normal indirect call
  at native speed, in-process — no interpreter involvement, and critically
  no IPC. If this instead stayed as an out-of-process compile-and-fetch,
  `dynamic` would not deliver the speed it's named for.

Both are lazy by default — nothing gets parsed/interpreted/JIT'd until the
first call actually reaches it, so a program that has a `dynamic` function
it never calls in a given run pays nothing for it beyond whatever binary
size it added.

`*Ast` (for `parse_ast`/`run_ast`) should be an opaque handle, not a raw
`*Node`/`*Unit` — expose accessor/mutator functions (kind, children,
replace-a-node, etc.) rather than the internal struct layout, so user code
can't corrupt compiler-internal state by getting a field wrong. This is
its own real sub-effort (a stable, ergonomic AST API is more design work
than the embedding mechanism itself) — scope it separately once the
embedding question above is resolved.

## Naming

- `interpreted` (point 2) — settled, matches `qela irun`'s existing name
  for the same execution mode.
- `dynamic` (point 2, the JIT one) — usable, but consider `jit` instead:
  shorter, and "dynamic" already means other things in other languages
  (dynamic typing, dynamic linking) that this isn't. No strong objection
  to keeping `dynamic` if it reads better against `interpreted` as a pair.
- The global JIT flag (point 5) — recommend `--jit` over `--dynamic` for
  the same reason (unambiguous, matches whatever the per-function keyword
  ends up being, so the flag and the annotation share a name). Pair:
  `--interpreted` / `--jit`.
- `frozen` (security section) — placeholder, fine unless something better
  comes up.

## Kept from the original design pass

### Rejected alternatives (both still valid reasoning)

**Embed a source blob permanently inside `qela` itself.** Survivable by
size (below) but permanent cost for a feature most builds never touch, and
needs a minifier + `$if (TARGET==...)` pre-resolution + a trimmed
`parse()` that skips `regalloc`/`opt`/`bounds`. Superseded by "read
`srcql/` off disk at build time" as the simpler option with the same
zero-cost-to-`qela` property, pending the unresolved question above.

**Mark a byte range inside `qela`'s own compiled image and copy those
bytes directly (skip recompilation).** Rejected: `qela` is `-no-pie`/
static, so every internal call/data reference in that range is a fixed
absolute address computed for `qela`'s *own* full layout. Copying it
elsewhere only works if the range is provably closed and pinned to the
same base address everywhere — fragile, fails silently in someone else's
shipped binary rather than at `qela`'s own build/test time, arch-locked to
the host `qela` was built for, and forecloses PIE/ASLR permanently for
anything the scheme touches.

### The subprocess/self-copy model (still the right answer for point 5)

`/proc/self/exe` → `$QELAPATH` fallback → error with a `QELAPATH=$(which
qela)` hint if neither resolves. Materialize via `memfd_create()` (no disk
write) + `fexecve()`. Real, complete, unmodified `qela` process, loaded by
the kernel's own ELF loader — no custom loader, no address-layout surgery.
Trade-off: separate process, IPC only (fine for "load a config file, get
data back," wrong for tight in-program calls — which is exactly why points
2–4 need the different, in-process mechanism above). A build today embeds
*today's* `qela`; upgrading the installed compiler later doesn't change
already-shipped binaries — a feature (reproducibility), not a bug.

## Size measurements (real numbers, not estimates)

Baseline: `qela` (S2) = 501,488 bytes, 47.8% of the 1 MiB budget.

Self-copy (whole binary embedded in the *output*, cost does not touch
`qela` itself):

| what's embedded in the output | size |
|---|---|
| raw `qela` binary, uncompressed | 501,488 |
| + gzip -9 | 137,941 |
| + xz -9 | 111,420 |

What permanently embedding a source blob *inside `qela`* would have cost
(relevant only if the disk-read option turns out to be unacceptable and
this path gets revisited):

whole compiler minus `lsp.qela`/`arm64_emit.qela`/`riscv_emit.qela`,
`$if (TARGET == "x86_64")` pre-resolved, comments/whitespace stripped,
then compressed — raw 570,741 → minified 402,838 → +gzip-9 87,687 (`qela`
would land at 589,175, 56.2%) → +xz-9 73,176 (574,664, 54.8%).
Interpreter-only (no codegen/JIT) subset: minified 212,293, +gzip-9 48,020
(`qela` at 549,508, 52.4%).

Method, for anyone re-deriving these: transitive `import` walk from
`srcql/interp.qela` restricted to `srcql/*.qela` for the frontend file set;
a hand-written Python minifier (string/char-literal aware, strips `//` and
`/* */` comments, collapses whitespace outside literals) and a
`$if (TARGET ...)` block resolver (brace-matched, evaluates
`==`/`!= "x86_64"`, keeps only the taken branch); `gzip.compress(..., 9)` /
`lzma.compress(..., preset=9)` as compression proxies. A real self-hosted
compressor would land between "minified only" and "gzip" — budget for
that, don't assume gzip-level ratios from a from-scratch LZSS.

## Open questions / next steps for implementation

1. **Resolve the disk-read-vs-self-copy-as-codegen-helper question above**
   before writing any code — it decides whether points 2–4 need any new
   syscall plumbing at all (if "read `srcql/` off disk," they mostly
   don't) or need the full self-copy mechanism repurposed as a build-time
   tool (if not).
2. **Grammar.** `fn interpreted name(...)`/`fn dynamic name(...)` as
   modifiers parsed the same place `fn naked` already is (see
   `docs/BOOTSTRAP.md`'s grammar list) — small, contained parser change.
   Decide `frozen`'s status (real third keyword vs. "default posture,
   global flags only upgrade already-annotated functions") here too.
3. **Call-site trampolines in codegen.qela.** New call shape for
   `interpreted`/`dynamic` targets — real, nontrivial codegen work,
   separate from the embedding question. The `interpreted` half reuses
   `ip_call` as-is; the `dynamic` half needs the `mmap`(RW)→write→
   `mprotect`(RX) sequence and a self-patching trampoline or indirect call
   through a pointer filled in after first JIT.
4. **The `*Ast` API surface.** `parse_ast`/`run_ast`/`eval` plus accessor/
   mutator functions for an opaque `*Ast` handle — scope as its own
   design pass once 1–3 are settled; this is more work than the embedding
   mechanism.
5. **`sys_readlink`, `memfd_create`, `fexecve` wrappers** in
   `std/sys.qela` — needed regardless, for point 5 at minimum.
6. **W^X on hardened kernels.** `mprotect(RW→RX)` for the `dynamic` path
   can be restricted under some hardening policies (SELinux, some
   container runtimes) — needs a real error message, not a silent
   failure, when it's unavailable.
7. **Test plan.** At minimum: one program using `interpreted`, one using
   `dynamic`, one using bare `eval`, one using `--interpreted`, one using
   `--jit`/whatever it's named, each with a small correctness check —
   wired into `tools/bootstrap.sh` the way the repl's interpolation test
   was (`tools/bootstrap.sh`'s "interpolation and repl" step), as a
   permanent regression gate, not a one-off manual check.
