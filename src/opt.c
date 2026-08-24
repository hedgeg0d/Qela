#include "comp.h"

static i64 trunc(i64 v, Type *t) {
	switch (t->size) {
	case 1: return t->is_unsigned || t->kind == TY_BOOL ? (i64)(u8)v : (i64)(i8)v;
	case 2: return t->is_unsigned ? (i64)(u16)v : (i64)(i16)v;
	case 4: return t->is_unsigned ? (i64)(u32)v : (i64)(i32)v;
	default: return v;
	}
}

static void const_fold(Node *n) {
	switch (n->kind) {
	case ND_NUM:
	case ND_STRLIT:
	case ND_VAR:
	case ND_COMPTIME:
		return;
	case ND_CAST:
		const_fold(n->lhs);
		if (n->lhs->kind == ND_NUM) {
			i64 v = n->lhs->val;
			if (n->ty->kind == TY_BOOL) n->val = (v != 0);
			else n->val = trunc(v, n->ty);
			n->kind = ND_NUM;
		}
		return;
	case ND_NEG:
		const_fold(n->lhs);
		if (n->lhs->kind == ND_NUM) { n->val = -n->lhs->val; n->kind = ND_NUM; }
		return;
	case ND_BITNOT:
		const_fold(n->lhs);
		if (n->lhs->kind == ND_NUM) { n->val = ~n->lhs->val; n->kind = ND_NUM; }
		return;
	case ND_NOT:
		const_fold(n->lhs);
		if (n->lhs->kind == ND_NUM) { n->val = !n->lhs->val; n->kind = ND_NUM; }
		return;
	case ND_ADD:
	case ND_SUB:
	case ND_MUL:
	case ND_DIV:
	case ND_MOD:
	case ND_BITAND:
	case ND_BITOR:
	case ND_BITXOR:
	case ND_SHL:
	case ND_SHR:
	case ND_EQ:
	case ND_NE:
	case ND_LT:
	case ND_LE:
	case ND_AND:
	case ND_OR:
		const_fold(n->lhs);
		/* Short-circuit: once the left side decides an && or ||, the right
		   side never runs at runtime, so folding it would both be wrong and
		   (with a constant division by zero in it) reject valid programs. */
		if (n->kind == ND_AND && n->lhs->kind == ND_NUM && !n->lhs->val)
			{ n->val = 0; n->kind = ND_NUM; return; }
		if (n->kind == ND_OR && n->lhs->kind == ND_NUM && n->lhs->val)
			{ n->val = 1; n->kind = ND_NUM; return; }
		const_fold(n->rhs);
		if (n->lhs->kind == ND_NUM && n->rhs->kind == ND_NUM) {
			i64 a = n->lhs->val, b = n->rhs->val;
			switch (n->kind) {
			case ND_ADD:    n->val = a + b; break;
			case ND_SUB:    n->val = a - b; break;
			case ND_MUL:    n->val = a * b; break;
			case ND_DIV:
				if (!b) error_at(n->pos, "division by zero in a constant expression");
				n->val = a / b; break;
			case ND_MOD:
				if (!b) error_at(n->pos, "division by zero in a constant expression");
				n->val = a % b; break;
			case ND_BITAND: n->val = a & b; break;
			case ND_BITOR:  n->val = a | b; break;
			case ND_BITXOR: n->val = a ^ b; break;
			case ND_SHL:    n->val = a << b; break;
			case ND_SHR:    n->val = a >> b; break;
			case ND_EQ:     n->val = a == b; break;
			case ND_NE:     n->val = a != b; break;
			case ND_LT:     n->val = a < b; break;
			case ND_LE:     n->val = a <= b; break;
			case ND_AND:    n->val = (a != 0) && (b != 0); break;
			case ND_OR:     n->val = (a != 0) || (b != 0); break;
			default: return;
			}
			n->kind = ND_NUM;
		}
		/* AND/OR with a constant right side */
		if (n->kind == ND_AND && n->rhs->kind == ND_NUM && !n->rhs->val)
			{ n->val = 0; n->kind = ND_NUM; }
		if (n->kind == ND_OR && n->rhs->kind == ND_NUM && n->rhs->val)
			{ n->val = 1; n->kind = ND_NUM; }
		return;
	default:
		return;
	}
}

/* Recurse into block children. */
static void fold_body(Node *n) {
	if (!n) return;
	const_fold(n);
	/* A folded node is a literal now: its children were consumed (or, for
	   a short-circuited && / ||, never evaluated) and must not be
	   visited. */
	if (n->kind == ND_NUM) return;
	fold_body(n->lhs);
	fold_body(n->rhs);
	fold_body(n->cond);
	fold_body(n->then);
	fold_body(n->els);
	fold_body(n->init);
	fold_body(n->step);
	fold_body(n->body);
	for (Node *a = n->args; a; a = a->next) fold_body(a);
	fold_body(n->next);
}

void opt_func(Func *f) {
	if (!f->body) return;
	/* Recursively fold constants in the function body and in all nested
	   expressions used by the body. */
	const_fold(f->body);
	fold_body(f->body);
}
