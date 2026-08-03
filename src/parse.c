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
static Var   *cur_locals;
static Func  *cur_fn;
static Str   *strs;
static int    nstrs;
static int    strs_cap;

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

static Var *declare(Str name, Type *ty, isize pos) {
	for (VarScope *v = scope->vars; v; v = v->next)
		if (str_eq(v->name, name)) error_at(pos, "redeclaration of '%s'", name);

	Var *var = anew(Var);
	*var = (Var){.name = name, .ty = ty, .next = cur_locals};
	cur_locals = var;

	VarScope *vs = anew(VarScope);
	*vs = (VarScope){.next = scope->vars, .name = name, .var = var};
	scope->vars = vs;
	return var;
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
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a type name");
	Type *ty = type_lookup((*t)->text);
	if (!ty) error_at((*t)->pos, "unknown type '%s'", (*t)->text);
	*t = (*t)->next;
	return ty;
}


static Node *expr(Token **t);
static Node *assign(Token **t);

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

	if (tok->kind == TK_STR) {
		Node *n = node(ND_STRLIT, tok->pos);
		n->val = intern_str(tok->str);
		*t = tok->next;
		return n;
	}

	if (tok->kind == TK_IDENT) {
		*t = tok->next;
		if (eq(*t, "(")) {
			NodeKind kind = ND_CALL;
			if (str_eq(tok->text, S("syscall"))) kind = ND_SYSCALL;
			else if (str_eq(tok->text, S("cstrlen"))) kind = ND_CSTRLEN;
			Node *n = node(kind, tok->pos);
			n->name = tok->text;
			call_args(t, n);
			if (kind == ND_SYSCALL && (n->nargs < 1 || n->nargs > 7))
				error_at(tok->pos, "syscall takes 1 to 7 arguments");
			if (kind == ND_CSTRLEN && n->nargs != 1)
				error_at(tok->pos, "cstrlen takes exactly 1 argument");
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
	return primary(t);
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
	if (n->kind != ND_VAR && n->kind != ND_DEREF)
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
		declare(name->text, ty, name->pos);
	}
	*t = expect(*t, ";");
	return n;
}

static Node *stmt(Token **t) {
	Token *tok = *t;

	if (eq(tok, "{")) return block(t);

	if (eq(tok, "return")) {
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

	cur_locals = NULL;
	cur_fn = f;
	enter_scope();

	*t = expect(*t, "(");
	while (!eq(*t, ")")) {
		if (f->nparams) *t = expect(*t, ",");
		if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a parameter name");
		Token *name = *t;
		*t = name->next;
		Type *ty = parse_type(t);
		if (f->nparams == 6) error_at(name->pos, "at most 6 parameters are supported");
		f->params[f->nparams++] = declare(name->text, ty, name->pos);
	}
	*t = expect(*t, ")");

	if (!eq(*t, "{")) f->ret = parse_type(t);

	f->body = block(t);
	leave_scope();

	f->locals = cur_locals;

	int off = 0;
	for (Var *v = f->locals; v; v = v->next) {
		int size = v->ty->size < 8 ? 8 : v->ty->size;
		off = (off + size + v->ty->align - 1) & ~(v->ty->align - 1);
		v->offset = off;
	}
	f->stack_size = (off + 15) & ~15;
	return f;
}

static Func *all_funcs;

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
		tail->next = function(&tok);
		tail = tail->next;
	}
	leave_scope();

	all_funcs = head.next;
	for (Func *f = all_funcs; f; f = f->next) {
		if (find_func(f->name) != f)
			error_at(f->pos, "redefinition of function '%s'", f->name);
		add_type(f->body);
	}
	return (Unit){.funcs = all_funcs, .strs = strs, .nstrs = nstrs};
}
