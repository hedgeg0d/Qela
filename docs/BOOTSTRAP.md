# The bootstrap subset

This file is the single source of truth for what stage1 sources may use.
It outranks convenience: breaking a rule here breaks self-hosting.

## Why

The compiler exists in three roles:

| | What | Built by | Shipped? |
|---|---|---|---|
| **stage0** | `src/*.c`, 46 KiB | gcc | no, one-shot |
| **stage1** | `srcql/*.qela` + `std/*.qela` | stage0, then itself | as **S2** |
| **S2** | stage1 compiled by itself | S1a | **yes** |

Everything else follows from one rule:

> **Every construct used by stage1 *sources* must be implemented twice: in
> stage0 (C) and in stage1 (Qela).**

So the subset is narrow. It is orthogonal to what stage1 offers its users:
stage1 implements the **whole** language — generics, comptime, defer, match —
but does not **use** any of it in its own sources. A bug in comptime then
cannot break the bootstrap.

stage0 is **frozen**. Changes to `src/*.c` are bug fixes backed by a test, never
new language features.

## Allowed

Top level:

```qela
import "path/file.qela"        // relative to the importing file
struct Name;                   // forward declaration for recursive types
struct Name { f T, g U, }      // trailing comma required
enum E { A, B(int), C(int, int), }
fn name(a T, b U) R { ... }    // at most 6 parameters; R defaults to void
var g T;                       // global, zeroed
var g T = <constant>;          // scalars only
let K = <constant>;            // type inferred; an explicit type is rejected
```

Types:

- `void`, `bool`, `i8 i16 i32 i64`, `u8 u16 u32 u64`, `int`(=`i64`), `uint`, `usize`
- `*T`, `&x`, `*p`, `p[i]`
- `[N]T` where N is a literal, `[]T`, `str` = `[]u8`, fields `.ptr` and `.len`
- `struct` (nesting allowed), `enum` with payload
- no function pointers — dispatch with `if` chains
- no floating point anywhere in the language

Expressions:

```
+ - * / %            == != < <= > >=        && || !
& | ^ ~ << >>        += -= *= /= %= &= |= ^= <<= >>=
x as T               sizeof(T)              a[lo..hi]
Point{x: 1, y: 2}    E.Variant(v)           'a' '\n' '\t' '\\' '\'' '\0'
true false           "text"                 0x1F
```

Struct literal fields may come in any order; omitted ones are zeroed.
A character literal is an ordinary integer constant.

Control flow:

```qela
if (c) { } else { }
if (c) { } else if (d) { }
while (c) { }
for (init; cond; step) { }
for i in lo..hi { }
break;  continue;  return e;  return;
```

There is no `switch`. Block scoping works; there are no nested functions.

## Forbidden in stage1 sources

| Construct | Reason |
|---|---|
| `comptime`, generics | keep them off the bootstrap-critical path; stage1 implements them without depending on them |
| `match` | would force the AST to be modelled as enums; use an `int` kind and `if` chains |
| `defer` | nothing to unwind, all memory comes from the arena |
| `syscall(...)` outside `std/` | system calls go through `std/sys.qela` |
| global aggregates with an initializer | stage0 cannot emit them; fill tables at startup |
| more than 6 parameters | pass a context struct by pointer |

`tools/check-subset.sh` enforces this.

## Determinism

The self-hosting gate is `S2` and `S3` being byte-identical. That only holds if
output depends on nothing but the source:

1. Walk the AST through `next` links only, never through node addresses and
   never through arena order.
2. No hash-table iteration on any path that reaches the output. Functions,
   globals and string literals appear in source order.
3. Arena offsets and `mmap` addresses must not leak into emitted bytes —
   monomorphization suffixes use counters, not addresses.
4. No timestamps, build paths, `getpid`, or directory order.
5. Same input, same output, at any arena size.

## Module layout

```
std/arena.qela     mmap + bump allocator
std/buf.qela       growable byte buffer
std/str.qela       string compare, search, slice
std/fmt.qela       formatted output for diagnostics
std/list.qela      growable pointer array
std/map.qela       str -> pointer hash table, deterministic iteration

srcql/comp.qela    types and the module contract, the only shared header
srcql/diag.qela    files, positions, error_at with a caret
srcql/lex.qela     tokenizer
srcql/parse.qela   recursive descent to AST
srcql/type.qela    name resolution, type checking, monomorphization
srcql/ir.qela      SSA-lite IR (phase D, after the bootstrap)
srcql/opt.qela     mem2reg, constant propagation, DCE (phase D)
srcql/codegen.qela x86-64 emission
srcql/elf.qela     ELF64 writer
srcql/main.qela    CLI
```

`srcql/comp.qela` holds definitions only; implementations live in their own
files. Changing it changes the contract and is agreed separately.

## The gate

```
S1a = stage0(srcql/main.qela)
S2  = S1a(srcql/main.qela)
S3  = S2(srcql/main.qela)
cmp S2 S3
```

Run `tools/bootstrap.sh`. S2 is what ships.

A mismatch means either non-determinism, or stage0 and stage1 disagreeing on
semantics: S1a comes from stage0 while S2 comes from stage1 itself, so a
behavioural difference surfaces exactly here.
