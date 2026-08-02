#include "comp.h"

#define RAX 0
#define RCX 1
#define RDX 2
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7
#define R8  8
#define R9  9
#define R10 10

static const int call_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
static const int sys_regs[6]  = {RDI, RSI, RDX, R10, R8, R9};

typedef struct {
	isize at;
	Str   name;
	isize pos;
} CallFixup;

typedef struct {
	isize at;
	int   id;
} StrFixup;

static Buf code;
static Buf rodata;
static int depth;
static Func *all_funcs;

static CallFixup *calls;
static int ncalls, calls_cap;
static StrFixup  *strrefs;
static int nstrrefs, strrefs_cap;
static isize *str_off;

static void b1(u8 v) { buf_u8(&code, v); }

static void b4(u32 v) { buf_u32(&code, v); }

static void rex_w(int reg, int rm) {
	b1((u8)(0x48 | ((reg >= 8) << 2) | (rm >= 8)));
}

static void modrm(int mod, int reg, int rm) {
	b1((u8)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

static void push_reg(int r) {
	if (r >= 8) b1(0x41);
	b1((u8)(0x50 + (r & 7)));
	depth++;
}

static void pop_reg(int r) {
	if (r >= 8) b1(0x41);
	b1((u8)(0x58 + (r & 7)));
	depth--;
}

static void mov_reg_imm(int r, i64 v) {
	if (v == 0) {
		if (r >= 8) b1(0x45);
		b1(0x31);
		modrm(3, r, r);
		return;
	}
	if (v > 0 && v <= 0xffffffffLL) {
		if (r >= 8) b1(0x41);
		b1((u8)(0xb8 + (r & 7)));
		b4((u32)v);
		return;
	}
	if (v >= -2147483648LL && v <= 2147483647LL) {
		rex_w(0, r);
		b1(0xc7);
		modrm(3, 0, r);
		b4((u32)v);
		return;
	}
	rex_w(0, r);
	b1((u8)(0xb8 + (r & 7)));
	buf_u64(&code, (u64)v);
}

static void mov_reg_reg(int dst, int src) {
	if (dst == src) return;
	rex_w(src, dst);
	b1(0x89);
	modrm(3, src, dst);
}

static void mem_rbp(int op, int reg, int off) {
	rex_w(reg, RBP);
	b1((u8)op);
	if (off >= -128 && off <= 127) {
		modrm(1, reg, RBP);
		b1((u8)off);
	} else {
		modrm(2, reg, RBP);
		b4((u32)off);
	}
}

static void load_local(int reg, int off) { mem_rbp(0x8b, reg, -off); }
static void store_local(int reg, int off) { mem_rbp(0x89, reg, -off); }

static void alu_rax_rdi(u8 op) {
	rex_w(RDI, RAX);
	b1(op);
	modrm(3, RDI, RAX);
}

static void setcc(u8 cc) {
	b1(0x0f);
	b1(cc);
	modrm(3, 0, RAX);
	b1(0x48);
	b1(0x0f);
	b1(0xb6);
	modrm(3, RAX, RAX);
}

static void test_rax(void) {
	b1(0x48);
	b1(0x85);
	modrm(3, RAX, RAX);
}

static isize jump(u8 opcode2) {
	if (opcode2) {
		b1(0x0f);
		b1(opcode2);
	} else {
		b1(0xe9);
	}
	isize at = code.n;
	b4(0);
	return at;
}

static void patch_here(isize at) {
	buf_patch32(&code, at, (u32)(code.n - (at + 4)));
}

static void add_call(Str name, isize pos) {
	if (ncalls == calls_cap) {
		int cap = calls_cap ? calls_cap * 2 : 32;
		CallFixup *p = anew_n(CallFixup, cap);
		memcpy(p, calls, sizeof(CallFixup) * (usize)ncalls);
		calls = p;
		calls_cap = cap;
	}
	b1(0xe8);
	calls[ncalls++] = (CallFixup){code.n, name, pos};
	b4(0);
}

static void add_strref(int id) {
	if (nstrrefs == strrefs_cap) {
		int cap = strrefs_cap ? strrefs_cap * 2 : 32;
		StrFixup *p = anew_n(StrFixup, cap);
		memcpy(p, strrefs, sizeof(StrFixup) * (usize)nstrrefs);
		strrefs = p;
		strrefs_cap = cap;
	}
	b1(0x48);
	b1(0x8d);
	modrm(0, RAX, RBP);
	strrefs[nstrrefs++] = (StrFixup){code.n, id};
	b4(0);
}

static void gen_expr(Node *n);

static void gen_cstrlen(void) {
	mov_reg_reg(RDX, RAX);
	isize top = code.n;
	b1(0x80);
	modrm(0, 7, RAX);
	b1(0x00);
	b1(0x74);
	isize je_at = code.n;
	b1(0);
	b1(0x48);
	b1(0xff);
	modrm(3, 0, RAX);
	b1(0xeb);
	b1((u8)(top - (code.n + 1)));
	code.p[je_at] = (u8)(code.n - (je_at + 1));
	rex_w(RDX, RAX);
	b1(0x29);
	modrm(3, RDX, RAX);
}

static void gen_args(Node *n, const int *regs, bool first_in_rax) {
	for (Node *a = n->args; a; a = a->next) {
		gen_expr(a);
		push_reg(RAX);
	}
	int i = n->nargs;
	while (i-- > 0) {
		if (first_in_rax && i == 0) pop_reg(RAX);
		else pop_reg(regs[i - (first_in_rax ? 1 : 0)]);
	}
}

static void gen_expr(Node *n) {
	switch (n->kind) {
	case ND_NUM:
		mov_reg_imm(RAX, n->val);
		return;
	case ND_STRLIT:
		add_strref((int)n->val);
		return;
	case ND_VAR:
		load_local(RAX, n->var->offset);
		return;
	case ND_ASSIGN:
		gen_expr(n->rhs);
		store_local(RAX, n->lhs->var->offset);
		return;
	case ND_NEG:
		gen_expr(n->lhs);
		b1(0x48);
		b1(0xf7);
		modrm(3, 3, RAX);
		return;
	case ND_NOT:
		gen_expr(n->lhs);
		test_rax();
		setcc(0x94);
		return;
	case ND_CSTRLEN:
		gen_expr(n->args);
		gen_cstrlen();
		return;
	case ND_SYSCALL:
		gen_args(n, sys_regs, true);
		b1(0x0f);
		b1(0x05);
		return;
	case ND_CALL: {
		if (n->nargs > 6) error_at(n->pos, "at most 6 arguments are supported");
		gen_args(n, call_regs, false);
		bool odd = depth & 1;
		if (odd) {
			b1(0x48);
			b1(0x83);
			modrm(3, 5, RSP);
			b1(8);
		}
		add_call(n->name, n->pos);
		if (odd) {
			b1(0x48);
			b1(0x83);
			modrm(3, 0, RSP);
			b1(8);
		}
		return;
	}
	case ND_AND:
	case ND_OR: {
		gen_expr(n->lhs);
		test_rax();
		isize skip = jump(n->kind == ND_AND ? 0x84 : 0x85);
		gen_expr(n->rhs);
		test_rax();
		isize skip2 = jump(n->kind == ND_AND ? 0x84 : 0x85);
		mov_reg_imm(RAX, n->kind == ND_AND ? 1 : 0);
		isize end = jump(0);
		patch_here(skip);
		patch_here(skip2);
		mov_reg_imm(RAX, n->kind == ND_AND ? 0 : 1);
		patch_here(end);
		return;
	}
	default:
		break;
	}

	gen_expr(n->rhs);
	push_reg(RAX);
	gen_expr(n->lhs);
	pop_reg(RDI);

	switch (n->kind) {
	case ND_ADD: alu_rax_rdi(0x01); return;
	case ND_SUB: alu_rax_rdi(0x29); return;
	case ND_MUL:
		b1(0x48);
		b1(0x0f);
		b1(0xaf);
		modrm(3, RAX, RDI);
		return;
	case ND_DIV:
	case ND_MOD:
		b1(0x48);
		b1(0x99);
		b1(0x48);
		b1(0xf7);
		modrm(3, 7, RDI);
		if (n->kind == ND_MOD) mov_reg_reg(RAX, RDX);
		return;
	case ND_EQ: alu_rax_rdi(0x39); setcc(0x94); return;
	case ND_NE: alu_rax_rdi(0x39); setcc(0x95); return;
	case ND_LT: alu_rax_rdi(0x39); setcc(0x9c); return;
	case ND_LE: alu_rax_rdi(0x39); setcc(0x9e); return;
	default: error_at(n->pos, "invalid expression");
	}
}

static void gen_stmt(Node *n) {
	switch (n->kind) {
	case ND_BLOCK:
		for (Node *s = n->body; s; s = s->next) gen_stmt(s);
		return;
	case ND_EXPRSTMT:
		if (n->lhs) gen_expr(n->lhs);
		return;
	case ND_RET:
		if (n->lhs) gen_expr(n->lhs);
		else mov_reg_imm(RAX, 0);
		b1(0xc9);
		b1(0xc3);
		return;
	case ND_IF: {
		gen_expr(n->cond);
		test_rax();
		isize els = jump(0x84);
		gen_stmt(n->then);
		if (n->els) {
			isize end = jump(0);
			patch_here(els);
			gen_stmt(n->els);
			patch_here(end);
		} else {
			patch_here(els);
		}
		return;
	}
	case ND_WHILE: {
		isize top = code.n;
		gen_expr(n->cond);
		test_rax();
		isize out_at = jump(0x84);
		gen_stmt(n->body);
		b1(0xe9);
		b4((u32)(top - (code.n + 4)));
		patch_here(out_at);
		return;
	}
	default:
		gen_expr(n);
		return;
	}
}

static void gen_func(Func *f) {
	f->addr = code.n;
	depth = 0;

	push_reg(RBP);
	depth--;
	mov_reg_reg(RBP, RSP);
	if (f->stack_size) {
		b1(0x48);
		b1(0x81);
		modrm(3, 5, RSP);
		b4((u32)f->stack_size);
	}

	int i = 0;
	for (Var *v = f->params; v; v = v->next) store_local(call_regs[i++], v->offset);

	gen_stmt(f->body);

	mov_reg_imm(RAX, 0);
	b1(0xc9);
	b1(0xc3);
}

Image codegen(Unit *u) {
	all_funcs = u->funcs;

	str_off = anew_n(isize, u->nstrs ? u->nstrs : 1);
	for (int i = 0; i < u->nstrs; i++) {
		str_off[i] = rodata.n;
		buf_bytes(&rodata, u->strs[i].p, u->strs[i].n);
		buf_u8(&rodata, 0);
	}

	add_call(S("main"), 0);
	b1(0x89);
	modrm(3, RAX, RDI);
	mov_reg_imm(RAX, 60);
	b1(0x0f);
	b1(0x05);

	bool has_main = false;
	for (Func *f = u->funcs; f; f = f->next) {
		if (str_eq(f->name, S("main"))) has_main = true;
		gen_func(f);
	}
	if (!has_main) die("error: no 'main' function\n");

	for (int i = 0; i < ncalls; i++) {
		Func *target = NULL;
		for (Func *f = u->funcs; f; f = f->next)
			if (str_eq(f->name, calls[i].name)) target = f;
		if (!target) error_at(calls[i].pos, "undefined function '%s'", calls[i].name);
		buf_patch32(&code, calls[i].at, (u32)(target->addr - (calls[i].at + 4)));
	}

	isize rodata_at = (code.n + 7) & ~(isize)7;
	for (int i = 0; i < nstrrefs; i++) {
		isize target = rodata_at + str_off[strrefs[i].id];
		buf_patch32(&code, strrefs[i].at, (u32)(target - (strrefs[i].at + 4)));
	}
	while (code.n < rodata_at) buf_u8(&code, 0);

	return (Image){.code = code, .rodata = rodata, .entry = 0};
}
