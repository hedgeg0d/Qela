#include "comp.h"

#define MAX_FILES 64

static const char *paths[MAX_FILES];
static Str         sources[MAX_FILES];
static int         nfiles;

int diag_add_file(const char *path, Str src) {
	if (nfiles == MAX_FILES) die("error: too many imported files\n");
	paths[nfiles] = path;
	sources[nfiles] = src;
	return nfiles++;
}

bool diag_already_imported(const char *path) {
	for (int i = 0; i < nfiles; i++)
		if (str_eq(str_from_cstr(paths[i]), str_from_cstr(path))) return true;
	return false;
}

int diag_file_dir(int file_id, char *buf, isize cap) {
	if (file_id < 0 || file_id >= nfiles || !buf || cap <= 0) return 0;
	Str p = str_from_cstr(paths[file_id]);
	isize i = p.n - 1;
	while (i >= 0 && p.p[i] != '/') i--;
	isize n = (i >= 0) ? i + 1 : 0;
	if (n >= cap) n = cap - 1;
	memcpy(buf, p.p, (usize)n);
	buf[n] = 0;
	return (int)n;
}

int diag_line_for(int file_id, isize off) {
	if (file_id < 0 || file_id >= nfiles) return 1;
	Str src = sources[file_id];
	isize line = 1;
	for (isize i = 0; i < off && i < src.n; i++)
		if (src.p[i] == '\n') line++;
	return (int)line;
}

_Noreturn void error_at(isize pos, const char *fmt, ...) {
	int   file = (int)(pos >> FILE_SHIFT);
	isize off = pos & (FILE_STRIDE - 1);
	if (file < 0 || file >= nfiles) file = 0;

	Str src = sources[file];
	if (off < 0 || off > src.n) off = src.n;

	isize line = 1, start = 0;
	for (isize i = 0; i < off; i++)
		if (src.p[i] == '\n') {
			line++;
			start = i + 1;
		}
	isize end = off;
	while (end < src.n && src.p[end] != '\n') end++;
	isize col = off - start + 1;

	eprint("%c:%d:%d: error: ", paths[file], line, col);
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	vprint(2, fmt, ap);
	__builtin_va_end(ap);
	eprint("\n");

	eprint("  %d | %s\n", line, (Str){src.p + start, end - start});

	isize digits = 0;
	for (isize v = line; v; v /= 10) digits++;
	char  gutter[80];
	isize g = 0;
	for (isize i = 0; i < digits + 3 && g < 70; i++) gutter[g++] = ' ';
	gutter[g++] = '|';
	gutter[g++] = ' ';
	for (isize i = 1; i < col && g < 78; i++)
		gutter[g++] = src.p[start + i - 1] == '\t' ? '\t' : ' ';
	gutter[g++] = '^';
	gutter[g++] = '\n';
	write_all(2, gutter, g);

	sys1(SYS_exit, 1);
	__builtin_unreachable();
}
