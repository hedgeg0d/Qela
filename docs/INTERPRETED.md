# Runtime metaprogramming: per-block `interpreted`/`dynamic`, `eval`, and the global flags

Started as planning notes from a design conversation; a real first slice is
now implemented (see "What's implemented" below) — `eval()`, and
`interpreted`/`dynamic` functions callable with ordinary call syntax from
AOT code, both over the same ABI subprocess. Read `docs/BOOTSTRAP.md` and
`docs/STATUS.md` first for the compiler's current shape; read
`srcql/interp.qela`'s own comments for what the tree-walking interpreter
already does (`qela irun`).

This revises an earlier, narrower version of this document (a whole-program
`--interpreted` subprocess model). The primary design is now finer-grained:
individual functions opt into being interpreted or JIT'd, ordinary AOT code
calls them exactly like any other function, and a small runtime API
(`parse_ast`/`run_ast`/`eval`) lets a normal compiled program reach for the
compiler explicitly when it wants to. The mechanism underneath all of it,
resolved after a few false starts (see "Resolved" below), turned out to be
the *same* subprocess/self-copy model the whole-program flags always used
— per-function granularity comes from what gets said over the pipe, not
from a second, in-process mechanism.

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
builds with). Resolved: `frozen` is a real fourth modifier, not a default posture. An
unmarked function is not automatically frozen — it's just ordinary AOT
code that the (not yet built) `--interpreted`/`--jit` global flags are free
to upgrade. `frozen` is the explicit opt-out from that upgrade, the same
way `eval var`/`eval fn` (below) is an explicit opt-in to ABI visibility —
both are grep-able properties of the source rather than an implicit
default, matching how this document already argued for `eval`'s allowlist.

## Resolved: one mechanism for everything, subprocess plus an ABI

Earlier passes at this document treated "points 2–4 need in-process
linking" and "point 5 is a subprocess" as two different mechanisms, and
got stuck on where the in-process compiler's source would come from
without a permanent blob in `qela` or a hard dependency on the `srcql/`
tree being on disk. That fork is resolved: **everything uses the
subprocess/self-copy model from point 5. There is no in-process compiler.**
What changes between `interpreted`/`dynamic` is *how* the call site talks
to that subprocess, not whether one exists.

The key realization, specifically for `dynamic`: the ABI does not have to
be crossed on every call. It is crossed **once**, to compile — the child
process compiles the function's AST and hands back *machine code bytes*
(or a relocatable object, reusing the existing `-c` output path), and the
*host* `mmap`s that `PROT_READ|WRITE`, copies the bytes in, `mprotect`s it
`PROT_READ|EXEC`, and calls it directly, in its own process, from then on.
The subprocess's job is "compile this and hand me bytes," not "run this
every time" — so `dynamic` gets genuine native speed after the first call,
same as the in-process design would have given, without needing the
compiler linked into the output at all.

`interpreted` does cross the ABI on every call — and that is fine, not a
compromise: choosing `interpreted` over `dynamic` is already choosing to
pay a speed cost for staying inspectable/patchable, so the added IPC cost
on top of that is a difference of degree, not a broken promise. `eval`/
`run_ast` on an AST that isn't a compile-once function work the same way,
per call, for whichever the caller keeps invoking it.

This also answers the earlier "where does the source come from" problem
by making it not apply: `qela` never needs to link its own frontend into
someone else's binary. It only ever needs to *run itself*, exactly as
built and shipped — the exact same self-copy mechanism as point 5, used
uniformly, just with two different conversations happening over the pipe
("interpret this and give me a result" vs. "compile this once and give me
bytes").

### The foreign-globals/foreign-calls ABI

**Done** (2026-08-08) — `eval fn` functions are callable, and `eval var`
globals are readable *and* writable, from `eval`'d and `interpreted`/
`dynamic` code today, over the same ABI. See "What's implemented" below
for exactly how, including a genuinely subtle double-write bug the `eval
var` set half exposed (and the fix). The rest of this subsection is kept
as originally designed — the shape it describes is what got built.

For `interpreted` code (or `eval`'d code generally) to read/write the
host's own state, or call the host's own functions, rather than only
operating on values passed in and returned — because otherwise this
degrades from "a real language for configs/patches" to "a pure function
calculator" — the ABI needs two more message shapes, in both directions:

```qela
fn eval_get(name str) i64        // child asks host: current value of a shared global
fn eval_set(name str, v i64)     // child asks host: write a shared global
// and the same shape for calling a host function by name with marshalled args
```

`srcql/interp.qela` already has exactly this kind of dispatch, for a
different purpose: `ip_intrinsic` decides whether a call is one the
interpreter must special-case (a coroutine primitive, an atomic) versus an
ordinary interpreted call. The same pattern extends here — a global or a
call that isn't part of the AST being run locally in the child resolves to
"ask the host over the ABI" instead of "not found."

**This needs an explicit, opt-in surface, not "everything visible."**
Point 3's security posture (nothing patchable unless marked) has to extend
to *state*, not just to *code* — a `frozen` core that happens to share its
globals unrestricted with `eval`'d code is not actually frozen. Proposed:
something in the shape of `extern`, but declaring the *export* side rather
than the import side —

```qela
eval var current_price i64;         // eval-visible global, get/settable over the ABI
eval fn apply_discount(pct i64);    // eval-visible function, callable over the ABI
```

— an explicit allowlist, checked at compile time, so "what can an eval'd
patch touch" is a `grep`-able property of the source, the same way
`frozen` makes "what can never be touched" one.

## Call-site mechanics

For AOT code to call an `interpreted`/`dynamic` function with ordinary
call syntax, codegen needs a new call shape: instead of a direct `call` to
a fixed address, the call site goes through a small per-function
trampoline that talks to the (lazily spawned, then kept alive for the
process's lifetime — not re-forked per call) self-copy subprocess:

- for `interpreted`: marshal the arguments over the ABI, ask the child to
  run that function's AST against them, marshal the result back. Every
  call round-trips.
- for `dynamic`: on first call, ask the child to compile the function and
  send back code bytes; `mmap`(RW) → copy → `mprotect`(RX) locally; rewrite
  the trampoline (or a function pointer it indirects through) to jump
  straight there. Every call after the first is a normal local indirect
  call — no ABI, no child involvement.

Both lazy by default: nothing is parsed, subprocess-spawned, interpreted,
or JIT'd until the first call actually reaches it. A program with a
`dynamic` function it never calls in a given run pays only its own binary
size, nothing at runtime.

`*Ast` (for `parse_ast`/`run_ast`) should be an opaque handle, not a raw
`*Node`/`*Unit` — expose accessor/mutator functions (kind, children,
replace-a-node, etc.) rather than the internal struct layout, so user code
can't corrupt compiler-internal state by getting a field wrong. Given the
subprocess model above, "run" an `*Ast` obtained via `parse_ast` most
likely means "hand it to the child over the same ABI," which may mean
`*Ast` itself is a handle into the *child's* memory (an opaque id/token),
not a local pointer at all — resolve this once the ABI's message shapes
are drafted (see open questions). This is its own real sub-effort — a
stable, ergonomic AST API is more design work than the ABI plumbing
itself.

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
- All of the above are now implemented as named: `interpreted`/`dynamic`/
  `frozen` per-function, `--interpreted`/`--jit` global flags.

## Kept from the original design pass

### Rejected alternatives (both still valid reasoning)

**Embed a source blob permanently inside `qela` itself.** Survivable by
size (below) but permanent cost for a feature most builds never touch, and
needs a minifier + `$if (TARGET==...)` pre-resolution + a trimmed
`parse()` that skips `regalloc`/`opt`/`bounds`. Made moot entirely by the
resolved design above — `qela` never needs to link its own frontend into
someone else's binary, so there's no in-process compiler to source in the
first place.

**Mark a byte range inside `qela`'s own compiled image and copy those
bytes directly (skip recompilation).** Rejected: `qela` is `-no-pie`/
static, so every internal call/data reference in that range is a fixed
absolute address computed for `qela`'s *own* full layout. Copying it
elsewhere only works if the range is provably closed and pinned to the
same base address everywhere — fragile, fails silently in someone else's
shipped binary rather than at `qela`'s own build/test time, arch-locked to
the host `qela` was built for, and forecloses PIE/ASLR permanently for
anything the scheme touches.

### The subprocess/self-copy model (now the answer for everything, see "Resolved" above)

`/proc/self/exe` → `$QELAPATH` fallback → error with a `QELAPATH=$(which
qela)` hint if neither resolves. Materialize via `memfd_create()` (no disk
write) + `fexecve()`. Real, complete, unmodified `qela` process, loaded by
the kernel's own ELF loader — no custom loader, no address-layout surgery.
The IPC cost this implies is not a blanket problem: it's paid per call for
`interpreted` (acceptable — that mode already trades speed for
flexibility) and paid exactly once, at compile time, for `dynamic` (the
compiled bytes cross the boundary, not the calls). A build today embeds
*today's* `qela`; upgrading the installed compiler later doesn't change
already-shipped binaries — a feature (reproducibility), not a bug.

**Implemented so far is the pre-embedding half of this, not the embedding
itself** — worth being precise about, since it's a real, load-bearing
correction to this section. `/proc/self/exe` only resolves to *the process
reading it* — at the *host program's own runtime*, that's the host, never
`qela`. The self-copy therefore has two halves that were conflated above:
(a) at **compile time**, `qela` reads its own bytes via `/proc/self/exe`
(reliably itself, since it's the process doing the compiling) and would
embed them into the output ELF; (b) at the **host's runtime**, the host
materializes that embedded copy via `memfd_create()`+`fexecve()` and never
needs to resolve a path at all — the "reproducibility" property above only
holds once (a) exists, since only then are the bytes frozen at build time
rather than whatever `qela` happens to be installed when the host runs.
Half (a) is not built — it needs a general "embed an arbitrary byte blob
into the output ELF" primitive that does not exist yet (`elf.qela` embeds
*qela's own* std/ as string literals via `genblob.py`, at qela's own build
time, which is a different thing). Until it exists, `std/eval.qela`'s
`abi_spawn()` does the honest fallback instead: `$QELAPATH`, then a `$PATH`
search for `qela`, then an actionable error — the host needs a real `qela`
install reachable at its own runtime. This is a real, working, tested
mechanism (see "What's implemented" below); it is just not yet the
zero-install, frozen-at-build-time version this section originally
described. `sys_memfd_create`/`sys_fexecve` (in `std/sys.qela`, done) are
exactly what half (b) will need once half (a) exists.

## What's implemented (2026-08-08)

A first vertical slice, real and tested end to end, not a stub:

- **The grammar** (`fn interpreted`/`fn dynamic`/`fn frozen`, `eval var`/
  `eval fn`) — see item 3 below. AOT compilation of an actual
  `interpreted`/`dynamic` function body still refuses with an actionable
  error; `qela irun` already runs such functions correctly (every function
  is interpreted there).
- **The wire protocol** (`std/abiwire.qela`, shared by both ends since it
  has no reason not to be): `[u32 total_len][u8 kind][payload]` frames.
  `total_len` includes the kind byte. One request kind so far (`run this
  source`), two response kinds (`ok, here's an i64` / `error`).
- **The server** (`qela --abi-server`, `srcql/main.qela`, hidden — not in
  `--help`): reads frames on fd 0, writes frames on fd 1, reuses the
  *entire* `qela repl` machinery (`repl_init`/`repl_run_source`/
  `repl_sync`/the fork-validate-then-commit crash isolation) unchanged —
  an eval server *is* a REPL driven by a wire protocol instead of a TTY,
  down to sharing the exact same persistent-session semantics: each `eval`
  call is classified exactly like one REPL input line (bare expression →
  its value; one or more statements → they execute for effect and the
  call returns whatever the previous expression call returned, 0 if
  there hasn't been one yet — this is inherited from REPL's own model, not
  a new design). State (`var`/`let`/`fn`/`struct`/...) persists across
  calls within one spawned child, exactly like a REPL session. A crashing
  `eval` (division by zero, a compile error) is caught by the same
  fork-validate step the REPL already used — the persistent session is
  provably unaffected, verified by running further calls after a crash in
  the same test.
- **The client** (`std/eval.qela`, stage1-only, opt-in via `import
  "std/eval.qela";` — deliberately **not** auto-triggered by scanning for
  calls named `eval`, unlike the `need_fmt`/`need_dyn` splice pattern this
  might otherwise have copied: `tests/match.qela` already has its own
  unrelated `fn eval(op Op) int`, and auto-splicing a global `eval` by
  name would have broken it and any other program with its own `eval`
  the moment it compiled, with no relation to this feature. Explicit
  import avoids the collision entirely, at the cost of point 3's "no
  source change needed" framing not quite holding for `eval` specifically
  — a one-line `import` is the price): `abi_spawn()` (lazy, kept alive for
  the process's lifetime, resolves the subprocess as described above) and
  `fn eval(src str) i64`.
- **A permanent regression test**: `tools/bootstrap.sh`'s "eval() over the
  abi subprocess" step — compiles a program that calls `eval()` twice with
  state carried between calls, runs it with `QELAPATH` pointing at the
  just-built `s2`, checks the output. Part of `make build`.

What a compiled program can do today: `import "std/eval.qela"; ... var v
i64 = eval("some qela expression");` — real, arbitrary, JIT-free
tree-walked execution of Qela source at runtime, in a process that never
linked a compiler frontend into itself.

**Call-site trampolines for `fn interpreted`/`fn dynamic` — also done,**
and the mechanism turned out not to need any new codegen at all. A
function marked `interpreted`/`dynamic` keeps its real, user-written body
in a new `Func.interp_body` field; what `codegen.qela` actually compiles
as `Func.body` is a small *synthesized* replacement — parsed from
generated source text, right where the real body just finished parsing,
in `parse_function` (`srcql/parse.qela`) — that forwards the function's
own parameters into `__interp_call_dispatch(def_src, name, &args[0])`
(`std/interpcall.qela`, auto-imported the same way `need_fmt`/`need_dyn`
already work) and returns its result. The caller-side machinery
(`gen_call`/`gen_args`, register/stack argument placement) never changes:
it's compiling an ordinary call to an ordinary-looking function, exactly
as it always has. `qela irun` and the ABI server both prefer
`interp_body` over `body` when present (`ip_body_of` in `interp.qela`),
so the *real* code still runs there — only AOT output goes through the
dispatcher. `__interp_call_dispatch` registers the function's original
source with the child once (by name, cached client-side) and then sends a
new `ABI_REQ_CALL` message per call — binary-marshalled `i64` arguments,
not re-parsed text — which the server answers by building `num_node`
argument nodes and calling the registered `Func` through the ordinary
`ip_call`, fork-validated exactly like `eval`. Scope, matching `eval`'s
own restrictions: parameters and return type must be non-aggregate,
non-float, ≤8 bytes (checked at parse time with an actionable error); the
function's own source text must not contain string interpolation (the
same `${`-collision guard `tools/genblob.py` already has, reused for the
same reason — the source gets re-embedded as a string literal to cross
the ABI). **`dynamic` is not yet a distinct, faster path** — it currently
behaves identically to `interpreted` (every call round-trips over the
ABI); the local `mmap`(RW)→copy→`mprotect`(RX)→call-natively optimization
described in "Call-site mechanics" above is still open, tracked alongside
item 4 below.

**`eval fn` host-call bridging — also done** (2026-08-08). An `eval`'d or
`interpreted`/`dynamic` call to a name that isn't defined locally no
longer just fails: `type_call` (`srcql/type.qela`), gated behind a new
`type_abi_mode` flag that `cmd_abi_server` alone sets, tags the call node
`foreign` instead of erroring (same restrictions as everywhere else —
non-aggregate, non-float, ≤6 args). At interpret time, `ip_call_node`
(`srcql/interp.qela`) recognizes a `foreign` call and, instead of running
it locally, marshals the evaluated arguments and asks the *host* over the
same two ABI fds, blocking for the answer — a genuinely new direction on
the wire: the child, not just the host, can be the requester. The host
side needed a real dispatcher: whenever a program pulls in the ABI client
(detected by `find_func("abi_spawn") != 0`, since `eval fn` itself doesn't
signal anything at parse time), `parse()` synthesizes
`__eval_call_dispatch(name str, args *i64) i64` — one `if
(str_eq(name, "..."))` per `eval fn`-tagged function, always present
(even as a trivial always-"not found" stub) so linking never depends on
whether any `eval fn` actually exists. `eval()` and
`__interp_call_dispatch` both now wait for their real response through
a new shared `abi_wait_result` (`std/abiconn.qela`) that answers any
interleaved host-call request inline before continuing to wait — the
`eval()`/interpreted-call machinery didn't need to change beyond calling
this instead of a bare `abi_recv_frame`.

Two real bugs surfaced building this, both worth recording:

1. **The ABI protocol moved off fd 0/1 onto fixed fds 3/4**
   (`ABI_FD_HOST_TO_CHILD`/`ABI_FD_CHILD_TO_HOST`, `std/abiwire.qela`).
   It had to: the fork-validate-then-commit safety net (borrowed from the
   repl, see the repl entry above) mutes the validation
   fork's stdout so a bad line's side effects aren't visible twice — fine
   when stdout is a terminal, fatal when stdout *is* the ABI channel. A
   foreign call made during validation wrote its request into `/dev/null`
   and then blocked forever reading a reply that could never arrive,
   hanging the whole host. Dedicated fds mean the interpreted program's
   own `write_str(STDOUT, ...)` and the protocol never collide again,
   validation-muting included.
2. **`abi_wait_result` used the caller's small result buffer for the
   *incoming* frame too.** `eval()`'s and `__interp_call_dispatch`'s
   result buffers are 8 bytes — plenty for the final `i64` answer, far
   too small for a host-call request (a name plus 48 bytes of packed
   arguments). `abi_recv_frame` correctly refused to read a frame larger
   than the caller's stated capacity and returned an error *before even
   reading the kind byte* — which `eval()` reported as "the subprocess
   died," a misleading symptom for an oversized-buffer bug. Fixed with an
   internal 512-byte scratch buffer inside `abi_wait_result` for
   receiving, only copying into the caller's small buffer for the actual
   final response.

**`eval var` get/set — also done** (2026-08-08), the same day, following
the call half's template closely. Get: a bare identifier that resolves to
neither a local `Var` nor a `Func` no longer errors under
`type_abi_mode` — it's the same `ND_FUNCADDR` node the parser already
produces for "not found yet, might be a function declared later" (see
`parse_primary` in `srcql/parse.qela`), so `add_type`'s existing
`ND_FUNCADDR` branch (`srcql/type.qela`) just gained the same "tag it
`foreign`, assume `i64`" fallback `type_call` already had. Interpreting
it (`ip_ev_funcaddr`) asks the host with a new request kind (4) instead
of treating the name as a function value. Set needed one more piece
`eval fn` didn't: `ip_assign` (`srcql/interp.qela`) checks `n.lhs.foreign`
*before* computing an address (`ip_addr` has no idea what to do with a
foreign `ND_FUNCADDR` node — it was never meant to be an assignment
target) and sends a new request kind (5), name plus the evaluated RHS
value, instead. Both dispatchers (`__eval_get_dispatch(name str) i64`,
`__eval_set_dispatch(name str, v i64)`) are generated by `parse()`
alongside `__eval_call_dispatch`, walking `global_vars` for
`.eval_export` instead of `all_funcs`. Verified the security boundary
holds: a plain `var` (no `eval var`) is invisible to `eval`'d code —
reads come back `0` from the always-generated default-case dispatcher,
exactly like an unexported function.

This surfaced a third bug, more interesting than the first two because
it's not a wiring mistake but a real architectural tension: **the
fork-validate-then-commit safety net double-applies any foreign write.**
The repl (and now the abi server, borrowing the identical pattern) always
runs a line twice — once in a forked, muted child purely to check it
doesn't crash, then for real in the parent if that succeeded — accepted
for years because the *known* cost was cosmetic (a `write_str` visible
twice). `eval var`'s set half made that cost land on the *host's own
state*: `eval("price = price + 50;")` against `price = 100` produced
`200`, not `150` — the validation run genuinely applied the write (fd 3/4
aren't muted, only stdout is), and the commit run applied it again on top.
Fixed with a new `ip_abi_validating` flag (`srcql/interp.qela`), set only
inside the two validation forks (`abi_try_run`/`abi_try_call` in
`srcql/main.qela`): `ip_foreign_get`/`ip_foreign_set`/`ip_foreign_call`
skip the real ABI round trip and return a dummy value whenever it's set,
so validation still exercises everything *around* a foreign touch (and
still catches a crash there) without the touch itself ever reaching the
host twice. Caught by turning the doc's own worked example into the
regression test (`tools/bootstrap.sh`'s "eval var" step asserts
`price=150`, not `200` — it would have shipped silently wrong without a
concrete expected value to check against).

**One `interpreted`/`dynamic` function calling another — a real,
launch-blocking bug, found and fixed** (2026-08-08). Each function was
registering *itself* with the child lazily, on its own first call, which
worked as long as the only caller was AOT code. The moment one
`interpreted` function called another — `square` and `quad` in the
example above — the *callee* had never been registered when the child
hit it, so `type_call`'s `type_abi_mode` fallback did exactly what it's
supposed to do for a genuinely unknown name: treated `square` as a
foreign *host* call. The host's `__eval_call_dispatch` had never heard of
`square` either (it only knows `eval fn`-tagged names), so it silently
returned `0` — `quad(2)` came back `0`, not `16`, no error, no crash, just
a wrong answer. This is exactly why `--interpreted`/`--jit` (below) were
the thing that surfaced it: forcing *every* function through the ABI
makes function-calls-function the common case instead of a corner case.
Fixed by registering all `interpreted`/`dynamic` functions **together**,
once, when the child connects — `parse()` concatenates every one of
their source spans into a single generated `__interp_all_src() str`
(mirroring the other generated dispatchers), and `abi_spawn()`
(`std/abiconn.qela`) sends it as one `ABI_REQ_RUN` right after the pipes
come up, before any real call happens. Calls between them now resolve as
ordinary local calls inside the child's own function table — no foreign
fallback involved at all. `__interp_call_dispatch` simplified as a
result: it no longer carries a source argument or does its own
lazy-registration bookkeeping (that whole mechanism — `InterpReg`, the
per-name registered-set, `interp_register` — is gone from
`std/interpcall.qela`).

**`--interpreted`/`--jit` global flags — done.** Parsed in `main()`
(mutually exclusive, `error_at`-style `die()` if both given).
`parse_function` applies the matching modifier to any function that has
no explicit `naked`/`interpreted`/`dynamic`/`frozen`, isn't `extern`,
isn't a compiler-internal name (`__`-prefixed), **and isn't defined in a
`std/` module** (`is_std_path` on the function's own source file). That
last exclusion is load-bearing, not cosmetic: without it, `--interpreted`
would also convert `str_eq`, `arena_alloc`, `abi_spawn` and everything
else the ABI client itself is built from, into functions that need the
ABI client to reach — a circular bootstrap that would hang or crash
before the first real call. So the actual behavior is "every function
*you* wrote becomes interpreted/dynamic; the standard library underneath
keeps running natively" — closer to how JIT'd languages usually draw this
line (your code is hot-swappable, the runtime under it isn't) than to a
literal reading of "every function." Includes `main` itself, which is
just an ordinary function from this mechanism's point of view — verified
in the test (`main` becomes a trampoline too, and still runs correctly).

One known cosmetic side effect, not a correctness issue: a local used
only inside an `interpreted`/`dynamic` function's *real* body (never
referenced by the synthetic trampoline body codegen actually compiles)
can trigger a spurious "unused variable" warning, because `warn_unused`/
`count_reads` walk `Func.body` — the synthetic one — and never see
`interp_body`. Harmless (it doesn't affect what's emitted, `main`'s
compiled form never touches those locals directly either way), but worth
fixing properly if this becomes annoying: `count_reads` would need to
walk `interp_body` instead of (or in addition to) `body` when one is set.

What it cannot do yet, precisely: `dynamic` does not actually run any
faster than `interpreted` — every call still round-trips over the ABI,
the local `mmap`/`mprotect` JIT path is unbuilt; `parse_ast`/`run_ast`
and AST inspection do not exist (item 5); and the self-copy is not yet
embedded at compile time (the correction above).

## Size measurements (real numbers, not estimates)

Baseline: `qela` (S2) = 526,816 bytes, 50.2% of the 1 MiB budget (up from
501,488 before this round of work: six new syscall wrappers, four new
grammar modifiers/fields, the wire protocol plus `--abi-server`, the
`ABI_REQ_CALL` handler for call-site trampolines, the `type_abi_mode`/
foreign-call/dispatcher-generation machinery, `eval var` get/set, the
combined-registration fix, and the `--interpreted`/`--jit` flags,
~24 KB total — crossing the halfway mark of the 1 MiB budget.
`std/eval.qela`, `std/abiwire.qela`, `std/abiconn.qela` and
`std/interpcall.qela` do not count against this budget: they ship in the
*output* of programs that import them, never inside `qela` itself.)

Self-copy (whole binary embedded in the *output*, cost does not touch
`qela` itself):

| what's embedded in the output | size |
|---|---|
| raw `qela` binary, uncompressed | 501,488 |
| + gzip -9 | 137,941 |
| + xz -9 | 111,420 |

What permanently embedding a source blob *inside `qela`* would have cost —
kept only as a record of a rejected path, not a live option under the
resolved design:

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

1. **The ABI's wire format — done except the JIT half.** A length-prefixed
   binary protocol (`std/abiwire.qela`, `[u32 total_len][u8 kind][payload]`)
   over a plain pipe pair (`sys_pipe2`) on fixed fds 3/4, bidirectional:
   host→child carries "run this source" (kind 1) and, now, child→host
   carries "call this host function" (kind 3), "read this host global"
   (kind 4), and "write this host global" (kind 5) — see "What's
   implemented" above for how the child ended up as a requester too, not
   just the host. Two response kinds (`ok` / `error`) both ways. Not done:
   "compile this AST, give me code bytes" (needed for `dynamic`'s actual
   JIT, item 4) and a marshalling convention for values that aren't a bare
   `i64` or a fixed 6-slot argument array (structs, strings) — every
   message shape built so far only ever carries "a source string," "an
   i64," or "a name plus up to six packed `i64` args."
2. **Child process lifecycle — decided and implemented for `eval`.** One
   child per host process, spawned lazily on first `eval()` call
   (`abi_spawn()` in `std/eval.qela`), kept alive for the host's lifetime
   — not re-forked per call. If the child dies mid-run, `eval()` reports
   the error and calls `sys_exit(1)` rather than silently respawning: a
   dead interpreter subprocess means the session state (every `var`/`fn`
   defined so far) is gone, and silently starting a fresh, empty session
   under the same variable names would be a correctness trap, not a
   convenience. This still needs re-deciding once `interpreted`/`dynamic`
   call-site trampolines (item 4) share the same child — a `dynamic`
   function that already JIT'd locally doesn't need the child to still be
   alive to keep working, so the "hard error" policy above may turn out to
   be `eval`/`interpreted`-specific rather than global.
3. **Grammar — done.** `fn interpreted name(...)`/`fn dynamic name(...)`/
   `fn frozen name(...)` are contextual modifiers, mutually exclusive with
   each other and with `naked`, parsed in `parse_function`
   (`srcql/parse.qela`) the same place `naked` already is. `eval var
   name T;`/`eval fn name(...)` at top level set `Var.eval_export`/
   `Func.eval_export` (`srcql/comp.qela`). None of the four new words
   (`interpreted`, `dynamic`, `frozen`, `eval`) were added to
   `srcql/lex.qela`'s `is_keyword` table — same choice already made for
   `comptime` — so they stay ordinary identifiers everywhere except these
   exact syntactic positions; `tests/match.qela`'s `fn eval(op Op) int`
   (an unrelated user function literally named `eval`) still compiles
   because of this. `extern fn interpreted`/`extern fn dynamic` is a
   compile error (an extern function is C-linked, not tree-walked or
   JIT'd). Since no call-site trampoline exists yet (item 4), an AOT
   compile of a function actually marked `interpreted`/`dynamic` errors
   at `srcql/codegen.qela`'s `gen_func` with a message pointing at `qela
   irun`, which already runs such a function correctly today — every
   function is interpreted there regardless of the annotation. `frozen`
   and `eval_export` are otherwise inert until items 1/2/4 exist: a program
   using them compiles as ordinary AOT code today.
4. **Call-site trampolines — done, but not via codegen.qela.** Turned out
   not to need new codegen: `interpreted`/`dynamic` functions get a
   *synthesized replacement body* at parse time (see "What's implemented"
   above) that ordinary codegen compiles like any other function calling
   a runtime helper. Still open: the actual JIT half. Right now `dynamic`
   is just `interpreted` under another name — every call round-trips over
   the ABI. The local `mmap`(RW)→write→`mprotect`(RX) sequence after a
   one-time compile round trip, so calls after the first run at native
   speed with no ABI involvement, is unbuilt; it needs the child to answer
   "compile this, give me code bytes" (a new ABI message — `-c`'s object
   output is the natural source for those bytes) and the host to load and
   jump to them, which likely *does* need real codegen work (a
   self-patching trampoline, or an indirect call through a pointer filled
   in after first JIT) since "run these bytes" isn't expressible as an
   ordinary call the way the `interpreted` half turned out to be.
5. **The `*Ast` API surface**, including whether `*Ast` is a local pointer
   or a handle into the child's memory (see "Call-site mechanics") — scope
   as its own design pass once (1)–(4) are settled.
6. **`sys_readlink`, `memfd_create`, `fexecve`, and pipe/socketpair
   wrappers — done**, in `std/sys.qela`: `sys_readlink`,
   `sys_memfd_create`, `sys_fexecve` (via `execveat` + `AT_EMPTY_PATH`,
   there is no bare `fexecve` syscall), `sys_pipe2`, `sys_socketpair`.
7. **W^X on hardened kernels.** `mprotect(RW→RX)` for the `dynamic` path
   can be restricted under some hardening policies (SELinux, some
   container runtimes) — needs a real error message, not a silent
   failure, when it's unavailable.
8. **Test plan — all six done**, though "`--jit`" currently exercises the
   same call mechanism as `--interpreted` rather than a distinct JIT path
   (see item 4). `tools/bootstrap.sh`: "eval() over the abi subprocess"
   (bare `eval`, cross-call session state); "interpreted/dynamic
   call-site trampolines" (both under AOT and `qela irun`); "eval fn:
   host functions callable from eval'd source"; "eval var: host globals
   readable and writable from eval'd source" (including the double-write
   regression); "interpreted functions calling each other" (both under
   AOT and `qela irun` — this one caught a real bug, see "What's
   implemented" above); "--interpreted and --jit global flags". A
   dedicated `dynamic`-is-actually-faster test still needs the JIT half
   to exist first.
