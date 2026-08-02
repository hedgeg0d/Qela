#include "comp.h"

static void phdr(Buf *f, u32 flags, i64 off, u64 vaddr, i64 filesz, i64 memsz) {
	buf_u32(f, 1);
	buf_u32(f, flags);
	buf_u64(f, (u64)off);
	buf_u64(f, vaddr);
	buf_u64(f, vaddr);
	buf_u64(f, (u64)filesz);
	buf_u64(f, (u64)memsz);
	buf_u64(f, SEG_GAP);
}

void write_elf(const char *path, Image *img) {
	isize text = img->code.n + img->rodata.n;
	isize rw = img->data.n + img->bss_size;
	int   nph = img->nph;
	isize hdr = EHDR_SZ + PHDR_SZ * nph;

	isize data_off = hdr + text;
	u64   data_vaddr = ELF_BASE + SEG_GAP + (u64)data_off;

	Buf f = {0};
	buf_grow(&f, data_off + img->data.n);

	static const u8 ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
	buf_bytes(&f, ident, 16);
	buf_u16(&f, 2);
	buf_u16(&f, 0x3e);
	buf_u32(&f, 1);
	buf_u64(&f, ELF_BASE + (u64)hdr + (u64)img->entry);
	buf_u64(&f, EHDR_SZ);
	buf_u64(&f, 0);
	buf_u32(&f, 0);
	buf_u16(&f, EHDR_SZ);
	buf_u16(&f, PHDR_SZ);
	buf_u16(&f, (u16)nph);
	buf_u16(&f, 0);
	buf_u16(&f, 0);
	buf_u16(&f, 0);

	phdr(&f, 5, 0, ELF_BASE, hdr + text, hdr + text);
	if (nph == 2) phdr(&f, 6, data_off, data_vaddr, img->data.n, rw);

	buf_bytes(&f, img->code.p, img->code.n);
	buf_bytes(&f, img->rodata.p, img->rodata.n);
	buf_bytes(&f, img->data.p, img->data.n);

	write_file(path, (const char *)f.p, f.n, true);
}
