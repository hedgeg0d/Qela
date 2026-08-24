#include "comp.h"

typedef struct VarScope VarScope;
struct VarScope {
	VarScope *next;
	Str       name;
	Var      *var;
};

typedef struct Scope Scope;
struct Scope {
	Scope    *next;
	VarScope *vars;
};

static Scope *scope;
static Func  *cur_fn;
static Str   *strs;
static int    nstrs;
static int    strs_cap;

/* Set while parsing the body of a `defer`: return, break and continue are
   rejected there, because codegen runs defer bodies while unwinding and
   another return would re-enter the same defer forever. */
static bool in_defer_body;

static Type *type_vars[6];
static int   ntype_vars;

static void enter_scope(void) {
	Scope *s = anew(Scope);
	*s = (Scope){.next = scope};
	scope = s;
}

static void leave_scope(void) { scope = scope->next; }

static bool eq(Token *t, const char *s) {
	return str_eq(t->text, str_from_cstr(s));
}

static Token *expect(Token *t, const char *s) {
	if (!eq(t, s)) error_at(t->pos, "expected '%c'", s);
	return t->next;
}

static bool consume(Token **t, const char *s) {
	if (!eq(*t, s)) return false;
	*t = (*t)->next;
	return true;
}

static Node *node(NodeKind kind, isize pos) {
	Node *n = anew(Node);
	*n = (Node){.kind = kind, .pos = pos};
	return n;
}

static Node *binary(NodeKind kind, Node *lhs, Node *rhs, isize pos) {
	Node *n = node(kind, pos);
	n->lhs = lhs;
	n->rhs = rhs;
	return n;
}

static Node *num_node(i64 v, isize pos) {
	Node *n = node(ND_NUM, pos);
	n->val = v;
	return n;
}

static Var *find_var(Str name) {
	for (Scope *s = scope; s; s = s->next)
		for (VarScope *v = s->vars; v; v = v->next)
			if (str_eq(v->name, name)) return v->var;
	return NULL;
}

static Var *globals;

static Var *declare(Str name, Type *ty, isize pos) {
	for (VarScope *v = scope->vars; v; v = v->next)
		if (str_eq(v->name, name)) error_at(pos, "redeclaration of '%s'", name);

	Var *var = anew(Var);
	*var = (Var){.name = name, .ty = ty};
	if (cur_fn) {
		var->next = cur_fn->locals;
		cur_fn->locals = var;
	} else {
		var->is_global = true;
		var->next = globals;
		globals = var;
	}

	VarScope *vs = anew(VarScope);
	*vs = (VarScope){.next = scope->vars, .name = name, .var = var};
	scope->vars = vs;
	return var;
}

static i64 trunc_to(i64 v, Type *ty) {
	switch (ty->size) {
	case 1: return ty->is_unsigned || ty->kind == TY_BOOL ? (i64)(u8)v : (i64)(i8)v;
	case 2: return ty->is_unsigned ? (i64)(u16)v : (i64)(i16)v;
	case 4: return ty->is_unsigned ? (i64)(u32)v : (i64)(i32)v;
	default: return v;
	}
}

static i64 eval_const(Node *n) {
	switch (n->kind) {
	case ND_NUM: return n->val;
	case ND_NEG: return -eval_const(n->lhs);
	case ND_BITNOT: return ~eval_const(n->lhs);
	case ND_NOT: return !eval_const(n->lhs);
	case ND_CAST:
		return n->ty->kind == TY_BOOL ? (eval_const(n->lhs) != 0)
		                              : trunc_to(eval_const(n->lhs), n->ty);
	case ND_ADD: return eval_const(n->lhs) + eval_const(n->rhs);
	case ND_SUB: return eval_const(n->lhs) - eval_const(n->rhs);
	case ND_MUL: return eval_const(n->lhs) * eval_const(n->rhs);
	case ND_BITAND: return eval_const(n->lhs) & eval_const(n->rhs);
	case ND_BITOR: return eval_const(n->lhs) | eval_const(n->rhs);
	case ND_BITXOR: return eval_const(n->lhs) ^ eval_const(n->rhs);
	case ND_SHL: return eval_const(n->lhs) << eval_const(n->rhs);
	case ND_SHR: return eval_const(n->lhs) >> eval_const(n->rhs);
	case ND_EQ: return eval_const(n->lhs) == eval_const(n->rhs);
	case ND_NE: return eval_const(n->lhs) != eval_const(n->rhs);
	case ND_LT: return eval_const(n->lhs) < eval_const(n->rhs);
	case ND_LE: return eval_const(n->lhs) <= eval_const(n->rhs);
	case ND_DIV:
	case ND_MOD: {
		i64 d = eval_const(n->rhs);
		if (d == 0) error_at(n->pos, "division by zero in a constant expression");
		return n->kind == ND_DIV ? eval_const(n->lhs) / d : eval_const(n->lhs) % d;
	}
	default:
		error_at(n->pos, "initializer must be a constant expression");
	}
}

static int intern_str(Str s) {
	if (nstrs == strs_cap) {
		int cap = strs_cap ? strs_cap * 2 : 16;
		Str *p = anew_n(Str, cap);
		memcpy(p, strs, sizeof(Str) * (usize)nstrs);
		strs = p;
		strs_cap = cap;
	}
	strs[nstrs] = s;
	return nstrs++;
}

static Type *parse_type(Token **t) {
	if (consume(t, "*")) return type_ptr(parse_type(t));
	if (eq(*t, "[")) {
		*t = (*t)->next;
		if (consume(t, "]")) {
			Type *base = parse_type(t);
			if (base->size == 0) error_at((*t)->pos, "cannot make a slice of that type");
			return type_slice(base);
		}
		if ((*t)->kind != TK_NUM) error_at((*t)->pos, "expected an array length");
		i64 len = (*t)->val;
		if (len <= 0) error_at((*t)->pos, "an array length must be positive");
		Token *lp = *t;
		*t = expect((*t)->next, "]");
		Type *base = parse_type(t);
		if (base->size == 0) error_at((*t)->pos, "cannot make an array of that type");
		if (base->size && len > (i64)0x7fffffff / base->size)
			error_at(lp->pos, "array is too large");
		return type_array(base, len);
	}
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a type name");
	for (int i = 0; i < ntype_vars; i++)
		if (str_eq(type_vars[i]->name, (*t)->text)) {
			Type *tv = type_vars[i];
			*t = (*t)->next;
			return tv;
		}
	Type *ty = type_lookup((*t)->text);
	if (!ty) error_at((*t)->pos, "unknown type '%s'", (*t)->text);
	*t = (*t)->next;
	return ty;
}


static Node *expr(Token **t);
static Node *assign(Token **t);
static Node *block(Token **t);

static Node *call_args(Token **t, Node *n) {
	*t = expect(*t, "(");
	Node head = {0};
	Node *tail = &head;
	while (!eq(*t, ")")) {
		if (tail != &head) *t = expect(*t, ",");
		tail->next = assign(t);
		tail = tail->next;
		n->nargs++;
	}
	*t = expect(*t, ")");
	n->args = head.next;
	return n;
}

/* Name{field: value, ...}. Fields may come in any order; the ones left out
   are zeroed. */
static Node *struct_lit(Token **t, Type *ty, isize pos) {
	Node *n = node(ND_STRUCTLIT, pos);
	n->ty = ty;
	*t = expect(*t, "{");

	Node head = {0};
	Node *tail = &head;
	while (!eq(*t, "}")) {
		if (tail != &head) *t = expect(*t, ",");
		if (eq(*t, "}")) break;
		if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a field name");
		Node *f = node(ND_FIELD, (*t)->pos);
		f->name = (*t)->text;
		*t = (*t)->next;
		*t = expect(*t, ":");
		f->lhs = assign(t);
		tail->next = f;
		tail = f;
		n->nargs++;
	}
	*t = expect(*t, "}");
	n->args = head.next;
	return n;
}

static Node *primary(Token **t) {
	Token *tok = *t;

	if (eq(tok, "(")) {
		*t = tok->next;
		Node *n = expr(t);
		*t = expect(*t, ")");
		return n;
	}

	if (tok->kind == TK_NUM) {
		*t = tok->next;
		return num_node(tok->val, tok->pos);
	}

	if (eq(tok, "true") || eq(tok, "false")) {
		*t = tok->next;
		Node *n = num_node(eq(tok, "true"), tok->pos);
		n->ty = ty_bool;
		return n;
	}

	if (tok->kind == TK_STR) {
		Node *n = node(ND_STRLIT, tok->pos);
		n->val = intern_str(tok->str);
		*t = tok->next;
		return n;
	}

	if (eq(tok, "comptime")) {
		*t = tok->next;
		Node *n = node(ND_COMPTIME, tok->pos);
		n->body = block(t);
		return n;
	}

	if (eq(tok, "sizeof")) {
		*t = tok->next;
		*t = expect(*t, "(");
		Type *ty = parse_type(t);
		*t = expect(*t, ")");
		Node *n = node(ND_SIZEOF, tok->pos);
		n->typeval = ty;
		return n;
	}

	if (tok->kind == TK_IDENT) {
		*t = tok->next;
		Type *et = type_lookup(tok->text);
		if (et && et->kind == TY_ENUM && eq(*t, ".")) {
			*t = (*t)->next;
			if ((*t)->kind != TK_IDENT)
				error_at((*t)->pos, "expected a variant name");
			Node *n = node(ND_ENUMLIT, tok->pos);
			n->ty = et;
			n->name = (*t)->text;
			*t = (*t)->next;
			if (eq(*t, "(")) call_args(t, n);
			return n;
		}
		Type *tt = et;
		if (!tt)
			for (int i = 0; i < ntype_vars; i++)
				if (str_eq(type_vars[i]->name, tok->text)) tt = type_vars[i];
		if (tt && tt->kind == TY_STRUCT && eq(*t, "{"))
			return struct_lit(t, tt, tok->pos);
		if (tt && !eq(*t, "(")) {
			Node *n = node(ND_TYPEEXPR, tok->pos);
			n->typeval = tt;
			return n;
		}
		if (eq(*t, "(")) {
			NodeKind kind = ND_CALL;
			if (str_eq(tok->text, S("syscall"))) kind = ND_SYSCALL;
			Node *n = node(kind, tok->pos);
			n->name = tok->text;
			call_args(t, n);
			if (kind == ND_SYSCALL && (n->nargs < 1 || n->nargs > 7))
				error_at(tok->pos, "syscall takes 1 to 7 arguments");
			return n;
		}
		Var *v = find_var(tok->text);
		if (!v) error_at(tok->pos, "undefined variable '%s'", tok->text);
		Node *n = node(ND_VAR, tok->pos);
		n->var = v;
		return n;
	}

	error_at(tok->pos, "expected an expression");
}

static Node *postfix(Token **t) {
	Node *n = primary(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "[")) {
			Node *lo = NULL;
			if (!eq(*t, "..")) lo = expr(t);
			if (consume(t, "..")) {
				Node *s = node(ND_SLICE, pos);
				s->lhs = n;
				s->rhs = lo;
				if (!eq(*t, "]")) s->then = expr(t);
				*t = expect(*t, "]");
				n = s;
				continue;
			}
			if (!lo) error_at((*t)->pos, "expected an index");
			*t = expect(*t, "]");
			n = binary(ND_INDEX, n, lo, pos);
			continue;
		}
		if (consume(t, ".")) {
			if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a field name");
			Node *m = node(ND_MEMBER, pos);
			m->lhs = n;
			m->name = (*t)->text;
			*t = (*t)->next;
			n = m;
			continue;
		}
		return n;
	}
}

static Node *unary(Token **t) {
	Token *tok = *t;
	if (consume(t, "-")) {
		Node *n = node(ND_NEG, tok->pos);
		n->lhs = unary(t);
		return n;
	}
	if (consume(t, "!")) {
		Node *n = node(ND_NOT, tok->pos);
		n->lhs = unary(t);
		return n;
	}
	if (consume(t, "~")) {
		Node *n = node(ND_BITNOT, tok->pos);
		n->lhs = unary(t);
		return n;
	}
	if (consume(t, "&")) {
		Node *n = node(ND_ADDR, tok->pos);
		n->lhs = unary(t);
		return n;
	}
	if (consume(t, "*")) {
		Node *n = node(ND_DEREF, tok->pos);
		n->lhs = unary(t);
		return n;
	}
	if (consume(t, "+")) return unary(t);
	return postfix(t);
}

static Node *cast(Token **t) {
	Node *n = unary(t);
	while (eq(*t, "as")) {
		isize pos = (*t)->pos;
		*t = (*t)->next;
		Node *c = node(ND_CAST, pos);
		c->lhs = n;
		c->ty = parse_type(t);
		n = c;
	}
	return n;
}

static Node *mul(Token **t) {
	Node *n = cast(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "*")) n = binary(ND_MUL, n, cast(t), pos);
		else if (consume(t, "/")) n = binary(ND_DIV, n, cast(t), pos);
		else if (consume(t, "%")) n = binary(ND_MOD, n, cast(t), pos);
		else if (consume(t, "<<")) n = binary(ND_SHL, n, cast(t), pos);
		else if (consume(t, ">>")) n = binary(ND_SHR, n, cast(t), pos);
		else if (consume(t, "&")) n = binary(ND_BITAND, n, cast(t), pos);
		else return n;
	}
}

static Node *add(Token **t) {
	Node *n = mul(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "+")) n = binary(ND_ADD, n, mul(t), pos);
		else if (consume(t, "-")) n = binary(ND_SUB, n, mul(t), pos);
		else if (consume(t, "|")) n = binary(ND_BITOR, n, mul(t), pos);
		else if (consume(t, "^")) n = binary(ND_BITXOR, n, mul(t), pos);
		else return n;
	}
}

static Node *relational(Token **t) {
	Node *n = add(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "<")) n = binary(ND_LT, n, add(t), pos);
		else if (consume(t, "<=")) n = binary(ND_LE, n, add(t), pos);
		else if (consume(t, ">")) n = binary(ND_LT, add(t), n, pos);
		else if (consume(t, ">=")) n = binary(ND_LE, add(t), n, pos);
		else return n;
	}
}

static Node *equality(Token **t) {
	Node *n = relational(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "==")) n = binary(ND_EQ, n, relational(t), pos);
		else if (consume(t, "!=")) n = binary(ND_NE, n, relational(t), pos);
		else return n;
	}
}

static Node *logand(Token **t) {
	Node *n = equality(t);
	isize pos;
	while (pos = (*t)->pos, consume(t, "&&")) n = binary(ND_AND, n, equality(t), pos);
	return n;
}

static Node *logor(Token **t) {
	Node *n = logand(t);
	isize pos;
	while (pos = (*t)->pos, consume(t, "||")) n = binary(ND_OR, n, logand(t), pos);
	return n;
}

static const struct {
	const char *op;
	NodeKind    kind;
} opassign[] = {
    {"+=", ND_ADD},    {"-=", ND_SUB},    {"*=", ND_MUL},    {"/=", ND_DIV},
    {"%=", ND_MOD},    {"&=", ND_BITAND}, {"|=", ND_BITOR},  {"^=", ND_BITXOR},
    {"<<=", ND_SHL},   {">>=", ND_SHR},   {NULL, ND_NUM},
};

static void check_lvalue(Node *n, isize pos) {
	if (n->kind != ND_VAR && n->kind != ND_DEREF && n->kind != ND_MEMBER &&
	    n->kind != ND_INDEX)
		error_at(pos, "cannot assign to this expression");
}

static Node *assign(Token **t) {
	Node *n = logor(t);
	isize pos = (*t)->pos;

	if (consume(t, "=")) {
		check_lvalue(n, pos);
		return binary(ND_ASSIGN, n, assign(t), pos);
	}
	for (int i = 0; opassign[i].op; i++) {
		if (!eq(*t, opassign[i].op)) continue;
		*t = (*t)->next;
		check_lvalue(n, pos);
		Node *a = binary(ND_OPASSIGN, n, assign(t), pos);
		a->val = opassign[i].kind;
		return a;
	}
	return n;
}

static Node *expr(Token **t) {
	Node *n = assign(t);
	isize pos = (*t)->pos;
	if (consume(t, ",")) return binary(ND_COMMA, n, expr(t), pos);
	return n;
}

static Node *stmt(Token **t);

static Node *block(Token **t) {
	Node *n = node(ND_BLOCK, (*t)->pos);
	*t = expect(*t, "{");
	enter_scope();
	Node head = {0};
	Node *tail = &head;
	while (!eq(*t, "}")) {
		if ((*t)->kind == TK_EOF) error_at((*t)->pos, "unclosed block");
		tail->next = stmt(t);
		tail = tail->next;
	}
	leave_scope();
	*t = expect(*t, "}");
	n->body = head.next;
	return n;
}

static Node *declaration(Token **t) {
	Token *kw = *t;
	bool typed = eq(kw, "var");
	*t = kw->next;

	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a variable name");
	Token *name = *t;
	*t = name->next;

	Type *ty = NULL;
	if (typed && !eq(*t, "=")) ty = parse_type(t);

	Node *n = node(ND_EXPRSTMT, kw->pos);
	if (consume(t, "=")) {
		Node *rhs = assign(t);
		if (!ty) {
			add_type(rhs);
			ty = rhs->ty;
			if (ty->kind == TY_VOID)
				error_at(name->pos, "cannot infer a type from a void expression");
		}
		Node *lhs = node(ND_VAR, name->pos);
		lhs->var = declare(name->text, ty, name->pos);
		n->lhs = binary(ND_ASSIGN, lhs, rhs, name->pos);
	} else {
		if (!ty) error_at(name->pos, "'let' requires an initializer");
		Node *z = node(ND_ZERO, name->pos);
		z->var = declare(name->text, ty, name->pos);
		n->lhs = z;
	}
	*t = expect(*t, ";");
	return n;
}

static Node *stmt(Token **t) {
	Token *tok = *t;

	if (eq(tok, "{")) return block(t);

	if (eq(tok, "return")) {
		if (in_defer_body) error_at(tok->pos, "cannot return from inside a defer body");
		*t = tok->next;
		Node *n = node(ND_RET, tok->pos);
		if (!eq(*t, ";")) n->lhs = expr(t);
		*t = expect(*t, ";");
		return n;
	}

	if (eq(tok, "if")) {
		*t = expect(tok->next, "(");
		Node *n = node(ND_IF, tok->pos);
		n->cond = expr(t);
		*t = expect(*t, ")");
		n->then = stmt(t);
		if (consume(t, "else")) n->els = stmt(t);
		return n;
	}

	if (eq(tok, "while")) {
		*t = expect(tok->next, "(");
		Node *n = node(ND_FOR, tok->pos);
		n->cond = expr(t);
		*t = expect(*t, ")");
		n->body = stmt(t);
		return n;
	}

	if (eq(tok, "break") || eq(tok, "continue")) {
		if (in_defer_body)
			error_at(tok->pos, "cannot %s from inside a defer body",
			         eq(tok, "break") ? "break" : "continue");
		Node *n = node(eq(tok, "break") ? ND_BREAK : ND_CONT, tok->pos);
		*t = expect(tok->next, ";");
		return n;
	}

	if (eq(tok, "for")) {
		*t = tok->next;
		Node *n = node(ND_FOR, tok->pos);
		enter_scope();

		if ((*t)->kind == TK_IDENT && eq((*t)->next, "in")) {
			Token *name = *t;
			*t = name->next->next;
			Node *lo = assign(t);
			*t = expect(*t, "..");
			Node *hi = assign(t);

			Var *v = declare(name->text, ty_i64, name->pos);
			Node *iv = node(ND_VAR, name->pos);
			iv->var = v;

			Node *init = node(ND_EXPRSTMT, name->pos);
			init->lhs = binary(ND_ASSIGN, iv, lo, name->pos);
			n->init = init;

			Node *iv2 = node(ND_VAR, name->pos);
			iv2->var = v;
			n->cond = binary(ND_LT, iv2, hi, name->pos);

			Node *iv3 = node(ND_VAR, name->pos);
			iv3->var = v;
			Node *step = binary(ND_OPASSIGN, iv3, num_node(1, name->pos), name->pos);
			step->val = ND_ADD;
			n->step = step;
		} else {
			*t = expect(*t, "(");
			if (!eq(*t, ";")) {
				if (eq(*t, "let") || eq(*t, "var")) {
					n->init = declaration(t);
				} else {
					Node *e = node(ND_EXPRSTMT, (*t)->pos);
					e->lhs = expr(t);
					n->init = e;
					*t = expect(*t, ";");
				}
			} else {
				*t = (*t)->next;
			}
			if (!eq(*t, ";")) n->cond = expr(t);
			*t = expect(*t, ";");
			if (!eq(*t, ")")) n->step = expr(t);
			*t = expect(*t, ")");
		}

		n->body = stmt(t);
		leave_scope();
		return n;
	}

	if (eq(tok, "defer")) {
		*t = tok->next;
		Node *n = node(ND_DEFER, tok->pos);
		in_defer_body = true;
		n->lhs = stmt(t);
		in_defer_body = false;
		return n;
	}

	if (eq(tok, "match")) {
		*t = expect(tok->next, "(");
		Node *n = node(ND_MATCH, tok->pos);
		n->cond = expr(t);
		*t = expect(*t, ")");
		*t = expect(*t, "{");

		Node  head = {0};
		Node *tail = &head;
		while (!eq(*t, "}")) {
			if ((*t)->kind == TK_EOF) error_at((*t)->pos, "unclosed match");
			Node *arm = node(ND_ARM, (*t)->pos);
			if (!consume(t, "_")) {
				if ((*t)->kind != TK_IDENT)
					error_at((*t)->pos, "expected a variant name or '_'");
				arm->name = (*t)->text;
				*t = (*t)->next;
			}

			enter_scope();
			if (consume(t, "(")) {
				Node  bh = {0};
				Node *bt = &bh;
				while (!eq(*t, ")")) {
					if (bt != &bh) *t = expect(*t, ",");
					if ((*t)->kind != TK_IDENT)
						error_at((*t)->pos, "expected a binding name");
					Node *b = node(ND_VAR, (*t)->pos);
					b->var = declare((*t)->text, ty_i64, (*t)->pos);
					*t = (*t)->next;
					bt->next = b;
					bt = b;
				}
				*t = expect(*t, ")");
				arm->args = bh.next;
			}
			*t = expect(*t, "=>");
			arm->body = stmt(t);
			leave_scope();

			tail->next = arm;
			tail = arm;
		}
		*t = expect(*t, "}");
		if (!head.next) error_at(tok->pos, "match needs at least one arm");
		n->body = head.next;
		return n;
	}

	if (eq(tok, "let") || eq(tok, "var")) return declaration(t);

	Node *n = node(ND_EXPRSTMT, tok->pos);
	n->lhs = expr(t);
	*t = expect(*t, ";");
	return n;
}

static Func *function(Token **t) {
	Token *kw = *t;
	*t = expect(*t, "fn");
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a function name");

	Func *f = anew(Func);
	*f = (Func){.name = (*t)->text, .ret = ty_void, .pos = kw->pos};
	*t = (*t)->next;

	cur_fn = f;
	type_set_fn(f);
	enter_scope();

	*t = expect(*t, "(");
	int rwords = 0;
	int saved_ntype_vars = ntype_vars;
	while (!eq(*t, ")")) {
		if (f->nparams) *t = expect(*t, ",");

		if (eq(*t, "comptime")) {
			*t = (*t)->next;
			if ((*t)->kind != TK_IDENT)
				error_at((*t)->pos, "expected a parameter name after comptime");
			Token *name = *t;
			*t = name->next;
			*t = expect(*t, ":");
			if (f->nct != f->nparams)
				error_at(name->pos, "comptime parameters must come first");
			if (f->nct == 6) error_at(name->pos, "too many comptime parameters");
			Type *tv = type_new_var(name->text);
			type_vars[ntype_vars++] = tv;
			/* The parameter carries the type variable itself: monomorphization
			 * reads it back from here to know what to substitute. */
			Var *p = declare(name->text, tv, name->pos);
			parse_type(t);
			f->params[f->nparams++] = p;
			f->nct++;
			continue;
		}

		if ((*t)->kind != TK_IDENT)
			error_at((*t)->pos, "expected a parameter name");
		Token *name = *t;
		*t = name->next;
		Type *ty = parse_type(t);
		if (ty->kind == TY_ARRAY)
			error_at(name->pos, "an array must be passed by pointer or as a slice");
		rwords += is_aggregate(ty) && ty->size > 8 && ty->size <= 16 ? 2 : 1;
		if (rwords > 6)
			error_at(name->pos, "parameters do not fit in the argument registers");
		if (f->nparams == 6) error_at(name->pos, "at most 6 parameters are supported");
		Var *p = declare(name->text, ty, name->pos);
		p->by_ref = is_aggregate(ty) && ty->size > 16;
		f->params[f->nparams++] = p;
	}
	*t = expect(*t, ")");

	if (!eq(*t, "{")) {
		f->ret = parse_type(t);
		if (f->ret->kind == TY_ARRAY)
			error_at(kw->pos, "an array cannot be returned by value");
	}

	f->body = block(t);
	leave_scope();
	ntype_vars = saved_ntype_vars;

	cur_fn = NULL;
	type_set_fn(NULL);
	return f;
}

static void enum_decl(Token **t) {
	*t = expect(*t, "enum");
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected an enum name");
	Token *name = *t;
	*t = name->next;
	if (type_lookup(name->text))
		error_at(name->pos, "redefinition of type '%s'", name->text);

	Type *ty = anew(Type);
	*ty = (Type){.kind = TY_ENUM, .align = 8, .name = name->text};
	type_define(name->text, ty);

	*t = expect(*t, "{");
	Variant  head = {0};
	Variant *tail = &head;
	int tag = 0, maxpay = 0;

	while (!eq(*t, "}")) {
		if (tail != &head) *t = expect(*t, ",");
		if (eq(*t, "}")) break;
		if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a variant name");
		Token *vname = *t;
		*t = vname->next;
		for (Variant *v = head.next; v; v = v->next)
			if (str_eq(v->name, vname->text))
				error_at(vname->pos, "duplicate variant '%s'", vname->text);
		if (tag == 64) error_at(vname->pos, "at most 64 variants are supported");

		Variant *v = anew(Variant);
		*v = (Variant){.name = vname->text, .tag = tag++, .pos = vname->pos};

		if (consume(t, "(")) {
			Member  fh = {0};
			Member *ft = &fh;
			int     off = 8;
			while (!eq(*t, ")")) {
				if (ft != &fh) *t = expect(*t, ",");
				Type *fty = parse_type(t);
				if (fty->size == 0)
					error_at(vname->pos, "a variant field cannot have type 'void'");
				off = (off + fty->align - 1) & ~(fty->align - 1);
				Member *m = anew(Member);
				*m = (Member){.ty = fty, .offset = off, .pos = vname->pos};
				off += fty->size;
				ft->next = m;
				ft = m;
				v->nfields++;
			}
			*t = expect(*t, ")");
			if (!v->nfields) error_at(vname->pos, "empty payload list");
			v->fields = fh.next;
			if (off - 8 > maxpay) maxpay = off - 8;
		}
		tail->next = v;
		tail = v;
	}
	*t = expect(*t, "}");
	if (!head.next) error_at(name->pos, "an enum must have at least one variant");

	ty->variants = head.next;
	ty->size = (8 + maxpay + 7) & ~7;
}

static void struct_decl(Token **t) {
	*t = expect(*t, "struct");
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a struct name");
	Token *name = *t;
	*t = name->next;

	/* Forward declaration: struct Name; */
	if (consume(t, ";")) {
		if (!type_lookup(name->text)) {
			Type *ty = anew(Type);
			*ty = (Type){.kind = TY_STRUCT, .align = 8, .size = 8, .name = name->text};
			type_define(name->text, ty);
		}
		return;
	}

	Type *prev = type_lookup(name->text);
	if (prev && prev->members)
		error_at(name->pos, "redefinition of type '%s'", name->text);

	Type *ty;
	if (prev && prev->kind == TY_STRUCT) {
		ty = prev;
	} else {
		ty = anew(Type);
		*ty = (Type){.kind = TY_STRUCT, .align = 1, .name = name->text};
		type_define(name->text, ty);
	}

	*t = expect(*t, "{");
	Member head = {0};
	Member *tail = &head;
	int off = 0, align = 1;

	while (!eq(*t, "}")) {
		if (tail != &head) *t = expect(*t, ",");
		if (eq(*t, "}")) break;
		if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a field name");
		Token *fname = *t;
		*t = fname->next;
		Type *fty = parse_type(t);
		if (fty == ty)
			error_at(fname->pos, "a struct cannot contain itself by value");
		if (fty->size == 0) error_at(fname->pos, "a field cannot have type 'void'");
		for (Member *m = head.next; m; m = m->next)
			if (str_eq(m->name, fname->text))
				error_at(fname->pos, "duplicate field '%s'", fname->text);

		off = (off + fty->align - 1) & ~(fty->align - 1);
		Member *m = anew(Member);
		*m = (Member){.name = fname->text, .ty = fty, .offset = off, .pos = fname->pos};
		off += fty->size;
		if (fty->align > align) align = fty->align;
		tail->next = m;
		tail = m;
	}
	*t = expect(*t, "}");

	if (!head.next) error_at(name->pos, "a struct must have at least one field");
	ty->members = head.next;
	ty->align = align;
	ty->size = (off + align - 1) & ~(align - 1);
}

static void global_decl(Token **t) {
	Token *kw = *t;
	bool typed = eq(kw, "var");
	*t = kw->next;

	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a variable name");
	Token *name = *t;
	*t = name->next;

	Type *ty = NULL;
	if (typed && !eq(*t, "=")) ty = parse_type(t);

	i64 init = 0;
	if (consume(t, "=")) {
		Node *rhs = assign(t);
		add_type(rhs);
		if (!ty) ty = rhs->ty;
		if (ty->kind == TY_PTR)
			error_at(name->pos, "a global pointer cannot have an initializer yet");
		if (is_aggregate(ty))
			error_at(name->pos, "a global aggregate cannot have an initializer yet");
		init = trunc_to(eval_const(rhs), ty);
	} else if (!ty) {
		error_at(name->pos, "'let' requires an initializer");
	}

	declare(name->text, ty, name->pos)->init = init;
	*t = expect(*t, ";");
}

static Func *all_funcs;
static Func *funcs_tail;

void add_func(Func *f) {
	if (!all_funcs) {
		all_funcs = f;
	} else {
		funcs_tail->next = f;
	}
	funcs_tail = f;
}

Func *find_func(Str name) {
	for (Func *f = all_funcs; f; f = f->next)
		if (str_eq(f->name, name)) return f;
	return NULL;
}

Unit parse(Token *tok) {
	enter_scope();
	Func head = {0};
	Func *tail = &head;
	while (tok->kind != TK_EOF) {
		if (eq(tok, "struct")) {
			struct_decl(&tok);
			continue;
		}
		if (eq(tok, "enum")) {
			enum_decl(&tok);
			continue;
		}
		if (eq(tok, "let") || eq(tok, "var")) {
			global_decl(&tok);
			continue;
		}
		if (eq(tok, "import")) {
			isize ipos = tok->pos;
			tok = tok->next;
			if (tok->kind != TK_STR)
				error_at(tok->pos, "import needs a string path");
			Str raw = tok->str;
			tok = tok->next;
			tok = expect(tok, ";");

			/* Resolve relative to the importing file, which is the file the
			   `import` keyword came from — not whatever follows the `;`. */
			char buf[4096];
			int cur_file = (int)(ipos >> FILE_SHIFT);
			int dir_len = diag_file_dir(cur_file, buf, sizeof(buf) - 256);
			char *p = buf + dir_len;
			memcpy(p, raw.p, (usize)raw.n);
			p[raw.n] = 0;

			if (!diag_already_imported(buf)) {
				Str src = read_file(buf);
				int fid = diag_add_file(buf, src);
				Token *head = lex(src, (isize)fid << FILE_SHIFT);
				/* Splice imported tokens between current position and
				   the main file's remaining tokens. Skip the imported
				   EOF token — the parser should continue past it. */
				Token *tail2 = head;
				while (tail2->next && tail2->next->kind != TK_EOF)
					tail2 = tail2->next;
				tail2->next = tok;
				tok = head;
			}
			continue;
		}
		tail->next = function(&tok);
		tail = tail->next;
		all_funcs = head.next;
	}
	leave_scope();

	all_funcs = head.next;
	funcs_tail = tail;
	for (Func *f = all_funcs; f; f = f->next) {
		if (find_func(f->name) != f)
			error_at(f->pos, "redefinition of function '%s'", f->name);
		if (f->nct) continue;
		type_func(f);
		opt_func(f);

		int off = 0;
		for (Var *v = f->locals; v; v = v->next) {
			int size = v->by_ref ? 8 : v->ty->size < 8 ? 8 : v->ty->size;
			int al = v->by_ref ? 8 : v->ty->align;
			off = (off + size + al - 1) & ~(al - 1);
			v->offset = off;
		}
		f->stack_size = (off + 15) & ~15;
	}
	return (Unit){
	    .funcs = all_funcs, .globals = globals, .strs = strs, .nstrs = nstrs};
}
