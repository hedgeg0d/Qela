#include "comp.h"

#define BASE     0x400000
#define EHDR_SZ  64
#define PHDR_SZ  56
#define HDR_SZ   (EHDR_SZ + PHDR_SZ)

void write_elf(const char *path, Image *img) {
	i64 total = HDR_SZ + img->code.n + img->rodata.n;
	Buf f = {0};
	buf_grow(&f, total);

	static const u8 ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
	buf_bytes(&f, ident, 16);
	buf_u16(&f, 2);
	buf_u16(&f, 0x3e);
	buf_u32(&f, 1);
	buf_u64(&f, BASE + HDR_SZ + (u64)img->entry);
	buf_u64(&f, EHDR_SZ);
	buf_u64(&f, 0);
	buf_u32(&f, 0);
	buf_u16(&f, EHDR_SZ);
	buf_u16(&f, PHDR_SZ);
	buf_u16(&f, 1);
	buf_u16(&f, 0);
	buf_u16(&f, 0);
	buf_u16(&f, 0);

	buf_u32(&f, 1);
	buf_u32(&f, 5);
	buf_u64(&f, 0);
	buf_u64(&f, BASE);
	buf_u64(&f, BASE);
	buf_u64(&f, (u64)total);
	buf_u64(&f, (u64)total);
	buf_u64(&f, 0x1000);

	buf_bytes(&f, img->code.p, img->code.n);
	buf_bytes(&f, img->rodata.p, img->rodata.n);

	write_file(path, (const char *)f.p, f.n, true);
}
