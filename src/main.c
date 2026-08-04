#include "comp.h"

static char module_dir[4096];
static int  module_dir_len;

void set_module_dir(const char *path) {
	module_dir_len = 0;
	if (!path) return;
	Str s = str_from_cstr(path);
	isize i = s.n - 1;
	while (i >= 0 && s.p[i] != '/') i--;
	if (i >= 0) {
		module_dir_len = (int)(i + 1);
		if (module_dir_len >= (int)sizeof(module_dir))
			module_dir_len = (int)sizeof(module_dir) - 1;
		memcpy(module_dir, s.p, (usize)module_dir_len);
	}
	module_dir[module_dir_len] = 0;
}

static const char *default_output(const char *in) {
	Str s = str_from_cstr(in);
	isize end = s.n;
	if (end > 5 && memcmp(s.p + end - 5, ".qela", 5) == 0) end -= 5;
	isize start = 0;
	for (isize i = 0; i < end; i++)
		if (s.p[i] == '/') start = i + 1;
	if (start == end) return "a.out";
	char *p = anew_n(char, end - start + 1);
	memcpy(p, s.p + start, (usize)(end - start));
	p[end - start] = 0;
	return p;
}

int qmain(int argc, char **argv) {
	const char *input = NULL;
	const char *output = NULL;

	for (int i = 1; i < argc; i++) {
		Str a = str_from_cstr(argv[i]);
		if (str_eq(a, S("-o"))) {
			if (++i == argc) die("error: -o needs an argument\n");
			output = argv[i];
		} else if (str_eq(a, S("--no-bounds-checks"))) {
			opt_no_bounds = true;
		} else if (a.n > 0 && a.p[0] == '-') {
			die("error: unknown option %c\n", argv[i]);
		} else if (!input) {
			input = argv[i];
		} else {
			die("error: more than one input file\n");
		}
	}

	if (!input) {
		eprint("usage: qela <file.qela> [-o output]\n");
		return 2;
	}
	if (!output) output = default_output(input);

	Str src = read_file(input);
	int file = diag_add_file(input, src);
	set_module_dir(input);

	Unit unit = parse(lex(src, (isize)file << FILE_SHIFT));
	Image img = codegen(&unit);
	write_elf(output, &img);
	return 0;
}
