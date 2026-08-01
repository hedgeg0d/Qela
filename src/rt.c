#include "qela.h"

void *memcpy(void *d, const void *s, usize n) {
	u8 *dp = d;
	const u8 *sp = s;
	while (n--) *dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, usize n) {
	u8 *dp = d;
	const u8 *sp = s;
	if (dp < sp) {
		while (n--) *dp++ = *sp++;
	} else {
		dp += n;
		sp += n;
		while (n--) *--dp = *--sp;
	}
	return d;
}

void *memset(void *d, int c, usize n) {
	u8 *dp = d;
	while (n--) *dp++ = (u8)c;
	return d;
}

int memcmp(const void *a, const void *b, usize n) {
	const u8 *x = a, *y = b;
	for (usize i = 0; i < n; i++)
		if (x[i] != y[i]) return (int)x[i] - (int)y[i];
	return 0;
}

#define ARENA_CHUNK (64u << 20)

static u8   *arena_ptr;
static usize arena_left;

void *arena_alloc(usize size, usize align) {
	usize pad = (usize)(-(isize)(usize)arena_ptr) & (align - 1);
	if (arena_left < pad + size) {
		usize want = size + align + ARENA_CHUNK;
		i64 r = sys6(SYS_mmap, 0, (i64)want, PROT_READ | PROT_WRITE,
		             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (r < 0 && r > -4096) die("out of memory\n");
		arena_ptr = (u8 *)r;
		arena_left = want;
		pad = (usize)(-(isize)(usize)arena_ptr) & (align - 1);
	}
	arena_ptr += pad;
	arena_left -= pad;
	void *p = arena_ptr;
	arena_ptr += size;
	arena_left -= size;
	return p;
}

Str str_from_cstr(const char *s) {
	isize n = 0;
	while (s[n]) n++;
	return (Str){s, n};
}

bool str_eq(Str a, Str b) {
	return a.n == b.n && memcmp(a.p, b.p, (usize)a.n) == 0;
}

Str str_dup(Str s) {
	char *p = anew_n(char, s.n + 1);
	memcpy(p, s.p, (usize)s.n);
	p[s.n] = 0;
	return (Str){p, s.n};
}

void write_all(int fd, const char *buf, isize n) {
	while (n > 0) {
		i64 w = sys3(SYS_write, fd, (i64)(usize)buf, n);
		if (w <= 0) return;
		buf += w;
		n -= w;
	}
}

typedef struct {
	int  fd;
	isize n;
	char buf[512];
} Sink;

static void sink_flush(Sink *s) {
	write_all(s->fd, s->buf, s->n);
	s->n = 0;
}

static void sink_ch(Sink *s, char c) {
	if (s->n == (isize)sizeof(s->buf)) sink_flush(s);
	s->buf[s->n++] = c;
}

static void sink_str(Sink *s, Str v) {
	for (isize i = 0; i < v.n; i++) sink_ch(s, v.p[i]);
}

static void sink_u64(Sink *s, u64 v, u64 base) {
	char tmp[24];
	int n = 0;
	do {
		u64 d = v % base;
		tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v);
	while (n) sink_ch(s, tmp[--n]);
}

static void vformat(Sink *s, const char *fmt, __builtin_va_list ap) {
	for (const char *f = fmt; *f; f++) {
		if (*f != '%') {
			sink_ch(s, *f);
			continue;
		}
		switch (*++f) {
		case 's': sink_str(s, __builtin_va_arg(ap, Str)); break;
		case 'c': sink_str(s, str_from_cstr(__builtin_va_arg(ap, const char *))); break;
		case 'd': {
			i64 v = __builtin_va_arg(ap, i64);
			if (v < 0) {
				sink_ch(s, '-');
				sink_u64(s, (u64)-v, 10);
			} else {
				sink_u64(s, (u64)v, 10);
			}
			break;
		}
		case 'u': sink_u64(s, __builtin_va_arg(ap, u64), 10); break;
		case 'x': sink_u64(s, __builtin_va_arg(ap, u64), 16); break;
		case '%': sink_ch(s, '%'); break;
		default:  sink_ch(s, '%'); sink_ch(s, *f); break;
		}
	}
}

void print(int fd, const char *fmt, ...) {
	Sink s = {fd, 0, {0}};
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	vformat(&s, fmt, ap);
	__builtin_va_end(ap);
	sink_flush(&s);
}

void eprint(const char *fmt, ...) {
	Sink s = {2, 0, {0}};
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	vformat(&s, fmt, ap);
	__builtin_va_end(ap);
	sink_flush(&s);
}

_Noreturn void die(const char *fmt, ...) {
	Sink s = {2, 0, {0}};
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	vformat(&s, fmt, ap);
	__builtin_va_end(ap);
	sink_flush(&s);
	sys1(SYS_exit, 1);
	__builtin_unreachable();
}

Str read_file(const char *path) {
	i64 fd = sys3(SYS_open, (i64)(usize)path, O_RDONLY, 0);
	if (fd < 0) die("cannot open %c\n", path);
	i64 size = sys3(SYS_lseek, fd, 0, 2);
	sys3(SYS_lseek, fd, 0, 0);
	char *buf = anew_n(char, size + 1);
	isize got = 0;
	while (got < size) {
		i64 r = sys3(SYS_read, fd, (i64)(usize)(buf + got), size - got);
		if (r <= 0) break;
		got += r;
	}
	sys1(SYS_close, fd);
	buf[got] = 0;
	return (Str){buf, got};
}

void write_file(const char *path, const char *buf, isize n, bool need_exec) {
	sys1(SYS_unlink, (i64)(usize)path);
	i64 fd = sys3(SYS_open, (i64)(usize)path, O_WRONLY | O_CREAT | O_TRUNC,
	              need_exec ? 0755 : 0644);
	if (fd < 0) die("cannot write %c\n", path);
	write_all((int)fd, buf, n);
	sys1(SYS_close, fd);
}

void buf_grow(Buf *b, isize need) {
	if (b->n + need <= b->cap) return;
	isize cap = b->cap ? b->cap * 2 : 4096;
	while (cap < b->n + need) cap *= 2;
	u8 *p = anew_n(u8, cap);
	memcpy(p, b->p, (usize)b->n);
	b->p = p;
	b->cap = cap;
}

void buf_u8(Buf *b, u8 v) {
	buf_grow(b, 1);
	b->p[b->n++] = v;
}

void buf_u16(Buf *b, u16 v) {
	buf_grow(b, 2);
	for (int i = 0; i < 2; i++) b->p[b->n++] = (u8)(v >> (i * 8));
}

void buf_u32(Buf *b, u32 v) {
	buf_grow(b, 4);
	for (int i = 0; i < 4; i++) b->p[b->n++] = (u8)(v >> (i * 8));
}

void buf_u64(Buf *b, u64 v) {
	buf_grow(b, 8);
	for (int i = 0; i < 8; i++) b->p[b->n++] = (u8)(v >> (i * 8));
}

void buf_bytes(Buf *b, const void *p, isize n) {
	buf_grow(b, n);
	memcpy(b->p + b->n, p, (usize)n);
	b->n += n;
}

void buf_patch32(Buf *b, isize at, u32 v) {
	for (int i = 0; i < 4; i++) b->p[at + i] = (u8)(v >> (i * 8));
}
