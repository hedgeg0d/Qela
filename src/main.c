#include "comp.h"

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
	diag_init(input, src);

	Unit unit = parse(lex(src));
	Image img = codegen(&unit);
	write_elf(output, &img);
	return 0;
}
