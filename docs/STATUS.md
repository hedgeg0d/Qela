# Where the project stands

Updated 2026-08-05. Read `BOOTSTRAP.md` first; it constrains everything below.

## Numbers

| | |
|---|---|
| stage0 (`src/*.c`, the throwaway bootstrap) | 46 696 B |
| **S2 — the shipped compiler, Qela compiled by itself** | **324 400 B** |
| stage1 sources | 12 785 lines of Qela |
| Emitted code vs `gcc -Os` on `bench/` | **231%**, or **192%** without bounds checks (M4 gate wants ≤150%) |

Everything is verified by `tools/bootstrap.sh`: S2 == S3 byte-for-byte, the 124-test corpus under S2, the embedded stdlib resolving outside the source tree,
coroutines, channels, the collector, `run`/`fmt`, stdin compilation, the panic
backtrace, interpolation and the repl, the compiler flags (`-g`,
`--backtrace`, `--no-bounds-checks`, `--dump-std`), and a scripted language
server conversation.

## Done

**Real parallelism: OS threads, `tvar`, `go`, work-stealing (2026-08-05).**
Coroutines used to be concurrency without parallelism -- one process, one
cooperative scheduler, no multi-core speedup. Now a fixed pool of real
`clone()`-backed OS threads each run their own coroutine scheduler, with
`go f(...)` (sugar identical to `spawn`, both desugar through
`parse_spawn_like` in `srcql/parse.qela`) round-robining work across them
and channels doing the cross-thread handoff. Six pieces, in build order:

- **Atomics.** `atomic_cas`/`atomic_add`/`atomic_load`/`atomic_store`/
  `fence` are codegen intrinsics with no Qela definition (`lock cmpxchg`/
  `xadd`, plain `mov`, `mfence`), following the same "magic function name"
  pattern as `gc_save_regs`/`coro_switch` -- no lexer or parser changes.
- **`thread_clone`.** A raw `clone()` syscall wrapped as another such
  intrinsic (`srcql/codegen.qela`, `gen_thread_clone`), because the child
  resumes on a brand-new stack holding none of the caller's frames -- an
  ordinary compiled `ret` would pop garbage off it. `fn`/`arg` cross the
  syscall in `r12`/`r13` (the syscall only clobbers `rcx`/`r11`); the
  child branch calls `fn` directly and exits without ever executing a
  compiled `ret`. Found and fixed a real bug here: the compiler's own
  process-exit path used plain `exit` (60), which races a still-running
  worker thread over which one's exit status the kernel reports for the
  whole process -- switched to `exit_group` (231), the only way `main`'s
  own status is ever authoritative.
- **`tvar`.** Thread-local storage with no ELF `PT_TLS` -- there is no
  dynamic linker here to need the real ABI, so it's a compiler-private
  flat `%fs:offset` block instead: a self-pointer at offset 0 (LEA cannot
  read a segment base, so materializing a `tvar`'s address always starts
  with `mov reg, fs:[0]`), tvars packed after it. `tls_init()` builds a
  fresh block for whichever thread calls it; the entry stub calls it
  automatically before `main()`, but only when the program actually
  declares a `tvar` -- a program with none pays nothing, not even the
  second ELF `LOAD` segment a placeholder byte would have forced (a real
  regression, caught by `tools/bench-size.sh` going from 231% to 303% on
  benchmarks that don't use threading at all, fixed same day).
- **Thread-safe channels.** `Chan(T)` gained a spinlock guarding every
  field, built from `atomic_cas`, never held across a block (send/recv
  became lock-check-unlock retry loops instead). The single-scheduler
  deadlock heuristic (`chan_nblocked`/`chan_epoch`/`chan_stall`) is
  fundamentally unsound once several OS threads share a channel -- a
  sibling thread being slow to get scheduled looks identical to real
  deadlock from a scheduler that only knows about its own coroutines --
  so `thread_pool_init` turns it off outright rather than let it produce
  false crashes; the counters moved to `tvar` so they at least stay
  correct per-thread while it's still on for single-scheduler programs.
- **GC stop-the-world.** No signals -- a thread registry plus
  `gc_safepoint` (checked at `gc_alloc`'s own entry, the one call an
  allocating thread makes often enough to park promptly) is the
  rendezvous. `gc_collect` requests a stop, waits for every other
  registered thread to park *or deregister* (a thread that calls
  `gc_thread_exit` without ever hitting another safepoint is a valid way
  out of the wait too -- missing that was a real deadlock, caught by a
  worker thread that finished its trivial workload before the collector
  ever asked it to park), then walks their saved stacks/registers
  alongside the usual data segment and current stack.
- **`thread_pool_init(n)` / `go`.** Each pool thread runs its own
  `coro_run_all`-shaped loop over a `tvar`-scoped `coros[]` -- two OS
  threads must never see each other's coroutines, so `coro.qela`'s
  scheduler state (`coros`/`ncoros`/`cur_coro`) is `tvar` now too, and
  `coro_spawn`'s stack allocation gained its own lock around the
  `arena_alloc` call site specifically: `arena.qela` itself is bootstrap
  subset (stage0 compiles it, and stage0 has never heard of `atomic_cas`),
  so the fix had to live in `coro.qela`, which is stage1-only. Idle
  threads steal *unstarted* spawn requests from a neighbor's queue before
  spinning (`pool_steal` in `std/thread.qela`) -- not a running
  coroutine's stack/registers, which stays pinned to its thread for good;
  migrating a *live* coroutine mid-flight is real extra work nothing has
  needed yet. `pool_drain` pops one queue entry per lock acquisition
  rather than the whole backlog at once, a real bug found the same way:
  holding the lock (or even re-acquiring it back to back) across a whole
  burst starved every steal attempt for as long as the burst lasted,
  which defeated stealing exactly when it would have mattered.

Every piece landed with its own corpus test and was run 15-50 times back
to back looking for the flakiness a threading bug tends to hide behind a
single green run: `tests/atomics.qela`, `tests/atomics_cas_loop.qela`,
`tests/thread_clone.qela`, `tests/tvar_basic.qela`,
`tests/tvar_threads.qela`, `tests/chan_threads.qela`,
`tests/gc_threads.qela`, `tests/thread_pool.qela`,
`tests/thread_steal.qela`. `tests/gc_coro.qela` also pins a pre-existing,
unrelated bug found on the way in: `gc_collect` only ever scanned the
*current* coroutine's stack, not every live one's -- fixed first, in
isolation, before any of the above.

Known limits, stated rather than hidden behind a heuristic that half-works:
the pool scheduler is round-robin plus queue-level stealing, not Go's
work-stealing across live goroutines -- a coroutine already handed to a
thread stays there. A thread that never allocates and isn't in the pool
loop can't be paused by the collector; nothing here tries to preempt it.
Cross-thread deadlock detection does not exist (not a buggy version of
it -- it is off). Costs +25.7 KB in S2.

**Default and named arguments (2026-08-05).** `fn sum(a=0 i64, b=0 i64) i64`
declares defaults; `sum(1,2)` still takes the unchanged positional path.
Naming any argument requires naming all of them -- `sum(a=1, c=3)` -- decided
purely from the shape of the *first* argument (`ident '=' expr` naming a real
parameter of an already-resolved, non-generic function): once a call commits
to named mode every further argument must match it, or it's a parse-time
"cannot mix positional and named arguments" error. A default may name a
global freely, or an earlier *required* (non-defaulted) parameter of the same
function -- substituted at the call site with whatever the caller actually
passed for it -- but never another defaulted parameter (no dependency order
between defaults, enforced when the default is parsed). The whole feature is
a parse-time-only AST rewrite (`call_args_named`, `default_expand`,
`expr_refs_default_param` in `srcql/parse.qela`, plus a `default *Node` field
on `Var` in `srcql/comp.qela`): a named call reorders into the function's
declared parameter order and clones in any missing defaults (the same
clone-and-substitute shape `macro_expand` already uses for `macro`, keyed by
`Func` parameters instead), producing an ordinary positional `ND_CALL` before
`type.qela` or `codegen.qela` ever see it -- zero changes to either. Inherits
the macro system's evaluation contract: a required parameter with a side
effect, named by more than one default, runs once per default that names it.

Known v1 limits: a call can only use named/default arguments against a
function already resolved at parse time (declared earlier in the file, or
via an already-parsed import) -- a genuine forward reference silently falls
back to requiring a full positional list, same as before this feature
existed. Indirect calls through a function-typed variable don't participate
either (no fixed parameter names to match against).

Backward compatible by construction: `sink(x = 5)` (assignment used as a
call argument, `tests/assignval.qela`) still means what it always meant,
because the named-argument shape check requires the name to match one of the
callee's actual parameters -- `sink`'s only parameter is `a`, not `x`, so it
never leaves the pre-existing parse path. `tests/defaultargs.qela` covers the
positive shapes (defaults, named calls in any order, an all-defaulted `f()`,
substitution from both a required parameter and a global);
`tests/defaultargs_reject.qela` and `tests/defaultargs_reject_chain.qela`
pin the two rejections. Costs +3768 B in S2 (294 944 -> 298 712).

**Typed block expressions (2026-08-05).** `T { stmts...; super v; }` in
expression position: an inline, value-producing block. It desugars at parse
time to a hidden function called immediately in place (`parse_typed_block`
in `srcql/parse.qela`), the same shape a lambda already produces, so codegen
needed no changes at all. `super v` is `return v` inside that hidden
function -- a separate keyword so it reads as "this is the block's value",
not "exit the enclosing function"; valid only inside a typed block
(`capturing` flag), rejected everywhere else. Unlike a lambda it can read
outer locals: the existing "no closures" check in `parse_primary` (a
variable resolved outside the lambda's own scope chain is an error) gets a
second mode, gated by the same `capturing` flag -- instead of erroring, the
first read of an outer local turns it into a hidden by-value parameter
(`capture_get_or_add`), reusing the exact register-word/stack-spill
bookkeeping `parse_func`'s parameter loop already does. Every later read of
the same local resolves to the same hidden parameter (deduped by `Var`
pointer identity in a small capture list). Since a parameter is always a
copy, the capture is readonly by construction -- no separate enforcement,
and verified both for scalars and for a >16-byte struct (passed by pointer
in this ABI): mutating the capture inside the block never changes the
caller's variable, confirming the by-ref path already copies to a temp
rather than aliasing caller storage. Nested typed blocks capture through
both levels correctly (a plain lambda nested inside one still rejects
captures -- `capturing` is forced false for the duration of `parse_lambda`'s
own body). `Name{` stays struct-literal syntax; a typed block is only
offered for non-struct types (`named.kind != TY_STRUCT`), since telling
`StructName { field: val }` apart from `StructName { statements... }`
would need real lookahead -- not built, a known v1 gap. `super` and `T {`
are stage1-only, like every construct that reaches through `parse_lambda`'s
machinery. Costs +2976 B in S2. Pinned by `tests/typedblock.qela` (no
capture, readonly scalar and struct capture, multiple captures, nesting)
and `tests/typedblock_reject.qela` (`super` outside a typed block).

**Dot-call method sugar (2026-08-05).** `x.foo(y, z)` desugars to
`foo(x, y, z)` at parse time (`srcql/parse.qela`, `parse_postfix`), before
any name is resolved. No new node kind, no new type-check or codegen path:
the rewritten call goes through the exact `find_func`/`type_call` path an
ordinary `foo(x, y, z)` would, so an argument-type mismatch on `x` reports
the same way. `.name` with no trailing `(` still parses as `ND_MEMBER`
(field access) exactly as before, so a struct field and a free function can
share a name -- `x.val` reads the field, `x.val()` calls the function,
distinguished purely by whether `(` follows. No overloading exists yet
(`find_func` is a plain linear name lookup, first match wins), so there was
no receiver-type dispatch to build; the rewrite intentionally goes through
the same call-resolution path a plain call uses, so overload resolution,
if it's added later, applies to both call styles at once with no separate
work. Chains fold naturally (`a.foo().bar()`) since the postfix loop just
keeps consuming `.`. Costs +392 B in S2. Pinned by `tests/dotcall.qela`
(chained calls, field/method name sharing). Stage1-only: the rewrite lives
only in `srcql/parse.qela`, not the frozen `src/parse.c`.

**`~` opt-in dynamic typing (2026-08-05).** `var ~n = expr;` declares a
local whose type is checked at runtime instead of compile time: it can be
reassigned across unrelated types (`n = 5; n = "hi"; n = SomeStruct{...};`),
and its value only comes back out through an explicit `n as T`, which
checks a runtime tag and panics on mismatch rather than reinterpreting the
bytes. No operators work on a `~` value directly (no `~n + 1`) -- everything
has to be cast out first, so this is a boxed variant, not JS-style implicit
coercion. Locals only: no `~` parameters, struct fields or return types yet
(see above), and `let` cannot be dynamic (nothing to reassign).
`extern ~var` is a compile error -- a value's type id is only stable within
the compilation that assigned it, so it cannot cross the `-c` object
boundary -- and assigning one dynamic value straight into another is
rejected too, to avoid two variables aliasing the same heap box.

The representation is `{type_id i64, ptr *u8}`, a 16-byte struct built
once by the compiler (`type_dynval` in `srcql/type.qela`) and registered
under the name `DynVal` before `std/dyn.qela` (the two runtime helpers,
`dyn_box` and `dyn_check`) is spliced in, so every site agrees on the exact
same `Type` pointer -- `same_type` is identity, not structural equality,
so two independently-parsed copies of the same shape would never compare
equal. `type_id`s are sequential integers assigned by the compiler itself,
by pointer identity, the first time a type is boxed or cast-checked; the
id is baked into the generated code as a plain constant, so nothing like a
type registry ships in the compiled binary. The value is always heap-
allocated (`std/heap.qela`, never the arena) regardless of size, so there
is exactly one code path for a primitive and for an arbitrary user struct.

Both `var ~n = expr;` and `n as T` are pure rewrites in `srcql/type.qela`
into node kinds that already exist -- a box becomes `(tmp = expr,
dyn_box(id, &tmp, size))` (a comma of an ordinary assign and an ordinary
call), an unbox becomes `*(dyn_check(n, id) as *T)` (a call, a pointer
cast, a deref) -- so `srcql/codegen.qela` needed no changes at all: the
existing aggregate-copy, call and deref paths just run. Costs +4488 B in
S2. Pinned by `tests/dyn.qela` (box/reassign/cast, including a user
struct), `tests/dynmismatch.qela` (the runtime panic), and the reject
triple `tests/dynreject.qela` (`let ~x`), `tests/dynreject_extern.qela`
(`extern ~var`), `tests/dynreject_direct.qela` (dynamic-to-dynamic
assignment).

**`~` box cleanup (2026-08-05).** The leak above is closed by two
complementary fixes, neither of which needed the risky tree-splice from
`type-checking` that the original gap note ruled out.

`type_assign_dyn` (`srcql/type.qela`) now frees the box already sitting in
the target, if any, before installing the new one: the rewrite grows from
a two-part comma to `(tmp = expr, (heap_free(dynvar.ptr), dyn_box(...)))`.
`tmp = expr` runs first, so an expression that reads the *old* value of
the same variable through a prior `as` cast (`n = (n as i64) + 1;`) still
sees valid memory before the free; only then is the previous box released
and the new one installed. This alone covers every reassignment and every
loop iteration that redeclares the same `var ~n = ...` -- the with-
initializer form has no separate zero step (unlike a bare `var n T;`), so
the local's frame slot still holds the previous box's pointer between
iterations, not zero.

The remaining case -- the *last* box a `~` local ever holds, freed when
its declaring scope exits rather than at process exit -- needed the
parser after all, but structured differently than the original note
assumed: `parse_decl` now chains a synthetic `defer { heap_free(n.ptr);
n.ptr = 0 as *u8; }` onto `.next` right after the declaration it belongs
to, the same node kinds a hand-written `defer` would parse to. Two call
sites needed fixing to carry a two-node chain safely: `parse_block`'s
`tail.next = parse_stmt(t)` loop now walks to the real end of whatever
comes back instead of advancing one node at a time (a one-line change,
since a plain statement's chain is still length one), and every single-
statement body slot (an unbraced `if`/`while`/`for`, a match arm, the
body of `for x in coll`) routes its result through a new `stmt_as_body`
helper that folds a two-node return into a block -- safe specifically in
those slots because nothing else follows there. `DeferScope` turned out to
already be block-scoped, not function-scoped (`gen_block` runs its own
defers on every pass through the block, including a loop's back edge) --
the "defer stretches to the end of the function" describes
`regalloc.qela`'s conservative live-range extension for anything a defer
reads, not the unwind semantics -- so the synthetic defer frees on every
loop iteration for free, with no separate "free-before-rebox" hook needed
for that case.

The `n.ptr = 0` half of the defer is load-bearing, not decoration: without
it, the two fixes double-free the same iteration's box (the defer frees it
at the block's end, then the *next* iteration's free-before-rebox reads
the same now-stale pointer and frees it again), corrupting
`std/heap.qela`'s free list and hanging the next `heap_free`'s pointer-
chasing search in an infinite loop -- caught by running `tests/dyn.qela`'s
new loop case under a timeout, not by the type-checker. Costs +1544 B in
S2 for both fixes together. `tests/dyn.qela` gained a 100000-iteration
loop that reboxes a different type every pass -- a leak would balloon the
heap, and a wrong free order would hang or crash well before it
finishes; it runs in ~40 ms.

**Scalar float marshalling on the extern boundary (2026-08-05).** Floats
now cross `extern` directly. Qela computes with floats as raw bits in
integer registers, but a call to (or from) a C-facing function goes
through the SysV convention: float arguments land in `xmm0` and up with
their own counter, integer arguments keep the GPRs with their own, and a
float result returns in `xmm0`. On the callee side `gen_extern_params`
re-stages incoming SysV integer registers from the stack (pushed all at
once so later float loads cannot clobber them) and pulls floats out of
XMM into their Qela word slots; the epilogue pushes a float result from
RAX back into `xmm0`. The marshalling applies to every call to an
`extern`-marked function, whether it has a body or not, so the same code
serves both crossing directions. The new `gpr_to_xmm`/`xmm_to_gpr`
helpers carry a correct REX prefix for `r8`/`r9` (the older float moves
never saw a register that high). SysV marshalling is register-only: a
parameter that would spill is a compile error in `parse.qela` ("an
extern function takes at most six register words"), and indirect calls
through function pointers and aggregate floats (`Vector2` and friends)
are not marshalled. Pinned by `tests/externfloat.qela` and
`tests/externfloat_reject.qela`; the crossing is verified against gcc in
both directions. `examples/flappy/` now calls `GetFrameTime` and
`DrawCircle` directly and `glue.c` is gone.

**Float global initializers (2026-08-05).** Two constants bugs, both found
while writing the raylib example (`examples/flappy/`): an `f32` global
initializer stored the *f64* bit pattern in the 4-byte slot — the low half
of the pattern is zero for any exponent above 0x3ff, so the global read as
`0.0` in both the exe and the object writer — and a negated float literal
(`-340.0`) was folded by two's-complementing the bits in `eval_const`
instead of flipping the IEEE sign bit (the optimizer already did it
right; the parse-time constant path did not). Fixes: `f64_to_f32_bits` in
`srcql/codegen.qela` narrows the pattern with pure integer math (the
bootstrap subset has no float, and subnormal doubles are below the f32
range anyway, so they underflow to zero), and `eval_const` mirrors
`opt.qela`'s sign-flip for float negation. Pinned by
`tests/f32global.qela` (`// stage1-only`). `examples/flappy/` — Flappy
Bird against raylib, `-c` + `gcc -lraylib`, everything crossing directly
now that scalar floats marshal to SysV XMM registers (ints, pointers,
`bool`, `str` as `const char*`, `Color` by value, and the floats of
`GetFrameTime` and `DrawCircle`). Verified headless by driving
`update()` with stub externs, and on the display by a `TakeScreenshot` of
frame 30 (sky, ground, green strip, yellow bird, score all present).

**C interop, first cut (2026-08-04).** `extern fn` declares a function whose
body lives elsewhere, and `qela -c file.qela -o file.o` emits a relocatable
ELF object instead of a runnable binary. The system linker (`gcc`, `cc`, ...)
then joins Qela and C in either direction. A Qela `.o` exports every
function `extern fn f(...) {...}` declares with a body as `STB_GLOBAL`, and
`main` is exported too, so a C `main` can call into a Qela library; an
`extern fn f(...);` with no body stays an undefined global symbol for the
linker to resolve from C. Data goes both ways too: `extern var x i64;` is an
import (an undefined `STB_GLOBAL` object), `extern var x i64 = K;` is an
export (a defined global symbol), and every non-extern Qela global is local
to the object. `extern let` is a compile error and the type is mandatory.
The ABI was already SysV (six argument words in registers, RAX for the
result), so there is no marshalling. `str` crosses as a `{ptr, len}` pair (a
two-word C struct); aggregates over 16 bytes pass by pointer as always.
Struct layout is natural order = C's layout, so a Qela `struct` and the
equivalent C `struct` alias each other with no padding work. Volatile and
the rest are not yet shared.

The object is a fresh path, not the exe writer with a flag: no startup stub,
no entry requirement, no `main` check, no absolute-address patching. Instead
the image is described by relocations — `R_X86_64_PLT32` on calls (addend
-4, the linker's P is the slot itself while the CPU computes from the next
instruction; gcc emits the same). Every data reference (string headers,
global addresses, `extern` slots) is a RIP-relative `lea` with `R_X86_64_PC32`,
so the object links under plain `gcc` as a PIE as well as with `-no-pie`; a
mov of an absolute 32-bit address (`R_32S`) is what forces `DT_TEXTREL`.
GOTPCREL was tried and dropped: binutils 2.46.1 dies with a BFD internal
error (`elf64-x86-64.c:3727`) on any object that uses it, so all references
stay PC-relative. Shared libraries work anyway: calls go through the PLT
(`R_X86_64_PLT32` becomes a PLT stub), and data the linker resolves from a
`.so` gets a COPY relocation into the executable — with the usual GNU caveat
that the library's own references to that variable still point at its own
copy, so shared mutable data can diverge. The object's `.text` carries
`SHF_ALLOC` (a missing flag made binutils refuse to create the PLT/COPY
entries; it was caught by the first `.so` link). The string headers
live in `.data.rel.ro` (with a `.rela.data.rel.ro`) rather than `.rodata`,
for the same reason: a relocation inside `.rodata` marks the whole load
text-relative. The string bytes keep a `.Lstrd<N>` symbol, the header a
`.Lstr<N>` one, `R_X86_64_64` fills the header's own pointer. Bounds checks
and `assert` work in object mode: the trampolines and the panic/assert stubs
are appended after the pass loop, and the bounds message is registered as a
real string so its `.Lstrd` symbol anchors the stub's PC32 relocation — in
the exe the same message is appended as raw bytes instead. `main` returning
into glibc works because nothing in the startup stub (zeroed locals, bounds
checks, panic) depends on libc.
Built entirely in `srcql/elf.qela` (`write_object` and the `o_*` helpers,
`o_sym_index` scans the symbol list linearly — object files are small) and
`srcql/codegen.qela` (`push_reloc` on every reference site; the string-header
relocations are collected *after* the pass loop because `reset_code` clears
the relocation lists at every pass boundary). In exe mode a call to a bodyless
extern is a clean compile error. `tests/extern.qela` pins the declare-only
shape, `tests/externvar.qela` the import/export split; the full round trip
(Qela calling C and C calling Qela, strings, interpolation, data and bss
globals, printing, runtime bounds panic) was verified by hand with and
without `-no-pie`. Costs ~+10.6 KB in S2.

## Done

**Implicit integer conversions only widen (2026-08-04).** An implicit cast
between integer types is allowed only when the value cannot change:
widening, and among widening casts only the safe direction — `u8`/`u16`/`u32`
→ `i64` is fine (every value fits), `i32` → `u64` is not (a negative value
sign-extends into a huge positive). Signedness changes at equal width
(`i64` ↔ `u64`), any narrowing, and `u64` mixed into an `i64` expression are
compile errors demanding an explicit `as`. The relief is a constant that
fits: `var c u8 = 'a'`, `arr[i] = 3`, `var b bool = true` stay implicit,
because fitting is provable at compile time (a constant that does not fit is
an error, not a silent wrap). Float literals narrow into `f32` implicitly for
the same reason; int↔float stays fully explicit. The rule lives in `cast_to`
in `srcql/type.qela` and is mirrored byte-for-byte in `src/type.c` (the
bootstrap subset has no float, so stage0 carries only the integer half);
`usual_conv`'s integer promotion to `i64` is unaffected because it widens.
Pinned by `tests/castimplicit.qela` and the reject pair
`tests/castreject_narrow.qela`, `tests/castreject_s2u.qela`,
`tests/castreject_mix.qela`; `boundsloop.qela` and `volatile.qela` gained
explicit casts where a byte read-modify-write used to narrow silently, and
`srcql/opt.qela`'s `trunc_val` now stays in `i64` instead of crossing to
`u64` and back.

**Floats (2026-08-04).** `f32`/`f64` (aliases `float`/`double`): literals
(`1.5`, `2.0`, `5f`, `2.5e-3` — a dot needs a digit on both sides, `f`
suffixes the value, and an exponent keeps a dot-based literal a float; a bare
`1e3` is the integer 1000),
`+ - * /`, unary minus, comparisons and `==`/`!=`, `as` casts in both
directions (float-to-int truncates), parameters and returns, struct fields,
array elements, and interpolation (`"${x}"`). The gate stayed green throughout
and `tests/float.qela` + `tests/floatfmt.qela` (stage1-only) pin the surface.

The implementation deliberately costs the bootstrap nothing. A float is raw
IEEE-754 bits in an ordinary general-purpose register or stack slot; SSE
registers (`movq`/`addsd`/`mulsd`/`ucomisd`...) appear only for the instant of
an operation, so the calling convention and register allocation are untouched.
Parsing (decimal → bits) and printing (bits → decimal) are pure integer
big-decimal arithmetic in `srcql/lex.qela` (`dec_to_f64_bits`) and
`std/fmt.qela` (`fmt_f64_bits`) — stage0 has no float, so the bootstrap subset
stays float-free and `tools/check-subset.sh` is still clean.

Four latent bugs surfaced because code that assumed "an 8-byte value is an
integer" met float bits: `spine_ok` folded float comparisons as integers
(`srcql/opt.qela`), and `gen_into`, `stmt_update` and `gen_compare`
miscopied or mistyped float moves (`srcql/codegen.qela`). All fixed.

Known limits: subnormal literals flush to zero on parse, printing rounds
half up instead of half to even, and there is no comptime float.

**Hardening pass (2026-08-04).** Every fix below is mirrored in stage0 where
the feature exists there, and pinned by a corpus test; the gate stayed green
throughout. Most were found by reading the source, then verified by running
before and after:

- **Comptime calls leaked control-flow state.** `ct_call` never restored
  `ct_returning`/`ct_broke`/`ct_continued`/`ct_ret_val`, so a callee's
  `return` stopped the caller's block at the first statement after the call
  and made the caller return the *callee's* value; a `break` inside a
  comptime loop leaked the same way and killed the rest of the block. A call
  is a boundary now: the flags are saved and restored around it, and a loop
  consumes its own `break` on exit. `tests/comptime.qela` covers all three
  shapes.
- **A call with more than 14 arguments miscompiled.** `gen_args`' placement
  arrays are 14 slots and its overflow scan stopped there, so the 15th+ spill
  never got pushed and the callee read garbage. The 15th parameter is now a
  compile error; 14 are proven to work (`tests/paramlimit15.qela`,
  `tests/paramlimit.qela`).
- **`return`/`break`/`continue` inside a `defer` body recursed forever**:
  codegen runs defer bodies while unwinding, so another return re-entered
  the same defer. Rejected at parse time (the same place the naked-function
  contract lives): `tests/deferreject.qela`, `tests/deferbreak.qela`,
  `tests/defercont.qela`.
- **A comptime block producing a string emitted a garbage pointer** (the
  constant had no slot to carry a string in). Now a clean error:
  `tests/comptimestr.qela`.
- **Constant division by zero folded into garbage** in `opt.qela`'s folder
  and in comptime op-assigns. Both reject it now — and the folder respects
  `&&`/`||` short-circuiting, so `2 > 1 || 1 / 0 == 0` still compiles (the
  right side never runs): `tests/divzero.qela`, `tests/divzeromod.qela`,
  `tests/comptimedivzero.qela`, and `tests/logic.qela` pins the fold order.
- **Fixed-size tables written unchecked**: 64 enum variants
   (stage1 only enforced the cap the C bootstrap already had), 256 mapped
  locals in a monomorphization, 6 type variables, 64 address-taken locals in
  the bounds pass, array lengths that overflowed the size computation. Each
  is a `error_at`/`die` now; array length must be positive and fit in
  `i32`.
- **The lexer read past EOF** for a lone `'` or `'\` at the end of the input,
  and `read_escape` silently accepted unknown escapes (the C bootstrap
  rejects them — the two now agree).
- **The LSP's hand-written JSON parser could spin forever** on a truncated
  string or object; `lsp_publish` dereferenced missing `line`/`col` fields.
  All three are guarded.
- The emitted-size sanity check the agents found (a constant index into a
  *slice* skipping the bounds check) turned out to be a false alarm — slices
  always take the checked path; `tests/sliceconst.qela` pins it.

**Test and tooling growth (same pass).** The corpus grew 66 -> 79 tests:
forward struct declarations, `*=`/`/=`/`%=` op-assigns, and first-ever tests
for `std/list.qela` and `std/map.qela` (previously compiled by nothing — a
module is done when it runs). `tools/bootstrap.sh` gained a compiler-flags
step (`-g` produces a working larger binary, `--backtrace` names frames on a
plain compile, `--no-bounds-checks` lets the out-of-bounds read through,
`--dump-std` prints the embedded source). `tools/torture.py` now generates
division and remainder (with nonzero constant divisors), compound
assignments, a global variable, nested loops, and — via the new
`tests/out/std` link so stage0's file-relative imports resolve — programs
that print to stdout, which the runner compares along with the exit code.

**`std/math.qela` and `std/sort.qela`.** `abs`/`min`/`max` and a Hoare
quicksort over `[]i64`, both written in the bootstrap subset.
`tests/sort.qela` runs under S2.

**First-class functions (2026-08-04).** A function name used as a value is
its address; a variable or parameter may carry it, be assigned and be called
through it: `var cb fn(i64, i64) i64 = add; cb(1, 2)`. The signature is a
type — `fn(t1, t2) ret` — stored as the return type in `base` and the
parameters as a `Member` list, 8 bytes / align 8 like any pointer, compared
structurally in `same_type` and substituted through in `tsubst`, so generic
arguments can be function types. A call through a value resolves the
signature off the variable's type instead of the function table, so it is a
single path, and the address goes in R10 (never an argument register)
*after* the arguments are set up: a call inside the argument list would
clobber R10, and a promoted target can only live in a callee-saved register
(an indirect call makes the function non-leaf, so the caller-saved pool is
out). Functions that use a function value are rejected: the generic — the
table has no monomorphized entry to take an address of — and any parameter
wider than one word (the indirect path is register-only). A function type
takes at most six parameters and cannot return an aggregate wider than 16
bytes. Pinned by `tests/fnptr.qela` (values, reassignment, passing and
returning, six parameters) and `tests/fnptr_sort.qela`, and demonstrated in
`std/sortcmp.qela` — the same quicksort with a caller-supplied comparator.
No closures: only the address is carried, so a `cmp` must be a real
function, not a capture. Stage1-only, like every feature new since the C
bootstrap froze; costs +6 KB in S2.

**Lambdas (2026-08-04).** `fn (x i64) i64 { return x * x; }` is an anonymous
function literal: the parser hoists it to a hidden top-level function named
`_$lambda<N>` (`parse_lambda` in `srcql/parse.qela`, added through
`add_func` so a lambda inside the very first function cannot orphan the
function chain — the main parse loop re-anchors `head.next` on the first
entry now), and the expression's value is the function's address, so a
lambda is just a function value: assign it, pass it inline, return it. Its
return type is inferred — a body pass (`infer_lambda_ret` in
`srcql/type.qela`) collects the `ND_RET`s and checks them against each
other, and `func_type_of`/`type_func` call it before `type_ret`, which
needs a concrete return type. No closures: reading a local of the enclosing
function is a compile-time error, and globals and the lambda's own
parameters are the only names it can reach. `count_reads` now counts a call
through a function-typed variable as a read, so the unused-variable warning
stays honest. Pinned by `tests/lambda.qela` (values, zero/multi-parameter,
returned, inline) and `tests/lambda_capture.qela` (reject). Costs +2.7 KB
in S2.

**Language.** i8–u64, bool, int/uint/usize, `f32`/`f64` (`float`/`double`);
pointers, arrays, slices, `str`; implicit integer casts widen only, and only
in the value-preserving direction (signedness crosses and narrowing need an
explicit `as`, fitting constants stay implicit);
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
drops the check — the most natural loop is also the smallest. A collection
that is not a plain variable is evaluated once into a hidden variable
(gen_for walks the init chain for it); the range form's bound is still
evaluated per iteration.

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
have names without a single new keyword. A value wider than a byte is a
byte string in hex: `asm(0x48c7c0)` emits `48 c7 c0`. `docs/ASM.md` is the
reference: the common encodings as one-`let` rows (mov/add/sub/cmp/jmp/
syscall/iretq/lgdt...), each verified on real hardware in `tests/asmref.qela`.
This is the whole "assembler": bytes with names, no mnemonics, no encoder
in the compiler.

**Entry control, with no OS-specific surface.** Three generic pieces:
top-level `asm { ... }` blocks emit as the image's first bytes (a multiboot
header fits in the file's first 8 KiB); `entry name;` generalizes "main is
the entry" — the ELF entry becomes that function and the default
call-main stub is suppressed; `$name` and `$rel name` inside asm operands
embed a symbol's absolute address or a rel32 slot, patched at finalize by
the same machinery that patches call targets (functions and globals both
resolve). A kernel entry is then just source:
`asm { 0x1badb002, 0, 0xe4524ffe }; entry start; fn naked start() { ... }`
— tested end to end in `tests/topasm.qela` (`$stack_top` for the stack,
`$rel kmain` for the jump). The base address stays 0x400000; low-half
multiboot kernels load there as-is.

**`std/vec.qela`.** A generic growable vector on the arena:
`Vec(T)`, `vec_init`, `vec_push`, `vec_get` (out-of-range reads return a
zeroed element), `vec_pop`, `vec_clear`, and `vec_view` — the slice view
that bridges to the language's checked iteration: `for x in vec_view(&v)`.
Elements are copied on push; the backing array doubles and never frees.

**Two bugs the vector exposed, both fixed in the compiler:** a generic
call typed by a `var` initializer crashed the compiler — the parse loop
rebuilt its function list from a local chain, orphaning the instantiation
and dereferencing a null tail — appending now goes through `funcs_tail`,
so mid-parse instantiations stay linked; and `gen_for` emitted only the
first statement of an init chain, which the for-in desugar (hoisting a
non-variable collection into a hidden variable) needs — it walks the chain
now.

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

**Concurrency.** `spawn f(...)`/`go f(...)`, `coro_yield`, `coro_run_all` on
separate stacks; `Chan(T)`, buffered channels of any element type, and
rendezvous at `chan_init(&ch, 0)` where the sender hands the value across
directly. `ch <- v` and `<-ch` infer that type. Waiting is polling over the
scheduler. A single-scheduler deadlock is when every coroutine that could run
on it is parked inside a wait and no channel has changed since —
`chan_nblocked` counts the parked, `chan_epoch` advances on every channel
state change including rendezvous handoffs, and both staying still long
enough for everyone to have had a turn is reported rather than spun on; see
"Real parallelism" above for real OS threads (`thread_pool_init`/`go` over a
pool, `atomic_*`, `tvar`, GC stop-the-world), where this heuristic turns
itself off because it no longer means anything. The corpus covers
coroutines, channels, the collector, deadlock reporting, a busy-consumer
regression, and the whole threading stack, all under S2.

**Memory.** Arena by default; `std/gc.qela` is a conservative mark-sweep
collector rooted in the callee-saved registers, the stack and the data
segment. **`arena_mark()`/`arena_reset(m)` (2026-08-04)** checkpoint and
rewind the bump pointer: `ArenaMark` carries `{ptr, left}`, so scoped
temporaries (a parse, a request, one loop iteration) free in one call with
no free-list, no bookkeeping per allocation. Safe across a chunk-growth
boundary because `arena_alloc` never returns memory to the OS — the old
chunk stays mapped, so rewinding into it is always valid; it does not zero
on reset, so memory reused past a rewind reads stale bytes. Costs +1152 B
in S2. Pinned by `tests/arenamark.qela`.

**`std/heap.qela` (2026-08-04).** A real malloc/free/realloc, K&R-style: a
circular free list of `Header{size, nextf}` blocks, first-fit search,
neighbor coalescing on free by address adjacency, backed by its own mmap
regions (never shares memory with `std/arena.qela` — the two allocators are
independent). `heap_alloc`/`heap_free`/`heap_realloc`. Unlike the arena,
individual blocks come back and get reused; unlike `std/gc.qela`, nothing
is scanned or reclaimed automatically — a leaked block stays leaked. Not
thread-safe: unlike `std/arena.qela` (locked at its one call site inside
`coro_spawn`) and `std/gc.qela` (its own lock), nothing here guards concurrent
`heap_alloc`/`heap_free` from two OS threads yet — `~` (dynamic typing, the
only current user) is not meant for cross-thread values. Costs +2776 B in S2.
`tests/heap.qela` (stage1-only) covers alloc, free-and-coalesce, and
realloc preserving contents.

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
  library. Comments were instead stripped at the source (2026-08-04): only
  comments the code cannot explain itself kept in `std/*.qela`, saving
  ~1.4 KB in S2 with `--dump-std` still readable. The `--min` indentation
  mangling remains not worth it.

## Rules that keep holding

1. Nothing goes into `src/*.c` except a bug fix backed by a test. A feature
   that is not in stage1 does not exist, because stage0 never ships.
2. No external toolchain in the build path — no assembler, no linker, no
   objcopy. We write the ELF ourselves.
3. Emission follows source order everywhere, or `S2 == S3` breaks.
4. A module counts as done when it compiles *and runs* on a minimal example
   under S2, not when its text is written.
