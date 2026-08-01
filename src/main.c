#include "qela.h"

int qmain(int argc, char **argv) {
	if (argc < 2) {
		eprint("usage: qela <file.qela>\n");
		return 2;
	}
	Str src = read_file(argv[1]);
	out("%c: %d bytes\n", argv[1], src.n);
	return 0;
}
