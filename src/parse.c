#include "comp.h"

static Var  *cur_locals;
static Var  *cur_params;
static Str  *strs;
static int   nstrs;
static int   strs_cap;

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

static Var *find_var(Str name) {
	for (Var *v = cur_locals; v; v = v->next)
		if (str_eq(v->name, name)) return v;
	for (Var *v = cur_params; v; v = v->next)
		if (str_eq(v->name, name)) return v;
	return NULL;
}

static Var *new_local(Str name, isize pos) {
	if (find_var(name)) error_at(pos, "redeclaration of '%s'", name);
	Var *v = anew(Var);
	*v = (Var){.name = name, .next = cur_locals};
	cur_locals = v;
	return v;
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

static Node *expr(Token **t);

static Node *call_args(Token **t, Node *n) {
	*t = expect(*t, "(");
	Node head = {0};
	Node *tail = &head;
	while (!eq(*t, ")")) {
		if (tail != &head) *t = expect(*t, ",");
		tail->next = expr(t);
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
		Node *n = node(ND_NUM, tok->pos);
		n->val = tok->val;
		*t = tok->next;
		return n;
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
	if (consume(t, "+")) return unary(t);
	return primary(t);
}

static Node *mul(Token **t) {
	Node *n = unary(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "*")) n = binary(ND_MUL, n, unary(t), pos);
		else if (consume(t, "/")) n = binary(ND_DIV, n, unary(t), pos);
		else if (consume(t, "%")) n = binary(ND_MOD, n, unary(t), pos);
		else return n;
	}
}

static Node *add(Token **t) {
	Node *n = mul(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "+")) n = binary(ND_ADD, n, mul(t), pos);
		else if (consume(t, "-")) n = binary(ND_SUB, n, mul(t), pos);
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
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "&&")) n = binary(ND_AND, n, equality(t), pos);
		else return n;
	}
}

static Node *logor(Token **t) {
	Node *n = logand(t);
	for (;;) {
		isize pos = (*t)->pos;
		if (consume(t, "||")) n = binary(ND_OR, n, logand(t), pos);
		else return n;
	}
}

static Node *assign(Token **t) {
	Node *n = logor(t);
	isize pos = (*t)->pos;
	if (consume(t, "=")) {
		if (n->kind != ND_VAR) error_at(pos, "cannot assign to this expression");
		n = binary(ND_ASSIGN, n, assign(t), pos);
	}
	return n;
}

static Node *expr(Token **t) { return assign(t); }

static void skip_type(Token **t) {
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a type name");
	*t = (*t)->next;
}

static Node *stmt(Token **t);

static Node *block(Token **t) {
	Node *n = node(ND_BLOCK, (*t)->pos);
	*t = expect(*t, "{");
	Node head = {0};
	Node *tail = &head;
	while (!eq(*t, "}")) {
		if ((*t)->kind == TK_EOF) error_at((*t)->pos, "unclosed block");
		tail->next = stmt(t);
		tail = tail->next;
	}
	*t = expect(*t, "}");
	n->body = head.next;
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
		Node *n = node(ND_WHILE, tok->pos);
		n->cond = expr(t);
		*t = expect(*t, ")");
		n->body = stmt(t);
		return n;
	}

	if (eq(tok, "let") || eq(tok, "var")) {
		bool typed = eq(tok, "var");
		*t = tok->next;
		if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a variable name");
		Token *name = *t;
		*t = name->next;
		if (typed) skip_type(t);
		Var *v = new_local(name->text, name->pos);
		Node *n = node(ND_EXPRSTMT, tok->pos);
		if (consume(t, "=")) {
			Node *lhs = node(ND_VAR, name->pos);
			lhs->var = v;
			n->lhs = binary(ND_ASSIGN, lhs, expr(t), name->pos);
		}
		*t = expect(*t, ";");
		return n;
	}

	Node *n = node(ND_EXPRSTMT, tok->pos);
	n->lhs = expr(t);
	*t = expect(*t, ";");
	return n;
}

static Func *function(Token **t) {
	Token *tok = *t;
	*t = expect(*t, "fn");
	if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a function name");

	Func *f = anew(Func);
	*f = (Func){.name = (*t)->text};
	*t = (*t)->next;

	cur_locals = NULL;
	cur_params = NULL;

	*t = expect(*t, "(");
	Var *ptail = NULL;
	while (!eq(*t, ")")) {
		if (ptail) *t = expect(*t, ",");
		if ((*t)->kind != TK_IDENT) error_at((*t)->pos, "expected a parameter name");
		Var *v = anew(Var);
		*v = (Var){.name = (*t)->text};
		*t = (*t)->next;
		skip_type(t);
		if (ptail) ptail->next = v;
		else f->params = v;
		ptail = v;
		f->nparams++;
	}
	*t = expect(*t, ")");
	if (f->nparams > 6) error_at(tok->pos, "at most 6 parameters are supported");
	cur_params = f->params;

	if (!eq(*t, "{")) skip_type(t);

	f->body = block(t);
	f->locals = cur_locals;

	int off = 0;
	for (Var *v = f->params; v; v = v->next) v->offset = (off += 8);
	for (Var *v = f->locals; v; v = v->next) v->offset = (off += 8);
	f->stack_size = (off + 15) & ~15;
	return f;
}

Unit parse(Token *tok) {
	Func head = {0};
	Func *tail = &head;
	while (tok->kind != TK_EOF) {
		tail->next = function(&tok);
		tail = tail->next;
	}
	return (Unit){.funcs = head.next, .strs = strs, .nstrs = nstrs};
}
