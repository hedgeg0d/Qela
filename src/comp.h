#ifndef QELA_COMP_H
#define QELA_COMP_H

#include "qela.h"

typedef enum {
	TK_EOF,
	TK_IDENT,
	TK_KW,
	TK_NUM,
	TK_STR,
	TK_PUNCT,
} TokKind;

typedef struct Token Token;
struct Token {
	TokKind kind;
	Token  *next;
	Str     text;
	Str     str;
	i64     val;
	isize   pos;
};

void diag_init(const char *path, Str src);
_Noreturn void error_at(isize pos, const char *fmt, ...);

Token *lex(Str src);

typedef enum {
	ND_NUM,
	ND_STRLIT,
	ND_VAR,
	ND_CALL,
	ND_SYSCALL,
	ND_CSTRLEN,
	ND_ADD,
	ND_SUB,
	ND_MUL,
	ND_DIV,
	ND_MOD,
	ND_NEG,
	ND_NOT,
	ND_EQ,
	ND_NE,
	ND_LT,
	ND_LE,
	ND_AND,
	ND_OR,
	ND_ASSIGN,
	ND_IF,
	ND_WHILE,
	ND_RET,
	ND_BLOCK,
	ND_EXPRSTMT,
} NodeKind;

typedef struct Var Var;
struct Var {
	Str  name;
	Var *next;
	int  offset;
};

typedef struct Node Node;
struct Node {
	NodeKind kind;
	Node    *next;
	Node    *lhs;
	Node    *rhs;
	Node    *cond;
	Node    *then;
	Node    *els;
	Node    *body;
	Node    *args;
	int      nargs;
	i64      val;
	Var     *var;
	Str      name;
	isize    pos;
};

typedef struct Func Func;
struct Func {
	Str   name;
	Func *next;
	Var  *params;
	int   nparams;
	Var  *locals;
	int   stack_size;
	Node *body;
	i64   addr;
};

typedef struct {
	Func *funcs;
	Str  *strs;
	int   nstrs;
} Unit;

Unit parse(Token *tok);

typedef struct {
	Buf code;
	Buf rodata;
	i64 entry;
} Image;

Image codegen(Unit *u);

void write_elf(const char *path, Image *img);

#endif
