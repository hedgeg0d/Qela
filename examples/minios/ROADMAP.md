# Roadmap

Goal: a demo that sells Qela. A tiny kernel (well under the 1 MiB line
that every shipped artifact must respect) built inside Linux, hosting a
shell, programs, processes, a filesystem, networking — and the Qela
compiler itself, compiling itself inside the OS. Feature bar: wow per
byte, same as the compiler's.

Current state: ~10 KB kernel; boots from QEMU multiboot, ring 3 user
program, keyboard, write/read/exit syscalls (Linux numbers), full cycle
on the serial line: "minios boot", "kernel ready", "hello from
userland", echoed key, "user exited with code 0".

## Stage 1: EXT2 on a RAM disk + syscalls

- Build-time python script writes a 4 MB ext2 image (1 KB blocks) by
  hand — superblock, group descriptors, block/inode bitmaps, inode
  table, directory entries, file blocks. No mkfs dependency; the image
  is embedded in the kernel like user.elf is today.
- Kernel: mount (parse superblock), path lookup (root inode 2, nested
  directories from day one), read file data (direct + single-indirect
  blocks — the compiler sources are ~600 KB, fits one indirect level),
  write (bitmap allocation, inode size update, directory entry create).
- Syscalls added (Linux numbers): open (2), close (3), getdents (217,
  needed for `qela .`), open-with-create (for compiler output). read,
  write, fstat, brk already exist.
- Done when: std/io.qela's file functions (open/read/write/close/read_dir)
  work unmodified against the ext2 image.

## Stage 2: the compiler inside the OS

- First step is an audit: which syscalls does the compiler actually
  use on Linux (read sys.qela; expect mmap/time surprises).
- Embed srcql/ + std/ into the user binary as an LZSS blob (same
  pipeline as std_blob.qela). User stack grows 64 KB -> 4 MB (US bit on
  pd[16..17], rsp moves up).
- Proof: run the compiler on its own embedded sources, emit the ELF to
  a memory buffer, hash it, print SHA-256; it must equal the SHA-256 of
  the real S2 byte-for-byte.
- Done when: `qela .` inside the OS compiles and the hash matches.

## Stage 3: networking

- PCI enumeration (first ~50 lines), e1000 driver (QEMU default nic,
  MMIO, no interrupts needed at first — poll the RX/TX rings).
- ARP + IPv4 + ICMP echo: the host pings the OS — first external wow.
- UDP + Linux-numbered socket syscalls (socket 41, connect 42, sendto
  44, recvfrom 45, bind 49, listen 50): std/net.qela works unmodified,
  programs are portable Linux <-> minios. Demo: UDP echo.
- TCP explicitly out of scope (cost >> wow).

## Stage 4: shell and processes

- User-space shell over ext2: ls/cat/run builtins, keyboard input.
- exec: generalize the existing elf_load into a syscall that loads an
  ELF from the filesystem and runs it.
- "Processes": coroutine tasks with their own stacks (std/coro.qela),
  launched from the shell; cooperative scheduling is fine for the demo.
- Final demo: shell -> run the compiler -> it compiles itself -> the
  hash matches.

## Size and risk notes

- Kernel stays ~20-30 KB of the 1 MiB budget: ext2 ~2-3 KB, network
  ~5-8 KB, shell/processes ~1-2 KB.
- Biggest unknowns: compiler runtime's hidden syscall dependencies
  (stage 2 audit), recursion depth of S2 vs a 4 MB user stack, e1000
  model variance in QEMU (82540EM is the default).
