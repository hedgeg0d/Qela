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

typedef struct {
	isize at;
	Var  *var;
} DataFixup;

typedef struct {
	isize *p;
	int    n;
	int    cap;
} List;

typedef struct Loop Loop;
struct Loop {
	Loop *prev;
	List  brk;
	List  cont;
	int   depth;
};

typedef struct DeferScope DeferScope;
struct DeferScope {
	DeferScope *prev;
	Node       *list;
	int         depth;
};

static Buf   code;
static Buf   rodata;
static Buf   data;
static isize bss_size;
static int   depth;
static Loop *cur_loop;
static Func *cur_func;
static DeferScope *cur_defers;
static int         defer_depth;

static CallFixup *calls;
static int ncalls, calls_cap;
static StrFixup *strrefs;
static int nstrrefs, strrefs_cap;
static isize *str_off;
static DataFixup *datarefs;
static int ndatarefs, datarefs_cap;
static List bounds_checks;

bool opt_no_bounds;

static void list_push(List *l, isize v) {
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 8;
		isize *p = anew_n(isize, cap);
		memcpy(p, l->p, sizeof(isize) * (usize)l->n);
		l->p = p;
		l->cap = cap;
	}
	l->p[l->n++] = v;
}

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

static void rbp_mem(u8 op, int reg, int disp) {
	rex_w(reg, RBP);
	b1(op);
	if (disp >= -128 && disp <= 127) {
		modrm(1, reg, RBP);
		b1((u8)disp);
	} else {
		modrm(2, reg, RBP);
		b4((u32)disp);
	}
}

static void lea_local(int reg, int off) { rbp_mem(0x8d, reg, -off); }
static void ld_local(int reg, int off) { rbp_mem(0x8b, reg, -off); }
static void st_local(int off, int reg) { rbp_mem(0x89, reg, -off); }

static void ld_at_rax(int reg, int disp) {
	rex_w(reg, RAX);
	b1(0x8b);
	if (disp == 0) {
		modrm(0, reg, RAX);
	} else {
		modrm(1, reg, RAX);
		b1((u8)disp);
	}
}

static void push_at_rax(int disp) {
	b1(0xff);
	if (disp == 0) {
		modrm(0, 6, RAX);
	} else {
		modrm(1, 6, RAX);
		b1((u8)disp);
	}
	depth++;
}

static void load(Type *ty) {
	switch (ty->size) {
	case 1:
		b1(0x48);
		b1(0x0f);
		b1(ty->is_unsigned || ty->kind == TY_BOOL ? 0xb6 : 0xbe);
		modrm(0, RAX, RAX);
		return;
	case 2:
		b1(0x48);
		b1(0x0f);
		b1(ty->is_unsigned ? 0xb7 : 0xbf);
		modrm(0, RAX, RAX);
		return;
	case 4:
		if (ty->is_unsigned) {
			b1(0x8b);
		} else {
			b1(0x48);
			b1(0x63);
		}
		modrm(0, RAX, RAX);
		return;
	default:
		b1(0x48);
		b1(0x8b);
		modrm(0, RAX, RAX);
		return;
	}
}

static void store(Type *ty) {
	pop_reg(RDI);
	switch (ty->size) {
	case 1: b1(0x88); modrm(0, RAX, RDI); return;
	case 2: b1(0x66); b1(0x89); modrm(0, RAX, RDI); return;
	case 4: b1(0x89); modrm(0, RAX, RDI); return;
	default: b1(0x48); b1(0x89); modrm(0, RAX, RDI); return;
	}
}

static void truncate_to(Type *ty) {
	switch (ty->size) {
	case 1:
		b1(0x48);
		b1(0x0f);
		b1(ty->is_unsigned || ty->kind == TY_BOOL ? 0xb6 : 0xbe);
		modrm(3, RAX, RAX);
		return;
	case 2:
		b1(0x48);
		b1(0x0f);
		b1(ty->is_unsigned ? 0xb7 : 0xbf);
		modrm(3, RAX, RAX);
		return;
	case 4:
		if (ty->is_unsigned) {
			b1(0x89);
			modrm(3, RAX, RAX);
		} else {
			b1(0x48);
			b1(0x63);
			modrm(3, RAX, RAX);
		}
		return;
	default:
		return;
	}
}

static void test_rax(void) {
	b1(0x48);
	b1(0x85);
	modrm(3, RAX, RAX);
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

static void add_strref(int reg, int id) {
	if (nstrrefs == strrefs_cap) {
		int cap = strrefs_cap ? strrefs_cap * 2 : 32;
		StrFixup *p = anew_n(StrFixup, cap);
		memcpy(p, strrefs, sizeof(StrFixup) * (usize)nstrrefs);
		strrefs = p;
		strrefs_cap = cap;
	}
	rex_w(reg, RBP);
	b1(0x8d);
	modrm(0, reg, RBP);
	strrefs[nstrrefs++] = (StrFixup){code.n, id};
	b4(0);
}

static void add_dataref(Var *v) {
	if (ndatarefs == datarefs_cap) {
		int cap = datarefs_cap ? datarefs_cap * 2 : 32;
		DataFixup *p = anew_n(DataFixup, cap);
		memcpy(p, datarefs, sizeof(DataFixup) * (usize)ndatarefs);
		datarefs = p;
		datarefs_cap = cap;
	}
	b1(0x48);
	b1(0x8d);
	modrm(0, RAX, RBP);
	datarefs[ndatarefs++] = (DataFixup){code.n, v};
	b4(0);
}

static void gen_expr(Node *n);
static void gen_stmt(Node *n);

static void add_rax_imm(i64 v) {
	if (v == 0) return;
	b1(0x48);
	if (v >= -128 && v <= 127) {
		b1(0x83);
		modrm(3, 0, RAX);
		b1((u8)v);
	} else {
		b1(0x81);
		modrm(3, 0, RAX);
		b4((u32)v);
	}
}

static void scale_by(int size);

static void jump_bounds(u8 cc) {
	b1(0x0f);
	b1(cc);
	list_push(&bounds_checks, code.n);
	b4(0);
}

static void gen_addr(Node *n) {
	switch (n->kind) {
	case ND_VAR:
		if (n->var->is_global) add_dataref(n->var);
		else if (n->var->by_ref) ld_local(RAX, n->var->offset);
		else lea_local(RAX, n->var->offset);
		return;
	case ND_DEREF:
		gen_expr(n->lhs);
		return;
	case ND_MEMBER:
		if (n->lhs->ty->kind == TY_PTR) gen_expr(n->lhs);
		else gen_addr(n->lhs);
		add_rax_imm(n->member->offset);
		return;
	case ND_INDEX: {
		Type *lt = n->lhs->ty;
		if (lt->kind == TY_SLICE) {
			gen_addr(n->lhs);
			push_at_rax(0);
			push_at_rax(8);
			gen_expr(n->rhs);
			pop_reg(RDI);
			if (!opt_no_bounds) {
				rex_w(RDI, RAX);
				b1(0x39);
				modrm(3, RDI, RAX);
				jump_bounds(0x83);
			}
		} else {
			if (lt->kind == TY_ARRAY) gen_addr(n->lhs);
			else gen_expr(n->lhs);
			push_reg(RAX);
			gen_expr(n->rhs);
			if (lt->kind == TY_ARRAY && !opt_no_bounds) {
				b1(0x48);
				b1(0x3d);
				b4((u32)lt->len);
				jump_bounds(0x83);
			}
		}
		scale_by(lt->base->size);
		pop_reg(RDI);
		rex_w(RDI, RAX);
		b1(0x01);
		modrm(3, RDI, RAX);
		return;
	}
	case ND_SLICE: {
		Type *lt = n->lhs->ty;
		int   off = n->tmp->offset;
		int   elem = lt->base->size;

		gen_addr(n->lhs);
		if (lt->kind == TY_SLICE) {
			ld_at_rax(RDI, 0);
			st_local(off, RDI);
			ld_at_rax(RDI, 8);
			st_local(off - 8, RDI);
		} else {
			st_local(off, RAX);
			mov_reg_imm(RDI, lt->len);
			st_local(off - 8, RDI);
		}

		if (n->rhs) gen_expr(n->rhs);
		else mov_reg_imm(RAX, 0);
		push_reg(RAX);

		if (n->then) gen_expr(n->then);
		else ld_local(RAX, off - 8);
		pop_reg(RDI);

		if (!opt_no_bounds) {
			rbp_mem(0x3b, RAX, -(off - 8));
			jump_bounds(0x87);
			rex_w(RAX, RDI);
			b1(0x39);
			modrm(3, RAX, RDI);
			jump_bounds(0x87);
		}

		rex_w(RDI, RAX);
		b1(0x29);
		modrm(3, RDI, RAX);
		st_local(off - 8, RAX);

		mov_reg_reg(RAX, RDI);
		scale_by(elem);
		rbp_mem(0x03, RAX, -off);
		st_local(off, RAX);

		lea_local(RAX, off);
		return;
	}
	default:
		if (is_aggregate(n->ty)) {
			gen_expr(n);
			return;
		}
		error_at(n->pos, "not an addressable expression");
	}
}

static void gen_lvalue(Node *n) {
	gen_addr(n);
	if (!is_aggregate(n->ty)) load(n->ty);
}

static void copy_rsi_rdi(int size) {
	mov_reg_imm(RCX, size);
	b1(0xf3);
	b1(0xa4);
}

static void copy_aggregate(int size) {
	pop_reg(RDI);
	mov_reg_reg(RSI, RAX);
	push_reg(RDI);
	mov_reg_imm(RCX, size);
	b1(0xf3);
	b1(0xa4);
	pop_reg(RAX);
}

static bool by_ref_ty(Type *ty) { return is_aggregate(ty) && ty->size > 16; }

static void gen_args(Node *n, const int *regs, bool first_in_rax, int hidden) {
	int dest[14];
	int nd = 0, ri = 0;

	if (hidden >= 0) {
		lea_local(RAX, hidden);
		push_reg(RAX);
		dest[nd++] = regs[ri++];
	}

	for (Node *a = n->args; a; a = a->next) {
		gen_expr(a);
		if (by_ref_ty(a->ty)) {
			mov_reg_reg(RSI, RAX);
			lea_local(RDI, a->tmp->offset);
			copy_rsi_rdi(a->ty->size);
			lea_local(RAX, a->tmp->offset);
			push_reg(RAX);
			dest[nd++] = regs[ri++];
		} else if (is_aggregate(a->ty)) {
			push_at_rax(0);
			int r = first_in_rax && nd == 0 ? RAX : regs[ri++];
			dest[nd++] = r;
			if (a->ty->size > 8) {
				push_at_rax(8);
				dest[nd++] = regs[ri++];
			}
		} else {
			push_reg(RAX);
			if (first_in_rax && nd == 0) dest[nd++] = RAX;
			else dest[nd++] = regs[ri++];
		}
	}
	while (nd-- > 0) pop_reg(dest[nd]);
}

static void scale_by(int size) {
	if (size == 1) return;
	mov_reg_imm(RCX, size);
	b1(0x48);
	b1(0x0f);
	b1(0xaf);
	modrm(3, RAX, RCX);
}

static void gen_binop(Node *n) {
	Type *t = n->ty;
	switch (n->kind) {
	case ND_ADD:
		rex_w(RDI, RAX);
		b1(0x01);
		modrm(3, RDI, RAX);
		return;
	case ND_SUB:
		rex_w(RDI, RAX);
		b1(0x29);
		modrm(3, RDI, RAX);
		return;
	case ND_MUL:
		b1(0x48);
		b1(0x0f);
		b1(0xaf);
		modrm(3, RAX, RDI);
		return;
	case ND_DIV:
	case ND_MOD:
		if (t->is_unsigned) {
			b1(0x31);
			modrm(3, RDX, RDX);
			b1(0x48);
			b1(0xf7);
			modrm(3, 6, RDI);
		} else {
			b1(0x48);
			b1(0x99);
			b1(0x48);
			b1(0xf7);
			modrm(3, 7, RDI);
		}
		if (n->kind == ND_MOD) mov_reg_reg(RAX, RDX);
		return;
	case ND_BITAND:
		rex_w(RDI, RAX);
		b1(0x21);
		modrm(3, RDI, RAX);
		return;
	case ND_BITOR:
		rex_w(RDI, RAX);
		b1(0x09);
		modrm(3, RDI, RAX);
		return;
	case ND_BITXOR:
		rex_w(RDI, RAX);
		b1(0x31);
		modrm(3, RDI, RAX);
		return;
	case ND_SHL:
	case ND_SHR:
		mov_reg_reg(RCX, RDI);
		b1(0x48);
		b1(0xd3);
		modrm(3, n->kind == ND_SHL ? 4 : (t->is_unsigned ? 5 : 7), RAX);
		return;
	case ND_EQ:
	case ND_NE:
	case ND_LT:
	case ND_LE: {
		rex_w(RDI, RAX);
		b1(0x39);
		modrm(3, RDI, RAX);
		bool uns = n->lhs->ty->is_unsigned || n->lhs->ty->kind == TY_PTR;
		u8 cc;
		if (n->kind == ND_EQ) cc = 0x94;
		else if (n->kind == ND_NE) cc = 0x95;
		else if (n->kind == ND_LT) cc = uns ? 0x92 : 0x9c;
		else cc = uns ? 0x96 : 0x9e;
		setcc(cc);
		return;
	}
	default:
		error_at(n->pos, "invalid expression");
	}
}

static void gen_expr(Node *n) {
	switch (n->kind) {
	case ND_NUM:
		mov_reg_imm(RAX, n->val);
		return;
	case ND_STRLIT:
		add_strref(RAX, (int)n->val);
		return;
	case ND_COMPTIME:
		/* Result was precomputed during type checking. */
		mov_reg_imm(RAX, n->val);
		return;
	case ND_SIZEOF:
		mov_reg_imm(RAX, n->typeval->size);
		return;
	case ND_VAR:
	case ND_MEMBER:
	case ND_INDEX:
	case ND_SLICE:
		gen_lvalue(n);
		return;
	case ND_ADDR:
		gen_addr(n->lhs);
		return;
	case ND_DEREF:
		gen_expr(n->lhs);
		if (!is_aggregate(n->ty)) load(n->ty);
		return;
	case ND_CAST:
		gen_expr(n->lhs);
		if (n->ty->kind == TY_BOOL) {
			test_rax();
			setcc(0x95);
		} else {
			truncate_to(n->ty);
		}
		return;
	case ND_ASSIGN:
		gen_addr(n->lhs);
		push_reg(RAX);
		gen_expr(n->rhs);
		if (is_aggregate(n->lhs->ty)) copy_aggregate(n->lhs->ty->size);
		else store(n->lhs->ty);
		return;
	case ND_OPASSIGN: {
		gen_addr(n->lhs);
		push_reg(RAX);
		load(n->lhs->ty);
		push_reg(RAX);
		gen_expr(n->rhs);
		if (n->lhs->ty->kind == TY_PTR) scale_by(n->lhs->ty->base->size);
		mov_reg_reg(RDI, RAX);
		pop_reg(RAX);
		Node op = {.kind = (NodeKind)n->val, .ty = n->lhs->ty, .lhs = n->lhs, .pos = n->pos};
		gen_binop(&op);
		truncate_to(n->lhs->ty);
		store(n->lhs->ty);
		return;
	}
	case ND_ENUMLIT: {
		int off = n->tmp->offset;
		mov_reg_imm(RAX, n->variant->tag);
		st_local(off, RAX);

		Member *f = n->variant->fields;
		for (Node *a = n->args; a; a = a->next, f = f->next) {
			lea_local(RDI, off - f->offset);
			push_reg(RDI);
			gen_expr(a);
			if (is_aggregate(f->ty)) copy_aggregate(f->ty->size);
			else store(f->ty);
		}
		lea_local(RAX, off);
		return;
	}
	case ND_COMMA:
		gen_expr(n->lhs);
		gen_expr(n->rhs);
		return;
	case ND_NEG:
		gen_expr(n->lhs);
		b1(0x48);
		b1(0xf7);
		modrm(3, 3, RAX);
		return;
	case ND_BITNOT:
		gen_expr(n->lhs);
		b1(0x48);
		b1(0xf7);
		modrm(3, 2, RAX);
		return;
	case ND_NOT:
		gen_expr(n->lhs);
		test_rax();
		setcc(0x94);
		return;
	case ND_SYSCALL:
		gen_args(n, sys_regs, true, -1);
		b1(0x0f);
		b1(0x05);
		return;
	case ND_CALL: {
		bool ret_mem = is_aggregate(n->ty) && n->ty->size > 16;
		gen_args(n, call_regs, false, ret_mem ? n->tmp->offset : -1);
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
		if (is_aggregate(n->ty) && !ret_mem) {
			int off = n->tmp->offset;
			st_local(off, RAX);
			if (n->ty->size > 8) st_local(off - 8, RDX);
			lea_local(RAX, off);
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

	Type *lt = n->lhs->ty;
	Type *rt = n->rhs->ty;
	bool ptr_diff = (n->kind == ND_SUB && lt->kind == TY_PTR && rt->kind == TY_PTR);

	gen_expr(n->rhs);
	if (!ptr_diff && lt->kind == TY_PTR && rt->kind != TY_PTR) scale_by(lt->base->size);
	push_reg(RAX);
	gen_expr(n->lhs);
	pop_reg(RDI);
	gen_binop(n);

	if (ptr_diff) {
		mov_reg_imm(RDI, lt->base->size);
		b1(0x48);
		b1(0x99);
		b1(0x48);
		b1(0xf7);
		modrm(3, 7, RDI);
	}
}

static void run_defers(DeferScope *ds) {
	for (Node *d = ds->list; d; d = d->els) gen_stmt(d->lhs);
}

static void unwind_to(int target) {
	for (DeferScope *ds = cur_defers; ds && ds->depth > target; ds = ds->prev)
		run_defers(ds);
}

static void gen_match(Node *n) {
	int soff = n->tmp->offset;
	gen_addr(n->cond);
	st_local(soff, RAX);

	List ends = {0};
	for (Node *arm = n->body; arm; arm = arm->next) {
		isize miss = -1;
		if (arm->name.n) {
			ld_local(RAX, soff);
			b1(0x48);
			b1(0x83);
			modrm(0, 7, RAX);
			b1((u8)arm->variant->tag);
			miss = jump(0x85);
		}

		Member *f = arm->variant ? arm->variant->fields : NULL;
		for (Node *b = arm->args; b; b = b->next, f = f->next) {
			lea_local(RDI, b->var->offset);
			push_reg(RDI);
			ld_local(RAX, soff);
			add_rax_imm(f->offset);
			if (is_aggregate(f->ty)) {
				copy_aggregate(f->ty->size);
			} else {
				load(f->ty);
				store(f->ty);
			}
		}

		gen_stmt(arm->body);
		if (arm->next) list_push(&ends, jump(0));
		if (miss >= 0) patch_here(miss);
	}
	for (int i = 0; i < ends.n; i++) patch_here(ends.p[i]);
}

static void gen_stmt(Node *n) {
	switch (n->kind) {
	case ND_BLOCK: {
		DeferScope ds = {.prev = cur_defers, .depth = ++defer_depth};
		cur_defers = &ds;
		for (Node *s = n->body; s; s = s->next) gen_stmt(s);
		run_defers(&ds);
		cur_defers = ds.prev;
		defer_depth--;
		return;
	}
	case ND_DEFER:
		if (!cur_defers) error_at(n->pos, "'defer' outside of a block");
		n->els = cur_defers->list;
		cur_defers->list = n;
		return;
	case ND_MATCH:
		gen_match(n);
		return;
	case ND_EXPRSTMT:
		if (n->lhs) gen_expr(n->lhs);
		return;
	case ND_RET:
		if (n->lhs) {
			gen_expr(n->lhs);
			if (is_aggregate(n->lhs->ty)) {
				if (cur_func->ret_slot) {
					mov_reg_reg(RSI, RAX);
					ld_local(RDI, cur_func->ret_slot->offset);
					copy_rsi_rdi(n->lhs->ty->size);
					ld_local(RAX, cur_func->ret_slot->offset);
				} else {
					if (n->lhs->ty->size > 8) ld_at_rax(RDX, 8);
					ld_at_rax(RAX, 0);
				}
			}
		} else {
			mov_reg_imm(RAX, 0);
		}
		if (cur_defers) {
			bool wide = n->lhs && is_aggregate(n->lhs->ty) &&
			            n->lhs->ty->size > 8 && n->lhs->ty->size <= 16;
			push_reg(RAX);
			if (wide) push_reg(RDX);
			unwind_to(0);
			if (wide) pop_reg(RDX);
			pop_reg(RAX);
		}
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
	case ND_FOR: {
		if (n->init) gen_stmt(n->init);

		Loop lp = {.prev = cur_loop, .depth = defer_depth};
		cur_loop = &lp;

		isize top = code.n;
		if (n->cond) {
			gen_expr(n->cond);
			test_rax();
			list_push(&lp.brk, jump(0x84));
		}
		gen_stmt(n->body);

		for (int i = 0; i < lp.cont.n; i++) patch_here(lp.cont.p[i]);
		if (n->step) gen_expr(n->step);
		b1(0xe9);
		b4((u32)(top - (code.n + 4)));

		for (int i = 0; i < lp.brk.n; i++) patch_here(lp.brk.p[i]);
		cur_loop = lp.prev;
		return;
	}
	case ND_BREAK:
		if (!cur_loop) error_at(n->pos, "'break' outside of a loop");
		unwind_to(cur_loop->depth);
		list_push(&cur_loop->brk, jump(0));
		return;
	case ND_CONT:
		if (!cur_loop) error_at(n->pos, "'continue' outside of a loop");
		unwind_to(cur_loop->depth);
		list_push(&cur_loop->cont, jump(0));
		return;
	default:
		gen_expr(n);
		return;
	}
}

static void gen_func(Func *f) {
	f->addr = code.n;
	depth = 0;
	cur_func = f;
	cur_loop = NULL;
	cur_defers = NULL;
	defer_depth = 0;

	push_reg(RBP);
	depth--;
	mov_reg_reg(RBP, RSP);
	if (f->stack_size) {
		b1(0x48);
		b1(0x81);
		modrm(3, 5, RSP);
		b4((u32)f->stack_size);
	}

	int ri = 0;
	if (f->ret_slot) st_local(f->ret_slot->offset, call_regs[ri++]);
	for (int i = 0; i < f->nparams; i++) {
		Var *v = f->params[i];
		st_local(v->offset, call_regs[ri++]);
		if (is_aggregate(v->ty) && !v->by_ref && v->ty->size > 8)
			st_local(v->offset - 8, call_regs[ri++]);
	}

	gen_stmt(f->body);

	mov_reg_imm(RAX, 0);
	b1(0xc9);
	b1(0xc3);
}

static void layout_globals(Var *globals) {
	for (Var *v = globals; v; v = v->next) {
		if (v->init == 0) continue;
		isize off = (data.n + v->ty->align - 1) & ~(isize)(v->ty->align - 1);
		while (data.n < off) buf_u8(&data, 0);
		v->data_off = data.n;
		for (int i = 0; i < v->ty->size; i++) buf_u8(&data, (u8)(v->init >> (i * 8)));
	}
	isize bss = data.n;
	for (Var *v = globals; v; v = v->next) {
		if (v->init != 0) continue;
		bss = (bss + v->ty->align - 1) & ~(isize)(v->ty->align - 1);
		v->data_off = bss;
		bss += v->ty->size;
	}
	bss_size = bss - data.n;
}

static const char panic_msg[] = "qela: index out of range\n";
#define PANIC_LEN ((isize)sizeof(panic_msg) - 1)

static void gen_panic_stub(int msg_id) {
	add_strref(RSI, msg_id);
	mov_reg_imm(RAX, 1);
	mov_reg_imm(RDI, 2);
	mov_reg_imm(RDX, PANIC_LEN);
	b1(0x0f);
	b1(0x05);
	mov_reg_imm(RAX, 60);
	mov_reg_imm(RDI, 134);
	b1(0x0f);
	b1(0x05);
}

Image codegen(Unit *u) {
	int    panic_id = u->nstrs;
	isize *bytes_off = anew_n(isize, u->nstrs ? u->nstrs : 1);
	str_off = anew_n(isize, u->nstrs + 1);

	for (int i = 0; i < u->nstrs; i++) {
		bytes_off[i] = rodata.n;
		buf_bytes(&rodata, u->strs[i].p, u->strs[i].n);
		buf_u8(&rodata, 0);
	}
	while (rodata.n & 7) buf_u8(&rodata, 0);

	for (int i = 0; i < u->nstrs; i++) {
		str_off[i] = rodata.n;
		buf_u64(&rodata, 0);
		buf_u64(&rodata, (u64)u->strs[i].n);
	}

	layout_globals(u->globals);
	int   nph = data.n + bss_size > 0 ? 2 : 1;
	isize hdr = EHDR_SZ + PHDR_SZ * nph;

	add_call(S("main"), 0);
	b1(0x89);
	modrm(3, RAX, RDI);
	mov_reg_imm(RAX, 60);
	b1(0x0f);
	b1(0x05);

	Func *main_fn = find_func(S("main"));
	if (!main_fn) die("error: no 'main' function\n");
	if (main_fn->nparams) die("error: 'main' must take no parameters\n");

	for (Func *f = u->funcs; f; f = f->next) {
		if (f->nct) continue;
		gen_func(f);
	}

	if (bounds_checks.n) {
		isize panic_addr = code.n;
		gen_panic_stub(panic_id);
		for (int i = 0; i < bounds_checks.n; i++)
			buf_patch32(&code, bounds_checks.p[i],
			            (u32)(panic_addr - (bounds_checks.p[i] + 4)));
	}

	for (int i = 0; i < ncalls; i++) {
		Func *target = find_func(calls[i].name);
		if (!target) error_at(calls[i].pos, "undefined function '%s'", calls[i].name);
		buf_patch32(&code, calls[i].at, (u32)(target->addr - (calls[i].at + 4)));
	}

	while (code.n & 7) buf_u8(&code, 0);

	if (bounds_checks.n) {
		str_off[panic_id] = rodata.n;
		buf_bytes(&rodata, panic_msg, PANIC_LEN);
		while (rodata.n & 7) buf_u8(&rodata, 0);
	}

	for (int i = 0; i < nstrrefs; i++) {
		isize target = code.n + str_off[strrefs[i].id];
		buf_patch32(&code, strrefs[i].at, (u32)(target - (strrefs[i].at + 4)));
	}

	u64 rodata_vaddr = ELF_BASE + (u64)hdr + (u64)code.n;
	for (int i = 0; i < u->nstrs; i++) {
		u64 abs = rodata_vaddr + (u64)bytes_off[i];
		for (int k = 0; k < 8; k++)
			rodata.p[str_off[i] + k] = (u8)(abs >> (k * 8));
	}

	for (int i = 0; i < ndatarefs; i++) {
		isize target = SEG_GAP + code.n + rodata.n + datarefs[i].var->data_off;
		buf_patch32(&code, datarefs[i].at, (u32)(target - (datarefs[i].at + 4)));
	}

	return (Image){.code = code,
	               .rodata = rodata,
	               .data = data,
	               .bss_size = bss_size,
	               .entry = 0,
	               .nph = nph};
}
