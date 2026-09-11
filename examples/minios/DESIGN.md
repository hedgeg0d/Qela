# Design

## Memory map

```
0x000000  .. 0x001000  real mode IVT/BDA (untouched)
0x090000  .. 0x095000  page tables built by the 32-bit stub
                       0x090000 PML4, 0x091000 PDPT, 0x092000 PD[0]
                       identity map of the low 1 GB in 2 MB pages
0x100000               kernel image (--base 0x100000; multiboot loads
                       the ELF segments where they say)
0xB8000                VGA text-mode framebuffer
0x3F8                  COM1
0x1000000              user program (compiled with --base 0x1000000)
0x2000000  .. 0x2010000  user stack (64 KB)
0x2020000              user heap top (brk starts here, no growth yet)
```

Everything above 1 MB up to 1 GB is identity-mapped supervisor memory;
the 2 MB pages holding the user program and its stack get the U bit in
`paging.qela`.

## Boot

`asm { 0x1badb002, 0, 0xe4524ffe }` in `main.qela` is the multiboot
header; QEMU scans the first 8 KiB of the image for it. The ELF entry is
`start`, whose body is one hand-assembled 32-bit stub (all encodings
from `docs/ASM.md`): position-independent via `call/pop ebx`, it zeroes
32 KB at 0x90000, fills PD[0] with 512 x 2 MB entries, links
PML4/PDPT/PD, sets PAE + EFER.LME + CR0.PG, loads a 2-descriptor GDT
embedded in the stub itself, and far-jumps to the 64-bit continuation,
which sets RSP from `$kstack` and calls `kernel_main`.

## Ring 0 policy: interrupts allowed since the isr-frame fix

`fn interrupt` handlers save every register and end in `iretq`. An
interrupt taken at ring 3 arrives with a five-word frame
(RIP/CS/RFLAGS/RSP/SS), but a ring-0 entry pushes only RIP/CS/RFLAGS —
no stack switch happened — and iretq would pop whatever the interrupted
stack held below RFLAGS as RSP/SS when it returns to a less privileged
ring, or #GP on the wrong segment. The compiler's `gen_isr_save` now
tests the CPL of the CS on the entry stack and pads a ring-0 frame up to
the same five words (shifts the three-word frame up 16 bytes and stores
the real RSP and SS below it), so handlers work from either ring.

The kernel therefore runs with IF enabled: FMASK (MSR 0xC0000084) keeps
the syscall entry path non-preemptible, and `sys_read` blocks on
`sti; hlt; cli` until the keyboard or timer IRQ wakes it — the timer
ticks inside ring 0 just as well as it does under user code.

## Syscalls

`syscall_entry` (naked) saves user RSP/RIP/RFLAGS to globals, switches
RSP to the kernel stack, maps Linux registers to SysV (r10 -> rcx for
argument 4, rax -> r9 as the last parameter) and calls `sys_dispatch`
with six parameters. The return value stays in RAX. `sysretq` restores
RSP/RIP/RFLAGS from the globals.

```
dispatch(a0, a1, a2, a3, a4, nr)      Linux: nr in rax
0   read    poll PS/2 into the queue, copy out, -EAGAIN when idle
1   write   fd 1/2 -> VGA + COM1
5   fstat   zero-fills 144 bytes
12  brk     bump-only, rejects shrink
39  getpid  always 1
60  exit    prints the code and halts
231 exit_group  same
else        -ENOSYS (-38)
```

A user program is compiled with `--base 0x1000000`; its compiler-
generated entry stub calls `main`, and `main`'s return goes out through
exit_group (231), the same way it does on Linux. Segment selectors:
kernel CS 0x08 / DS 0x10, user CS 0x20 / SS 0x18, TSS at 0x28 with
RSP0 = kernel stack top. The user data segment sits *below* the user
code on purpose: `sysretq` loads CS = STAR[63:48]+16 and SS =
STAR[63:48]+8 (not CS+8), so the GDT must put the data one index below
the code; `ring3_enter`'s iretq uses the same pair (CS 0x23 / SS 0x1B).

## Bug log

1. ~~`fn interrupt` at ring 0 misbuilds the iretq frame~~ fixed in the
   compiler (`gen_isr_save` pads ring-0 frames with the missing
   RSP/SS pair; see above). The workaround — never enabling IF in
   ring 0 — is gone: `sys_read` blocks on `sti; hlt; cli` and the
   timer ticks in the kernel. (Two compiler bugs were found on the way:
   `jump()` was handed a short-form opcode so the CPL branch emitted
   `GS; DAS` = #UD, and the segment-store modrm used the FS encoding
   instead of SS's — both fixed.)
2. The multiboot header must be 4-byte aligned inside the first 8 KiB
   and the ELF entry must not point at it: the header is a top-level
   `asm` block and `start` follows it.
3. `$bytes` on a missing file is a warning, not an error — the kernel
   checks `ptr == 0` and panics with "user.elf missing: run make".
4. ~~A 32-bit far jump cannot take `$rel`~~ fixed in the compiler:
   `$abs name` embeds a symbol's address in a 4-byte slot, so the stub
   can write `jmp far [ebx+disp]` with the target patched at link time
   instead of computing it from EIP by hand.
5. The GDT in the stub must be referenced with a disp32 modrm: its
   offset from EIP exceeds the disp8 range.
6. `sysretq` loads CS = STAR[63:48]+16 and SS = STAR[63:48]+8 — not
   CS+8 as one would guess. With STAR[63:48]=0x1b the user came back
   with CS=0x2b (the TSS selector), and the first IRQ's iretq died with
   #GP(0x28); with 0x0b, SS=0x13 (the kernel data selector), and the
   iretq died with #GP(0x10). The GDT therefore orders the user
   segments data-below-code (0x18/0x20) and STAR[63:48]=0x13.
7. The PIC mask register is active-low: 1 masks, 0 enables. 0xfe keeps
   only the timer (not the keyboard), 0xfd only the keyboard. The
   original 0xfe "keyboard enabled" reading was backwards, and the
   "spontaneous IRQ1" that killed every run was just the 100 Hz PIT.
8. The 8042 does not raise IRQ1 until bit 0 of its command byte is set:
   `outb(0x64, 0x60); outb(0x60, 0x41)` (bit 6 also turns on scancode
   translation; without it the guest sees set-2 codes and the kb_char
   tables are off).
9. QEMU 11's monitor `sendkey` does not reach the i8042 with
   `-display none` (the status register never shows the OBF bit);
   QMP `input-send-event` with the key qcode does.
10. The original kb_char rows were sequential arithmetic, so "s" came
    back as "b" and "h" as "f". Rows are now string lookups
    ("asdfghjkl" etc.).
