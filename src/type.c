#include "comp.h"

#define PRIM(var, k, sz, uns, nm)                                                 \
	static Type var##_v = {.kind = k, .size = sz, .align = sz, .is_unsigned = uns, \
	                       .name = {nm, sizeof(nm) - 1}};                          \
	Type *var = &var##_v;

PRIM(ty_void, TY_VOID, 1, false, "void")
PRIM(ty_bool, TY_BOOL, 1, false, "bool")
PRIM(ty_i8, TY_INT, 1, false, "i8")
PRIM(ty_i16, TY_INT, 2, false, "i16")
PRIM(ty_i32, TY_INT, 4, false, "i32")
PRIM(ty_i64, TY_INT, 8, false, "i64")
PRIM(ty_u8, TY_INT, 1, true, "u8")
PRIM(ty_u16, TY_INT, 2, true, "u16")
PRIM(ty_u32, TY_INT, 4, true, "u32")
PRIM(ty_u64, TY_INT, 8, true, "u64")

Type *type_ptr(Type *base) {
	Type *t = anew(Type);
	*t = (Type){.kind = TY_PTR, .size = 8, .align = 8, .is_unsigned = true, .base = base};
	return t;
}

Type *type_array(Type *base, isize len) {
	Type *t = anew(Type);
	*t = (Type){.kind = TY_ARRAY,
	            .size = (int)(base->size * len),
	            .align = base->align,
	            .base = base,
	            .len = len};
	return t;
}

Type *type_slice(Type *base) {
	Type *t = anew(Type);
	*t = (Type){.kind = TY_SLICE, .size = 16, .align = 8, .base = base};

	Member *len = anew(Member);
	*len = (Member){.name = S("len"), .ty = ty_i64, .offset = 8};
	Member *ptr = anew(Member);
	*ptr = (Member){.next = len, .name = S("ptr"), .ty = type_ptr(base), .offset = 0};
	t->members = ptr;
	return t;
}

Type *type_str(void) {
	static Type *cached;
	if (!cached) cached = type_slice(ty_u8);
	return cached;
}

typedef struct Named Named;
struct Named {
	Named *next;
	Str    name;
	Type  *ty;
};

static Named *named_types;

void type_define(Str name, Type *ty) {
	Named *n = anew(Named);
	*n = (Named){.next = named_types, .name = name, .ty = ty};
	named_types = n;
}

Type *type_lookup(Str name) {
	static const struct {
		const char *name;
		Type      **ty;
	} table[] = {
	    {"void", &ty_void}, {"bool", &ty_bool}, {"i8", &ty_i8},   {"i16", &ty_i16},
	    {"i32", &ty_i32},   {"i64", &ty_i64},   {"u8", &ty_u8},   {"u16", &ty_u16},
	    {"u32", &ty_u32},   {"u64", &ty_u64},   {"int", &ty_i64}, {"uint", &ty_u64},
	    {"usize", &ty_u64}, {NULL, NULL},
	};
	for (int i = 0; table[i].name; i++)
		if (str_eq(name, str_from_cstr(table[i].name))) return *table[i].ty;
	if (str_eq(name, S("str"))) return type_str();
	for (Named *n = named_types; n; n = n->next)
		if (str_eq(n->name, name)) return n->ty;
	return NULL;
}

bool is_integer(Type *t) { return t->kind == TY_INT || t->kind == TY_BOOL; }
bool is_numeric(Type *t) { return is_integer(t) || t->kind == TY_PTR; }
bool is_aggregate(Type *t) {
	return t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_SLICE;
}

Type *decayed(Type *t) { return t->kind == TY_ARRAY ? type_ptr(t->base) : t; }

Str type_name(Type *t) {
	if (t->kind == TY_PTR) {
		Str base = type_name(t->base);
		char *p = anew_n(char, base.n + 2);
		p[0] = '*';
		memcpy(p + 1, base.p, (usize)base.n);
		p[base.n + 1] = 0;
		return (Str){p, base.n + 1};
	}
	if (t->kind == TY_SLICE) {
		if (t->base == ty_u8) return S("str");
		Str base = type_name(t->base);
		char *p = anew_n(char, base.n + 3);
		p[0] = '[';
		p[1] = ']';
		memcpy(p + 2, base.p, (usize)base.n);
		p[base.n + 2] = 0;
		return (Str){p, base.n + 2};
	}
	if (t->kind == TY_ARRAY) {
		Str base = type_name(t->base);
		char *p = anew_n(char, base.n + 24);
		isize n = 0;
		p[n++] = '[';
		char tmp[20];
		int  d = 0;
		isize v = t->len;
		do {
			tmp[d++] = (char)('0' + v % 10);
			v /= 10;
		} while (v);
		while (d) p[n++] = tmp[--d];
		p[n++] = ']';
		memcpy(p + n, base.p, (usize)base.n);
		n += base.n;
		p[n] = 0;
		return (Str){p, n};
	}
	return t->name;
}

static Type *common(Type *a, Type *b) {
	if (a->kind == TY_PTR) return a;
	if (b->kind == TY_PTR) return b;
	if (a->size < 8 && b->size < 8) return ty_i64;
	if (a->size > b->size) return a;
	if (b->size > a->size) return b;
	return a->is_unsigned ? a : b;
}

static Node *cast_to(Node *n, Type *ty) {
	if (n->ty == ty) return n;
	Node *c = anew(Node);
	*c = (Node){.kind = ND_CAST, .lhs = n, .ty = ty, .pos = n->pos};
	return c;
}

static Func *cur_fn_typing;

static Var *alloc_temp(Type *ty) {
	if (!cur_fn_typing) return NULL;
	Var *v = anew(Var);
	*v = (Var){.ty = ty, .next = cur_fn_typing->locals};
	cur_fn_typing->locals = v;
	return v;
}

static Node *to_value(Node *n) {
	return n->ty->kind == TY_ARRAY ? cast_to(n, decayed(n->ty)) : n;
}

static bool same_type(Type *a, Type *b) {
	if (a == b) return true;
	if (a->kind != b->kind) return false;
	switch (a->kind) {
	case TY_PTR:
	case TY_SLICE: return same_type(a->base, b->base);
	case TY_ARRAY: return a->len == b->len && same_type(a->base, b->base);
	case TY_STRUCT: return false;
	default: return a->size == b->size && a->is_unsigned == b->is_unsigned;
	}
}

static Node *coerce(Node *n, Type *want) {
	if (want->kind != TY_SLICE || n->ty->kind != TY_ARRAY) return n;
	if (!same_type(want->base, n->ty->base)) return n;
	Node *s = anew(Node);
	*s = (Node){
	    .kind = ND_SLICE, .typed = true, .ty = want, .lhs = n, .pos = n->pos};
	s->tmp = alloc_temp(want);
	return s;
}

static void usual_conv(Node *n) {
	Type *t = common(n->lhs->ty, n->rhs->ty);
	n->lhs = cast_to(n->lhs, t);
	n->rhs = cast_to(n->rhs, t);
}

void type_set_fn(Func *f) { cur_fn_typing = f; }

void type_func(Func *f) {
	cur_fn_typing = f;
	add_type(f->body);
}

void add_type(Node *n) {
	if (!n || n->typed) return;
	n->typed = true;

	add_type(n->lhs);
	add_type(n->rhs);
	add_type(n->cond);
	add_type(n->then);
	add_type(n->els);
	add_type(n->init);
	add_type(n->step);
	add_type(n->body);
	for (Node *a = n->args; a; a = a->next) add_type(a);
	for (Node *s = n->next; s; s = s->next) add_type(s);

	switch (n->kind) {
	case ND_NUM:
		n->ty = ty_i64;
		return;
	case ND_STRLIT:
		n->ty = type_str();
		return;
	case ND_VAR:
		n->ty = n->var->ty;
		return;

	case ND_ADD:
	case ND_SUB: {
		n->lhs = to_value(n->lhs);
		n->rhs = to_value(n->rhs);
		Type *lt = n->lhs->ty, *rt = n->rhs->ty;
		if (lt->kind == TY_PTR && rt->kind == TY_PTR) {
			if (n->kind == ND_ADD) error_at(n->pos, "cannot add two pointers");
			n->ty = ty_i64;
			return;
		}
		if (lt->kind == TY_PTR || rt->kind == TY_PTR) {
			if (rt->kind == TY_PTR) {
				Node *t = n->lhs;
				n->lhs = n->rhs;
				n->rhs = t;
			}
			if (!is_integer(n->rhs->ty))
				error_at(n->pos, "pointer arithmetic needs an integer offset");
			n->ty = n->lhs->ty;
			return;
		}
		if (!is_integer(lt) || !is_integer(rt))
			error_at(n->pos, "operands must be numeric");
		usual_conv(n);
		n->ty = n->lhs->ty;
		return;
	}

	case ND_MUL:
	case ND_DIV:
	case ND_MOD:
	case ND_BITAND:
	case ND_BITOR:
	case ND_BITXOR:
		if (!is_integer(n->lhs->ty) || !is_integer(n->rhs->ty))
			error_at(n->pos, "operands must be integers");
		usual_conv(n);
		n->ty = n->lhs->ty;
		return;

	case ND_SHL:
	case ND_SHR:
		if (!is_integer(n->lhs->ty) || !is_integer(n->rhs->ty))
			error_at(n->pos, "shift operands must be integers");
		n->lhs = cast_to(n->lhs, n->lhs->ty->size < 8 ? ty_i64 : n->lhs->ty);
		n->ty = n->lhs->ty;
		return;

	case ND_NEG:
		if (!is_integer(n->lhs->ty)) error_at(n->pos, "operand must be an integer");
		n->ty = n->lhs->ty->size < 8 ? ty_i64 : n->lhs->ty;
		return;

	case ND_BITNOT:
		if (!is_integer(n->lhs->ty)) error_at(n->pos, "operand must be an integer");
		n->ty = n->lhs->ty->size < 8 ? ty_i64 : n->lhs->ty;
		return;

	case ND_EQ:
	case ND_NE:
	case ND_LT:
	case ND_LE:
		n->lhs = to_value(n->lhs);
		n->rhs = to_value(n->rhs);
		if (is_aggregate(n->lhs->ty) || is_aggregate(n->rhs->ty))
			error_at(n->pos, "aggregates cannot be compared");
		if (is_integer(n->lhs->ty) && is_integer(n->rhs->ty)) usual_conv(n);
		n->ty = ty_bool;
		return;

	case ND_NOT:
	case ND_AND:
	case ND_OR:
		n->ty = ty_bool;
		return;

	case ND_ASSIGN:
		if (n->lhs->ty->kind == TY_ARRAY)
			error_at(n->pos, "cannot assign to an array");
		if (is_aggregate(n->lhs->ty)) {
			n->rhs = coerce(n->rhs, n->lhs->ty);
			if (!same_type(n->lhs->ty, n->rhs->ty))
				error_at(n->pos, "cannot assign '%s' to '%s'",
				         type_name(n->rhs->ty), type_name(n->lhs->ty));
			n->ty = n->lhs->ty;
			return;
		}
		n->rhs = to_value(n->rhs);
		if (n->lhs->ty->kind == TY_PTR && is_integer(n->rhs->ty) &&
		    !(n->rhs->kind == ND_NUM && n->rhs->val == 0))
			error_at(n->pos, "cannot assign an integer to a pointer");
		if (is_integer(n->lhs->ty) && n->rhs->ty->kind == TY_PTR)
			error_at(n->pos, "cannot assign a pointer to an integer");
		if (n->lhs->ty->kind == TY_PTR && n->rhs->ty->kind == TY_PTR &&
		    !same_type(n->lhs->ty, n->rhs->ty) && n->rhs->ty->base->kind != TY_VOID &&
		    n->lhs->ty->base->kind != TY_VOID)
			error_at(n->pos, "cannot assign '%s' to '%s'", type_name(n->rhs->ty),
			         type_name(n->lhs->ty));
		n->rhs = cast_to(n->rhs, n->lhs->ty);
		n->ty = n->lhs->ty;
		return;

	case ND_INDEX: {
		Type *lt = n->lhs->ty;
		if (lt->kind != TY_ARRAY && lt->kind != TY_PTR && lt->kind != TY_SLICE)
			error_at(n->pos, "cannot index a value of type '%s'", type_name(lt));
		if (lt->base->kind == TY_VOID) error_at(n->pos, "cannot index a *void");
		if (!is_integer(n->rhs->ty))
			error_at(n->pos, "an index must be an integer");
		n->ty = lt->base;
		return;
	}

	case ND_SLICE: {
		Type *lt = n->lhs->ty;
		if (lt->kind != TY_ARRAY && lt->kind != TY_SLICE)
			error_at(n->pos, "cannot slice a value of type '%s'", type_name(lt));
		if (n->rhs && !is_integer(n->rhs->ty))
			error_at(n->pos, "a slice bound must be an integer");
		if (n->then && !is_integer(n->then->ty))
			error_at(n->pos, "a slice bound must be an integer");
		n->ty = type_slice(lt->base);
		n->tmp = alloc_temp(n->ty);
		if (!n->tmp) error_at(n->pos, "a slice expression needs a function scope");
		return;
	}

	case ND_MEMBER: {
		Type *lt = n->lhs->ty;
		if (lt->kind == TY_PTR) lt = lt->base;
		if (lt->kind != TY_STRUCT && lt->kind != TY_SLICE)
			error_at(n->pos, "type '%s' has no fields", type_name(n->lhs->ty));
		for (Member *m = lt->members; m; m = m->next)
			if (str_eq(m->name, n->name)) {
				n->member = m;
				n->ty = m->ty;
				return;
			}
		error_at(n->pos, "'%s' has no field named '%s'", type_name(lt), n->name);
	}

	case ND_ADDR:
		n->ty = type_ptr(n->lhs->ty);
		return;

	case ND_DEREF:
		if (n->lhs->ty->kind != TY_PTR)
			error_at(n->pos, "cannot dereference a value of type '%s'",
			         type_name(n->lhs->ty));
		if (n->lhs->ty->base->kind == TY_VOID)
			error_at(n->pos, "cannot dereference a *void");
		n->ty = n->lhs->ty->base;
		return;

	case ND_OPASSIGN: {
		Type *lt = n->lhs->ty;
		if (lt->kind == TY_PTR) {
			if (n->val != ND_ADD && n->val != ND_SUB)
				error_at(n->pos, "operator not defined for pointers");
			if (!is_integer(n->rhs->ty))
				error_at(n->pos, "pointer arithmetic needs an integer offset");
		} else if (!is_integer(lt) || !is_integer(n->rhs->ty)) {
			error_at(n->pos, "operands must be numeric");
		}
		n->ty = lt;
		return;
	}

	case ND_CAST:
		if (!is_numeric(n->ty) && n->ty->kind != TY_VOID)
			error_at(n->pos, "unsupported cast target");
		return;

	case ND_COMMA:
		n->ty = n->rhs->ty;
		return;

	case ND_CALL: {
		Func *f = find_func(n->name);
		if (!f) error_at(n->pos, "undefined function '%s'", n->name);
		if (n->nargs != f->nparams)
			error_at(n->pos, "'%s' takes a different number of arguments", n->name);
		int i = 0;
		Node head = {0};
		Node *tail = &head;
		for (Node *a = n->args; a;) {
			Node *next = a->next;
			a->next = NULL;
			Type *want = f->params[i++]->ty;
			a = coerce(a, want);
			if (is_aggregate(want)) {
				if (!same_type(want, a->ty))
					error_at(a->pos, "argument must have type '%s'", type_name(want));
				tail->next = a;
			} else {
				a = to_value(a);
				if ((want->kind == TY_PTR) != (a->ty->kind == TY_PTR))
					error_at(a->pos, "argument must have type '%s'", type_name(want));
				tail->next = cast_to(a, want);
			}
			tail = tail->next;
			a = next;
		}
		n->args = head.next;
		n->ty = f->ret;
		if (is_aggregate(f->ret)) n->tmp = alloc_temp(f->ret);
		return;
	}

	case ND_SYSCALL: {
		Node head = {0};
		Node *tail = &head;
		for (Node *a = n->args; a;) {
			Node *next = a->next;
			a->next = NULL;
			a = to_value(a);
			if (!is_numeric(a->ty))
				error_at(a->pos,
				         "a syscall argument must be an integer or a pointer");
			tail->next = a;
			tail = a;
			a = next;
		}
		n->args = head.next;
		n->ty = ty_i64;
		return;
	}

	default:
		if (!n->ty) n->ty = ty_void;
		return;
	}
}
