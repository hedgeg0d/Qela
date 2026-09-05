# Writing raw x86-64 in Qela

`asm(byte, ...)` splats bytes into the code. An operand may be any comptime
constant, and a value wider than a byte is a byte string written in hex:
`asm(0x48c7c0)` emits `48 c7 c0`, most significant byte first — the order a
disassembler prints. So every row in the tables below is one `let`:

```qela
let MOV_RAX_IMM32 = 0x48c7c0;   // mov rax, imm32
asm(MOV_RAX_IMM32, 42, 0, 0, 0);
```

Immediates are separate operands, little-endian on the wire as usual.

## Registers

modrm bytes below are written for rax. The register field is
`0xc0 | (reg << 3) | rm` with rax..rdi = 0..7 and r8..r15 = 8..15
(the rex prefix is the `48` already in front):

| reg | modrm | | reg | modrm |
|---|---|---|---|---|
| rax | c0 | | r8 | 48 c0 |
| rcx | c8 | | r9 | 48 c8 |
| rdx | d0 | | r10 | 48 d0 |
| rbx | d8 | | r11 | 48 d8 |
| rsp | e0 | | r12 | 48 e0 |
| rbp | e8 | | r13 | 48 e8 |
| rsi | f0 | | r14 | 48 f0 |
| rdi | f8 | | r15 | 48 f8 |

## Control flow

| asm | bytes | notes |
|---|---|---|
| nop | `0x90` | |
| ret | `0xc3` | |
| int3 | `0xcc` | breakpoint |
| syscall | `0x0f, 0x05` | rax = nr, rdi..r10 = args |
| iretq | `0x48, 0xcf` | ISR return |
| hlt | `0xf4` | |
| cli / sti | `0xfa` / `0xfb` | |
| cld / std | `0xfc` / `0xfd` | |
| pause | `0xf3, 0x90` | |
| int imm8 | `0xcd, imm8` | |
| jmp rel32 | `0xe9, imm32` | rel32 |
| jmp rel8 | `0xeb, imm8` | rel8 |
| jmp rax | `0xff, 0xe0` | |
| call rel32 | `0xe8, imm32` | rel32 |
| call rax | `0xff, 0xd0` | |

## Data movement

| asm | bytes | notes |
|---|---|---|
| mov rax, imm64 | `0x48, 0xb8, imm64` | movabs |
| mov rax, imm32 | `0x48, 0xc7, 0xc0, imm32` | zero-extends |
| mov rax, rbx | `0x48, 0x89, modrm` | r/m <- reg |
| mov rax, [rbx] | `0x48, 0x8b, modrm` | reg <- mem, modrm = 03 for [rbx] |
| mov [rbx], rax | `0x48, 0x89, modrm` | mem <- reg |
| lea rax, [rax+imm8] | `0x48, 0x8d, 0x80, imm8` | |
| push rax | `0x50` | 50+r |
| pop rax | `0x58` | 58+r |
| push r8 | `0x41, 0x50` | 41 50+(r-8) |
| movzx rax, byte [rax] | `0x48, 0x0f, 0xb6, 0x00` | |
| movsx rax, byte [rax] | `0x48, 0x0f, 0xbe, 0x00` | |
| movsxd rax, dword [rax] | `0x48, 0x63, 0x00` | |
| movsb / movsw / movsq | `0xa4` / `0x66, 0xa5` / `0x48, 0xa5` | with `0xf3` rep prefix |
| stosb | `0xaa` | rep stosb = `0xf3, 0xaa` |

## Arithmetic

| asm | bytes | notes |
|---|---|---|
| add rax, imm8 | `0x48, 0x83, 0xc0, imm8` | |
| add rax, imm32 | `0x48, 0x81, 0xc0, imm32` | |
| sub rax, imm8 | `0x48, 0x83, 0xe8, imm8` | |
| cmp rax, imm8 | `0x48, 0x83, 0xf8, imm8` | |
| cmp rax, imm32 | `0x48, 0x81, 0xf8, imm32` | |
| cmp rax, rbx | `0x48, 0x3b, modrm` | |
| test rax, rax | `0x48, 0x85, 0xc0` | |
| test rax, imm8 | `0x48, 0xf7, 0xc0, imm8` | |
| and / or / xor rax, imm8 | `0x48, 0x83, 0xe0 / 0xc8 / 0xf0, imm8` | |
| xor rax, rax | `0x48, 0x31, 0xc0` | the classic zero |
| and / or rax, rbx | `0x48, 0x21, modrm` / `0x48, 0x09, modrm` | |
| inc rax / dec rax | `0x48, 0xff, 0xc0` / `0x48, 0xff, 0xc8` | |
| neg rax / not rax | `0x48, 0xf7, 0xd8` / `0x48, 0xf7, 0xd0` | |
| shl rax, imm8 | `0x48, 0xc1, 0xe0, imm8` | shr = e8, sar = f8 |
| shl rax, cl | `0x48, 0xd3, 0xe0` | |
| imul rax, rbx | `0x48, 0x0f, 0xaf, modrm` | |
| div rax / idiv rax | `0x48, 0xf7, 0xf0` / `0x48, 0xf7, 0xf8` | rdx:rax / rax |

## System (kernel-only)

| asm | bytes | notes |
|---|---|---|
| rdmsr / wrmsr | `0x0f, 0x32` / `0x0f, 0x30` | ecx = msr, edx:eax |
| cpuid | `0x0f, 0xa2` | eax in, eax..edx out |
| rdtsc | `0x0f, 0x31` | edx:eax |
| mov cr0, rax | `0x0f, 0x22, 0xc0` | |
| mov rax, cr0 | `0x0f, 0x20, 0xc0` | |
| lgdt [rax] | `0x0f, 0x01, 0x10` | |
| lidt [rax] | `0x0f, 0x01, 0x18` | |
| ltr rax | `0x0f, 0x00, 0xd8` | |
| swapgs | `0x0f, 0x01, 0xf8` | |
| in al, dx | `0xec` | in eax, dx = `0xed` |
| out dx, al | `0xee` | out dx, eax = `0xef` |

`tests/asmref.qela` exercises most of the user-space rows on real hardware.
## The image entry

```qela
asm { 0x1badb002, 0, 0xe4524ffe };   // multiboot header, first bytes of the file

var stack_top [65536]u8;

entry start;                          // the ELF entry; suppresses call-main stub

fn naked start() {
	asm(0x48, 0xbc, $stack_top);      // mov rsp, stack_top
	asm(0xe9, $rel qela_main);        // jmp qela_main
}
```

`$name` embeds an 8-byte absolute address (the `movabs`/`mov r64, imm64`
operand); `$rel name` embeds a rel32 slot (the `jmp`/`call` operand). Both
patch at link time, functions and globals alike. `$abs name` is the same
absolute address in a **4-byte** slot, for 32-bit code where the 8-byte
form would spill into the following instructions (a far-jump target, for
example). Multi-byte constants are byte strings in print order: a 32-bit
little-endian value like the multiboot magic is written as its byte-reversed
hex (`0x02b0ad1b`). When a little-endian value of a fixed width is wanted
instead — a multiboot header field, an `imm32` operand — write
`$le N v`, which emits exactly N bytes (2/4/8), least significant byte
first, with no leading-zero truncation: `asm(0xb8, $le 4 0x00010000)`
is `b8 00 00 01 00`.
