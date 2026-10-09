# minios

A minimal x86_64 kernel written in Qela: boots from a multiboot loader
(QEMU's `-kernel`), switches to long mode from raw 32-bit asm, sets up
paging/GDT/IDT/PIC/PIT, drives VGA text mode and COM1 for output and the
PS/2 keyboard for input, and runs user programs at ring 3 where syscalls
carry the same numbers Linux uses.

```
examples/
  minios/        the kernel (qela .)
  minios_user/   the user-space program (compiled separately)
```

## Build

```sh
make            # builds user.elf, then the kernel (qela . -o kernel.elf)
qela .          # kernel only; user.elf must already exist (embedded via $bytes)
```

The kernel embeds the user binary at compile time (`$bytes("user.elf")` in
`elf.qela`), so there is no filesystem and no loader handoff: the sequence
is always `make`, then run.

## Run

```sh
qemu-system-x86_64 -kernel examples/minios/kernel.elf \
  -serial stdio -display none -no-reboot
```

VGA and COM1 mirror each other, so the serial line shows everything the
screen does. The user program prints a line, waits for keyboard input,
echoes it and exits. QEMU's monitor `sendkey` does not deliver keys
with `-display none`, so drive the keyboard over QMP instead:

```sh
# from another shell, with -qmp unix:/tmp/qmp,server,nowait added to qemu:
python3 - <<'EOF'
import socket, json, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/qmp')
def recv():
    b = b''
    while True:
        b += s.recv(65536)
        try: return json.loads(b)
        except: pass
recv()
def cmd(n, **k):
    s.sendall((json.dumps({'execute': n, 'arguments': k}) + '\n').encode()); return recv()
cmd('qmp_capabilities')
for k in ['h', 'ret']:
    cmd('input-send-event', events=[{'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': k}}}])
    cmd('input-send-event', events=[{'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': k}}}])
    time.sleep(0.2)
time.sleep(1)
EOF
```

## What runs

Boot order, from `main.qela`'s naked entry:

1. the first 32 bytes are a multiboot header, followed by a hand-written
   32-bit stub: it fills identity-mapped 2 MB pages at 0x90000, enables
   PAE/LME/paging, loads a minimal GDT and far-jumps into 64-bit code;
2. `kernel_main` sets up the real GDT (user segments, TSS), the IDT with
   16 exception handlers plus timer/keyboard IRQs, remaps the PIC,
   arms the PIT at 100 Hz, initializes the PS/2 controller, installs the
   syscall MSRs (STAR/LSTAR/FMASK) and flips the user bit on the user
   pages;
3. `elf_load` parses the embedded user ELF, copies its PT_LOAD segments
   and zeroes the BSS;
4. `ring3_enter` iretq's into the user program at ring 3.

The syscall entry masks IF via FMASK (the entry path is brief and
non-preemptible), but the kernel itself runs with interrupts enabled:
`sys_read` blocks on `sti; hlt; cli` until a keyboard or timer IRQ
wakes it — ring-0 entries work since the compiler's `gen_isr_save`
pads their interrupt frames (see DESIGN.md).

## Status

- boots to long mode, prints, runs ring-3 code, handles write/read/exit
  syscalls, echoes keyboard input (PS/2, US layout, shift works, no
  caps/ctrl/alt); `sys_read` blocks with interrupts enabled at ring 0;
- syscalls implemented (Linux numbers): read 0, write 1, fstat 5,
  brk 12, getpid 39, exit 60, exit_group 231. Everything else returns
  -ENOSYS (-38);
- the user program is written in Qela with raw `syscall` instructions;
  a gcc-compiled static ELF needs a few more syscalls (ioctl, mmap,
  rt_sigprocmask...) and is the next step;
- ring-0-only quirks of the compiler are documented in DESIGN.md.
