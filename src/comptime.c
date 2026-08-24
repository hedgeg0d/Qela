#include "comp.h"

typedef struct CTVar CTVar;
struct CTVar {
	CTVar      *next;
	Str         name;
	ComptimeVal val;
};

typedef struct CTFrame CTFrame;
struct CTFrame {
	CTFrame *prev;
	CTVar   *vars;
};

static CTFrame *ct_stack;

/* Control-flow state, cleared at the top of every ct_eval_block. */
static bool        ct_returning;
static ComptimeVal ct_ret_val;
static bool        ct_broke;
static bool        ct_continued;

static void ct_push_frame(void) {
	CTFrame *f = anew(CTFrame);
	*f = (CTFrame){.prev = ct_stack};
	ct_stack = f;
}

static void ct_pop_frame(void) { ct_stack = ct_stack->prev; }

static CTVar *ct_find(Str name) {
	for (CTFrame *f = ct_stack; f; f = f->prev)
		for (CTVar *v = f->vars; v; v = v->next)
			if (str_eq(v->name, name)) return v;
	return NULL;
}

static CTVar *ct_declare(Str name, ComptimeVal val) {
	CTVar *v = anew(CTVar);
	*v = (CTVar){.next = ct_stack->vars, .name = name, .val = val};
	ct_stack->vars = v;
	return v;
}

static ComptimeVal ct_int(i64 v) {
	return (ComptimeVal){.kind = CV_INT, .num = v};
}

static ComptimeVal ct_void(void) {
	return (ComptimeVal){.kind = CV_VOID};
}

static ComptimeVal ct_eval_expr(Node *n);
static ComptimeVal ct_eval_block(Node *n);
static void        ct_eval_for(Node *n);

static ComptimeVal ct_call(Func *f, Node *args) {
	/* A call is a boundary: a callee's return, break or continue must not
	   leak into the caller, or the caller's block would stop at the first
	   statement after the call and return the callee's value. */
	bool        save_r = ct_returning;
	bool        save_b = ct_broke;
	bool        save_c = ct_continued;
	ComptimeVal save_v = ct_ret_val;
	ct_push_frame();
	int i = 0;
	for (Node *a = args; a; a = a->next, i++) {
		if (i >= f->nparams) break;
		ct_declare(f->params[i]->name, ct_eval_expr(a));
	}
	ComptimeVal ret = ct_eval_block(f->body);
	ct_pop_frame();
	ct_returning = save_r;
	ct_broke = save_b;
	ct_continued = save_c;
	ct_ret_val = save_v;
	return ret;
}

static void ct_run_stmt(Node *s) {
	if (!s || ct_returning || ct_broke || ct_continued) return;
	switch (s->kind) {
	case ND_RET:
		ct_ret_val = s->lhs ? ct_eval_expr(s->lhs) : ct_void();
		ct_returning = true;
		return;
	case ND_EXPRSTMT:
		if (s->lhs) ct_eval_expr(s->lhs);
		return;
	case ND_BLOCK:
		ct_eval_block(s);
		return;
	case ND_IF: {
		ComptimeVal c = ct_eval_expr(s->cond);
		Node *br = c.num ? s->then : s->els;
		ct_run_stmt(br);
		return;
	}
	case ND_FOR:
		ct_eval_for(s);
		return;
	case ND_BREAK:
		ct_broke = true;
		return;
	case ND_CONT:
		ct_continued = true;
		return;
	case ND_DEFER:
		return;
	default:
		ct_eval_expr(s);
		return;
	}
}

static void ct_eval_for(Node *n) {
	if (n->init) ct_run_stmt(n->init);
	while (!ct_returning) {
		if (n->cond) {
			ComptimeVal c = ct_eval_expr(n->cond);
			if (!c.num) break;
		}
		ct_broke = false;
		ct_continued = false;
		ct_run_stmt(n->body);
		if (ct_broke) { ct_broke = false; break; }
		if (ct_returning) break;
		if (n->step) ct_run_stmt(n->step);
	}
}

ComptimeVal ct_eval_block(Node *n) {
	ct_push_frame();
	ct_returning = false;
	ct_broke = false;
	ct_continued = false;
	for (Node *s = n->body; s && !ct_returning && !ct_broke && !ct_continued; s = s->next)
		ct_run_stmt(s);
	ct_pop_frame();
	return ct_returning ? ct_ret_val : ct_void();
}

static ComptimeVal ct_eval_expr(Node *n) {
	if (!n) return ct_void();
	switch (n->kind) {
	case ND_NUM:
		return ct_int(n->val);

	case ND_STRLIT:
		return (ComptimeVal){.kind = CV_STR, .str = n->name};

	case ND_VAR: {
		CTVar *v = ct_find(n->var->name);
		if (v) return v->val;
		if (n->var->is_global) return ct_int(n->var->init);
		error_at(n->pos, "undefined comptime variable '%s'", n->var->name);
	}

	case ND_SIZEOF:
		return ct_int(n->typeval->size);

	case ND_TYPEEXPR:
		error_at(n->pos, "cannot use a type value in a comptime expression");

	case ND_CAST: {
		ComptimeVal v = ct_eval_expr(n->lhs);
		if (n->ty->kind == TY_BOOL) return ct_int(v.num != 0);
		switch (n->ty->size) {
		case 1: return ct_int(n->ty->is_unsigned ? (i64)(u8)v.num : (i64)(i8)v.num);
		case 2: return ct_int(n->ty->is_unsigned ? (i64)(u16)v.num : (i64)(i16)v.num);
		case 4: return ct_int(n->ty->is_unsigned ? (i64)(u32)v.num : (i64)(i32)v.num);
		default: return v;
		}
	}

	case ND_ADD: return ct_int(ct_eval_expr(n->lhs).num + ct_eval_expr(n->rhs).num);
	case ND_SUB: return ct_int(ct_eval_expr(n->lhs).num - ct_eval_expr(n->rhs).num);
	case ND_MUL: return ct_int(ct_eval_expr(n->lhs).num * ct_eval_expr(n->rhs).num);
	case ND_DIV: {
		i64 d = ct_eval_expr(n->rhs).num;
		if (!d) error_at(n->pos, "division by zero in comptime");
		return ct_int(ct_eval_expr(n->lhs).num / d);
	}
	case ND_MOD: {
		i64 d = ct_eval_expr(n->rhs).num;
		if (!d) error_at(n->pos, "division by zero in comptime");
		return ct_int(ct_eval_expr(n->lhs).num % d);
	}
	case ND_BITAND: return ct_int(ct_eval_expr(n->lhs).num & ct_eval_expr(n->rhs).num);
	case ND_BITOR:  return ct_int(ct_eval_expr(n->lhs).num | ct_eval_expr(n->rhs).num);
	case ND_BITXOR: return ct_int(ct_eval_expr(n->lhs).num ^ ct_eval_expr(n->rhs).num);
	case ND_SHL:    return ct_int(ct_eval_expr(n->lhs).num << ct_eval_expr(n->rhs).num);
	case ND_SHR:    return ct_int(ct_eval_expr(n->lhs).num >> ct_eval_expr(n->rhs).num);

	case ND_EQ: return ct_int(ct_eval_expr(n->lhs).num == ct_eval_expr(n->rhs).num);
	case ND_NE: return ct_int(ct_eval_expr(n->lhs).num != ct_eval_expr(n->rhs).num);
	case ND_LT: return ct_int(ct_eval_expr(n->lhs).num < ct_eval_expr(n->rhs).num);
	case ND_LE: return ct_int(ct_eval_expr(n->lhs).num <= ct_eval_expr(n->rhs).num);

	case ND_NOT: return ct_int(!ct_eval_expr(n->lhs).num);
	case ND_AND: {
		if (!ct_eval_expr(n->lhs).num) return ct_int(0);
		return ct_int(ct_eval_expr(n->rhs).num != 0);
	}
	case ND_OR: {
		if (ct_eval_expr(n->lhs).num) return ct_int(1);
		return ct_int(ct_eval_expr(n->rhs).num != 0);
	}

	case ND_NEG: return ct_int(-ct_eval_expr(n->lhs).num);
	case ND_BITNOT: return ct_int(~ct_eval_expr(n->lhs).num);

	case ND_COMMA: {
		ct_eval_expr(n->lhs);
		return ct_eval_expr(n->rhs);
	}

	case ND_ASSIGN: {
		if (n->lhs->kind == ND_VAR) {
			CTVar *v = ct_find(n->lhs->var->name);
			ComptimeVal val = ct_eval_expr(n->rhs);
			if (v) v->val = val;
			else ct_declare(n->lhs->var->name, val);
			return val;
		}
		error_at(n->pos, "unsupported comptime assignment target");
	}

	case ND_OPASSIGN: {
		if (n->lhs->kind == ND_VAR) {
			CTVar *v = ct_find(n->lhs->var->name);
			if (!v) error_at(n->pos, "undefined comptime variable '%s'",
			                 n->lhs->var->name);
			ComptimeVal rhs = ct_eval_expr(n->rhs);
			i64 a = v->val.num, b = rhs.num, r = 0;
			switch (n->val) {
			case ND_ADD: r = a + b; break;
			case ND_SUB: r = a - b; break;
			case ND_MUL: r = a * b; break;
			case ND_DIV:
				if (!b) error_at(n->pos, "division by zero in a constant expression");
				r = a / b; break;
			case ND_MOD:
				if (!b) error_at(n->pos, "division by zero in a constant expression");
				r = a % b; break;
			case ND_BITAND: r = a & b; break;
			case ND_BITOR:  r = a | b; break;
			case ND_BITXOR: r = a ^ b; break;
			case ND_SHL: r = a << b; break;
			case ND_SHR: r = a >> b; break;
			default: error_at(n->pos, "unsupported comptime op-assign");
			}
			v->val = ct_int(r);
			return v->val;
		}
		error_at(n->pos, "unsupported comptime op-assign target");
	}

	case ND_CALL: {
		Func *f = find_func(n->name);
		if (!f) error_at(n->pos, "undefined function '%s'", n->name);
		return ct_call(f, n->args);
	}

	case ND_IF: {
		ComptimeVal c = ct_eval_expr(n->cond);
		Node *branch = c.num ? n->then : n->els;
		if (!branch) return ct_void();
		if (branch->kind == ND_BLOCK) {
			ct_eval_block(branch);
			return ct_returning ? ct_ret_val : ct_void();
		}
		ct_returning = false;
		ct_run_stmt(branch);
		return ct_returning ? ct_ret_val : ct_void();
	}

	case ND_BLOCK:
		ct_eval_block(n);
		return ct_returning ? ct_ret_val : ct_void();

	case ND_COMPTIME:
		ct_eval_block(n->body);
		return ct_returning ? ct_ret_val : ct_void();

	default:
		error_at(n->pos, "unsupported expression in comptime");
	}
}

ComptimeVal comptime_eval(Node *n) {
	ct_returning = false;
	ct_broke = false;
	ct_continued = false;
	ct_push_frame();
	ComptimeVal v = ct_eval_expr(n);
	ct_pop_frame();
	return v;
}
