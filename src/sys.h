#ifndef QELA_SYS_H
#define QELA_SYS_H

typedef signed char        i8;
typedef unsigned char      u8;
typedef short              i16;
typedef unsigned short     u16;
typedef int                i32;
typedef unsigned int       u32;
typedef long               i64;
typedef unsigned long      u64;
typedef unsigned long      usize;
typedef long               isize;

#define NULL ((void *)0)

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_fstat      5
#define SYS_lseek      8
#define SYS_mmap       9
#define SYS_exit      60
#define SYS_ftruncate 77
#define SYS_unlink    87

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0x4000

static inline i64 sys1(i64 n, i64 a) {
	i64 r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
	return r;
}
static inline i64 sys2(i64 n, i64 a, i64 b) {
	i64 r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
	return r;
}
static inline i64 sys3(i64 n, i64 a, i64 b, i64 c) {
	i64 r;
	__asm__ volatile("syscall"
	                 : "=a"(r)
	                 : "a"(n), "D"(a), "S"(b), "d"(c)
	                 : "rcx", "r11", "memory");
	return r;
}
static inline i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
	register i64 r10 __asm__("r10") = d;
	register i64 r8 __asm__("r8") = e;
	register i64 r9 __asm__("r9") = f;
	i64 r;
	__asm__ volatile("syscall"
	                 : "=a"(r)
	                 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
	                 : "rcx", "r11", "memory");
	return r;
}

#endif
