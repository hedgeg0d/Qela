#ifndef QELA_H
#define QELA_H

#include "sys.h"

// Emitted implicitly by the compiler for struct assignment and array init,
// even under -ffreestanding.
void *memcpy(void *d, const void *s, usize n);
void *memmove(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);
int memcmp(const void *a, const void *b, usize n);

void *arena_alloc(usize size, usize align);
#define anew(T)       ((T *)arena_alloc(sizeof(T), _Alignof(T)))
#define anew_n(T, n)  ((T *)arena_alloc(sizeof(T) * (usize)(n), _Alignof(T)))

typedef struct {
	const char *p;
	isize       n;
} Str;

#define S(lit) ((Str){(lit), (isize)(sizeof(lit) - 1)})

Str  str_from_cstr(const char *s);
bool str_eq(Str a, Str b);
Str  str_dup(Str s);

void write_all(int fd, const char *buf, isize n);

// Supports %s (Str), %c (const char *), %d, %u, %x, %%.
void print(int fd, const char *fmt, ...);
#define out(...) print(1, __VA_ARGS__)
void eprint(const char *fmt, ...);
_Noreturn void die(const char *fmt, ...);

Str  read_file(const char *path);
void write_file(const char *path, const char *buf, isize n, bool need_exec);

typedef struct {
	u8   *p;
	isize n;
	isize cap;
} Buf;

void buf_grow(Buf *b, isize need);
void buf_u8(Buf *b, u8 v);
void buf_u16(Buf *b, u16 v);
void buf_u32(Buf *b, u32 v);
void buf_u64(Buf *b, u64 v);
void buf_bytes(Buf *b, const void *p, isize n);
void buf_patch32(Buf *b, isize at, u32 v);

#endif
