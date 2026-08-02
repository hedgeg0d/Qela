#include "comp.h"

static const char *src_path;
static Str         src_text;

void diag_init(const char *path, Str src) {
	src_path = path;
	src_text = src;
}

_Noreturn void error_at(isize pos, const char *fmt, ...) {
	if (pos < 0 || pos > src_text.n) pos = src_text.n;

	isize line = 1, start = 0;
	for (isize i = 0; i < pos; i++)
		if (src_text.p[i] == '\n') {
			line++;
			start = i + 1;
		}
	isize end = pos;
	while (end < src_text.n && src_text.p[end] != '\n') end++;
	isize col = pos - start + 1;

	eprint("%c:%d:%d: error: ", src_path, line, col);
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	{
		char buf[512];
		isize n = 0;
		for (const char *f = fmt; *f && n < (isize)sizeof(buf) - 1; f++) {
			if (*f != '%') {
				buf[n++] = *f;
				continue;
			}
			Str s;
			switch (*++f) {
			case 's': s = __builtin_va_arg(ap, Str); break;
			case 'c': s = str_from_cstr(__builtin_va_arg(ap, const char *)); break;
			default:  s = S("?"); break;
			}
			for (isize i = 0; i < s.n && n < (isize)sizeof(buf) - 1; i++)
				buf[n++] = s.p[i];
		}
		write_all(2, buf, n);
	}
	__builtin_va_end(ap);
	eprint("\n");

	eprint("  %d | %s\n", line, (Str){src_text.p + start, end - start});

	isize digits = 0;
	for (isize v = line; v; v /= 10) digits++;
	char gutter[80];
	isize g = 0;
	for (isize i = 0; i < digits + 3 && g < 70; i++) gutter[g++] = ' ';
	gutter[g++] = '|';
	gutter[g++] = ' ';
	for (isize i = 1; i < col && g < 78; i++)
		gutter[g++] = src_text.p[start + i - 1] == '\t' ? '\t' : ' ';
	gutter[g++] = '^';
	gutter[g++] = '\n';
	write_all(2, gutter, g);

	sys1(SYS_exit, 1);
	__builtin_unreachable();
}
