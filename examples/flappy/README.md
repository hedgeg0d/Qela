# Flappy Bird on Qela + raylib

Flappy Bird written in Qela, rendered with [raylib](https://www.raylib.com/).
SPACE flaps, R restarts, ESC quits. Difficulty grows with survival time:
pipe speed climbs from 150 to 260 px/s and the gap narrows from 170 to 120 px.

## Build and run

```sh
qela -c flappy.qela -o flappy.o
gcc -o flappy flappy.o glue.c -lraylib -lm
./flappy
```

(On Debian/Ubuntu: `apt install libraylib-dev`; on Arch: `pacman -S raylib`.)

## The ABI bridge

Qela's SysV-flavoured calling convention crosses integers, pointers, `bool`
and small structs directly, so most raylib calls are plain `extern fn`
declarations. `str` crosses as a `{ptr, len}` pair, which is why the code
passes `s.ptr as *i8` where raylib wants a `const char*` (string bytes are
NUL-terminated in the object).

Two things do not cross directly, and both are float-shaped:

- **float arguments**: a Qela float is raw bits in an integer register,
  SysV (what raylib expects) wants it in an XMM register;
- **float returns**: same mismatch in the other direction.

`glue.c` bridges exactly the two float-touching calls (`GetFrameTime`,
`DrawCircle`): the float crosses as an integer bit pattern and is
reinterpreted on the C side. Everything else — `DrawRectangle`,
`DrawText`, `ClearBackground`, `Color` by value — is called directly.
The bird physics (gravity, flap, pipe speed, delta time) runs in `f32`.

## Why Qela does not link yet

Qela ships as one self-contained binary and writes its own ELF. In the
ordinary mode it needs no linker at all: the executable is a static image
with raw syscalls, no libc, no relocations. `qela -c` is the other path —
it emits a relocatable object for C interop, and *the system linker
(`gcc`/`ld`) joins that object to C code*. The compiler deliberately has
no linker stage, and this example is exactly why that boundary exists:

- linking against raylib means linking against its dependency chain
  (GLFW, GL, libc), which means implementing a real linker: TLS,
  GOT/PLT, `crt1.o`, archive handling — a second `ld`, not a mini one;
- a linker that only joins Qela objects would not remove gcc from this
  loop (there is C on the other side either way), and `import` already
  merges Qela sources at compile time, so separate compilation buys
  little;
- `extern` data in a `.so` would need GOTPCREL relocations, and binutils
  2.46.1 dies with a BFD internal error on those; calls still work
  through the PLT and data through COPY relocations, but a COPY is a
  per-executable snapshot, so shared mutable data can diverge from the
  library's own copy.

So the philosophy is kept by construction: the compiler is the whole
toolchain *for Qela*, and the moment C enters the picture, the C toolchain
joins too — `gcc` is the linker here, not the compiler.

## The two compiler bugs this example found

Writing this game tripped two real compiler bugs, both fixed in
`srcql/`:

1. an `f32` global initializer stored the *f64* bit pattern in a 4-byte
   slot (the low half of the pattern is zero for any exponent above
   0x3ff, so the global read as `0.0` — no gravity, no pipe movement,
   exactly the "nothing happens" symptom);
2. a negated float literal (`-340.0`) was constant-folded by
   two's-complementing the stored bits instead of flipping the IEEE sign
   bit.

Pinned by `tests/f32global.qela` (`// stage1-only`).
