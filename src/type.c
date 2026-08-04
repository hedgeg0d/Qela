#include "comp.h"

#define PRIM(var, k, sz, uns, nm)                                                 \
	static Type var##_v = {.kind = k, .size = sz, .align = sz, .is_unsigned = uns, \
	                       .name = {nm, sizeof(nm) - 1}};                          \
	Type *var = &var##_v;

PRIM(ty_void, TY_VOID, 1, false, "void")
PRIM(ty_bool, TY_BOOL, 1, false, "bool")
PRIM(ty_type, TY_TYPE, 8, false, "type")
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

Type *type_new_var(Str name) {
	Type *t = anew(Type);
	*t = (Type){.kind = TY_TYPEVAR, .size = 8, .align = 8, .name = name};
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

int enum_tag_of(Type *ty, Str name, Variant **out) {
	for (Variant *v = ty->variants; v; v = v->next)
		if (str_eq(v->name, name)) {
			if (out) *out = v;
			return v->tag;
		}
	return -1;
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
	    {"void", &ty_void}, {"bool", &ty_bool}, {"type", &ty_type},
	    {"i8", &ty_i8}, {"i16", &ty_i16}, {"i32", &ty_i32}, {"i64", &ty_i64},
	    {"u8", &ty_u8}, {"u16", &ty_u16}, {"u32", &ty_u32}, {"u64", &ty_u64},
	    {"int", &ty_i64}, {"uint", &ty_u64}, {"usize", &ty_u64}, {NULL, NULL},
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
	return t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_SLICE ||
	       t->kind == TY_ENUM;
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

/* ── monomorphization ── */

static Type *tsubst(Type *t, int nct, Type **vars, Type **concretes) {
	if (!t) return NULL;
	if (t->kind == TY_TYPEVAR) {
		for (int i = 0; i < nct; i++)
			if (t == vars[i]) return concretes[i];
		return t;
	}
	if (t->kind == TY_PTR) {
		Type *b = tsubst(t->base, nct, vars, concretes);
		return b != t->base ? type_ptr(b) : t;
	}
	if (t->kind == TY_ARRAY) {
		Type *b = tsubst(t->base, nct, vars, concretes);
		return b != t->base ? type_array(b, t->len) : t;
	}
	if (t->kind == TY_SLICE) {
		Type *b = tsubst(t->base, nct, vars, concretes);
		return b != t->base ? type_slice(b) : t;
	}
	return t;
}

static Var *var_map_old[256];
static Var *var_map_new[256];
static int    nvar_map;

static Var *clone_var(Var *v, int nct, Type **vars, Type **concretes) {
	if (!v) return NULL;
	Var *c = anew(Var);
	*c = *v;
	c->ty = tsubst(v->ty, nct, vars, concretes);
	c->next = NULL;
	var_map_old[nvar_map] = v;
	var_map_new[nvar_map] = c;
	nvar_map++;
	return c;
}

static Var *remap_var(Var *v) {
	for (int i = 0; i < nvar_map; i++)
		if (var_map_old[i] == v) return var_map_new[i];
	return v;
}

static Node *clone_node(Node *n, int nct, Type **vars, Type **concretes) {
	if (!n) return NULL;
	Node *c = anew(Node);
	*c = *n;
	c->ty = tsubst(n->ty, nct, vars, concretes);
	if (n->typeval) c->typeval = tsubst(n->typeval, nct, vars, concretes);
	if (n->var) c->var = remap_var(n->var);
	if (n->tmp) c->tmp = remap_var(n->tmp);
	c->lhs = clone_node(n->lhs, nct, vars, concretes);
	c->rhs = clone_node(n->rhs, nct, vars, concretes);
	c->cond = clone_node(n->cond, nct, vars, concretes);
	c->then = clone_node(n->then, nct, vars, concretes);
	c->els = clone_node(n->els, nct, vars, concretes);
	c->init = clone_node(n->init, nct, vars, concretes);
	c->step = clone_node(n->step, nct, vars, concretes);
	c->body = clone_node(n->body, nct, vars, concretes);
	c->args = clone_node(n->args, nct, vars, concretes);
	c->next = clone_node(n->next, nct, vars, concretes);
	return c;
}

static void build_name(char *buf, isize cap, Func *f, int nct, Type **concretes) {
	isize n = 0;
	Str fn = f->name;
	if (n + fn.n + 1 < cap) {
		memcpy(buf + n, fn.p, (usize)fn.n);
		n += fn.n;
		buf[n++] = '$';
	}
	for (int i = 0; i < nct; i++) {
		Str tn = type_name(concretes[i]);
		if (n + tn.n + 1 >= cap) break;
		memcpy(buf + n, tn.p, (usize)tn.n);
		n += tn.n;
		buf[n++] = '$';
	}
	buf[n] = 0;
}

static Func *clone_func(Func *f, int nct, Type **concretes) {
	Type *vars[6];
	for (int i = 0; i < nct; i++) vars[i] = f->params[i]->ty;

	char namebuf[256];
	build_name(namebuf, sizeof(namebuf), f, nct, concretes);

	Func *existing = find_func(str_from_cstr(namebuf));
	if (existing) return existing;

	Func *f2 = anew(Func);
	*f2 = (Func){.name = str_dup(str_from_cstr(namebuf)),
	             .ret = tsubst(f->ret, nct, vars, concretes),
	             .pos = f->pos};
	nvar_map = 0;

	/* Clone locals (params + temps), skipping comptime params. */
	Var hd = {0};
	Var *tl = &hd;
	for (Var *v = f->locals; v; v = v->next) {
		for (int i = 0; i < nct; i++)
			if (v == f->params[i]) goto skip;
		Var *cv = clone_var(v, nct, vars, concretes);
		tl->next = cv;
		tl = cv;
	skip:;
	}
	f2->locals = hd.next;

	/* Clone non-comptime params. */
	for (int i = nct; i < f->nparams; i++) {
		f2->params[f2->nparams] = remap_var(f->params[i]);
		f2->nparams++;
	}

	f2->body = clone_node(f->body, nct, vars, concretes);
	add_func(f2);
	return f2;
}

void monomorphize_call(Node *n, Func *f) {
	Type *concretes[6];
	int nct = f->nct;

	Node *a = n->args;
	for (int i = 0; i < nct; i++, a = a->next) {
		if (!a || a->kind != ND_TYPEEXPR)
			error_at(a ? a->pos : n->pos, "expected a type argument");
		concretes[i] = a->typeval;
		if (!concretes[i] || concretes[i]->kind == TY_TYPEVAR)
			error_at(a->pos, "type argument must be a concrete type");
	}

	Func *f2 = clone_func(f, nct, concretes);

	/* Rewrite the call to reference the instantiation. */
	n->name = f2->name;
	n->args = a; /* strip the comptime type args */
	n->nargs -= nct;

	/* Type the clone (ret_slot, stack_size handled by the parse loop picking it up). */
	cur_fn_typing = f2;
	if (is_aggregate(f2->ret) && f2->ret->size > 16)
		f2->ret_slot = alloc_temp(type_ptr(f2->ret));
	add_type(f2->body);
}

void type_func(Func *f) {
	cur_fn_typing = f;
	if (is_aggregate(f->ret) && f->ret->size > 16)
		f->ret_slot = alloc_temp(type_ptr(f->ret));
	add_type(f->body);
}

void add_type(Node *n);

static void type_match(Node *n) {
	add_type(n->cond);

	Type *st = n->cond->ty;
	if (st->kind != TY_ENUM)
		error_at(n->pos, "match needs an enum value, got '%s'", type_name(st));

	bool seen[64] = {false};
	bool has_default = false;

	for (Node *arm = n->body; arm; arm = arm->next) {
		arm->typed = true;
		arm->ty = ty_void;

		if (arm->name.n == 0) {
			if (has_default) error_at(arm->pos, "duplicate '_' arm");
			has_default = true;
		} else {
			Variant *v = NULL;
			int tag = enum_tag_of(st, arm->name, &v);
			if (tag < 0)
				error_at(arm->pos, "'%s' has no variant named '%s'", type_name(st),
				         arm->name);
			if (seen[tag]) error_at(arm->pos, "duplicate arm for '%s'", arm->name);
			seen[tag] = true;
			arm->variant = v;

			int nb = 0;
			for (Node *b = arm->args; b; b = b->next) nb++;
			if (nb && nb != v->nfields)
				error_at(arm->pos, "'%s' binds %d value(s), not %d", arm->name, nb,
				         v->nfields);

			Member *f = v->fields;
			for (Node *b = arm->args; b; b = b->next, f = f->next) {
				b->var->ty = f->ty;
				b->ty = f->ty;
				b->typed = true;
			}
		}
		add_type(arm->body);
	}

	if (!has_default)
		for (Variant *v = st->variants; v; v = v->next)
			if (!seen[v->tag]) error_at(n->pos, "match does not cover '%s'", v->name);

	n->tmp = alloc_temp(type_ptr(st));
	if (!n->tmp) error_at(n->pos, "match needs a function scope");
	n->ty = ty_void;
}

void add_type(Node *n) {
	if (!n || n->typed) return;
	n->typed = true;

	if (n->kind == ND_MATCH) {
		type_match(n);
		for (Node *s = n->next; s; s = s->next) add_type(s);
		return;
	}

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
	case ND_COMPTIME: {
		ComptimeVal v = comptime_eval(n);
		if (v.kind == CV_INT) {
			n->val = v.num;
			n->ty = ty_i64;
			return;
		}
		if (v.kind == CV_STR) {
			n->ty = type_str();
			return;
		}
		n->ty = ty_void;
		return;
	}
	case ND_VAR:
		n->ty = n->var->ty;
		return;

	case ND_TYPEEXPR:
		n->ty = ty_type;
		return;

	case ND_SIZEOF:
		n->ty = ty_i64;
		if (n->typeval->kind != TY_TYPEVAR)
			n->val = n->typeval->size;
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

	case ND_ENUMLIT: {
		Variant *v = NULL;
		if (enum_tag_of(n->ty, n->name, &v) < 0)
			error_at(n->pos, "'%s' has no variant named '%s'", type_name(n->ty),
			         n->name);
		int i = 0;
		Node head = {0};
		Node *tail = &head;
		Member *f = v->fields;
		for (Node *a = n->args; a;) {
			Node *next = a->next;
			a->next = NULL;
			if (!f) error_at(n->pos, "too many values for '%s'", n->name);
			a = coerce(a, f->ty);
			if (is_aggregate(f->ty)) {
				if (!same_type(f->ty, a->ty))
					error_at(a->pos, "value must have type '%s'", type_name(f->ty));
				tail->next = a;
			} else {
				a = to_value(a);
				tail->next = cast_to(a, f->ty);
			}
			tail = tail->next;
			f = f->next;
			i++;
			a = next;
		}
		if (i != v->nfields)
			error_at(n->pos, "'%s' needs %d more value(s)", n->name, v->nfields - i);
		n->args = head.next;
		n->variant = v;
		n->tmp = alloc_temp(n->ty);
		if (!n->tmp) error_at(n->pos, "an enum value needs a function scope");
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
		if (f->nct) { monomorphize_call(n, f); f = find_func(n->name); }
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
				if (want->size > 16) {
					a->tmp = alloc_temp(want);
					if (!a->tmp)
						error_at(a->pos, "this call needs a function scope");
				}
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
