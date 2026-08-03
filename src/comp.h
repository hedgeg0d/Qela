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
	TY_VOID,
	TY_BOOL,
	TY_INT,
	TY_PTR,
	TY_ARRAY,
	TY_STRUCT,
	TY_SLICE,
} TypeKind;

typedef struct Type   Type;
typedef struct Member Member;

struct Member {
	Member *next;
	Str     name;
	Type   *ty;
	int     offset;
	isize   pos;
};

struct Type {
	TypeKind kind;
	int      size;
	int      align;
	bool     is_unsigned;
	Type    *base;
	isize    len;
	Str      name;
	Member  *members;
};

extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_i8, *ty_i16, *ty_i32, *ty_i64;
extern Type *ty_u8, *ty_u16, *ty_u32, *ty_u64;

Type *type_ptr(Type *base);
Type *type_array(Type *base, isize len);
Type *type_slice(Type *base);
Type *type_str(void);
Type *type_lookup(Str name);
void  type_define(Str name, Type *ty);
bool  is_integer(Type *t);
bool  is_numeric(Type *t);
bool  is_aggregate(Type *t);
Type *decayed(Type *t);
Str   type_name(Type *t);

typedef enum {
	ND_NUM,
	ND_STRLIT,
	ND_VAR,
	ND_CALL,
	ND_SYSCALL,
	ND_ADDR,
	ND_DEREF,
	ND_MEMBER,
	ND_INDEX,
	ND_SLICE,
	ND_CAST,
	ND_ADD,
	ND_SUB,
	ND_MUL,
	ND_DIV,
	ND_MOD,
	ND_NEG,
	ND_NOT,
	ND_BITAND,
	ND_BITOR,
	ND_BITXOR,
	ND_BITNOT,
	ND_SHL,
	ND_SHR,
	ND_EQ,
	ND_NE,
	ND_LT,
	ND_LE,
	ND_AND,
	ND_OR,
	ND_ASSIGN,
	ND_OPASSIGN,
	ND_COMMA,
	ND_IF,
	ND_FOR,
	ND_BREAK,
	ND_CONT,
	ND_RET,
	ND_BLOCK,
	ND_EXPRSTMT,
} NodeKind;

typedef struct Var Var;
struct Var {
	Str   name;
	Type *ty;
	Var  *next;
	int   offset;
	bool  is_global;
	isize data_off;
	i64   init;
};

typedef struct Node Node;
struct Node {
	NodeKind kind;
	Type    *ty;
	bool     typed;
	Node    *next;
	Node    *lhs;
	Node    *rhs;
	Node    *cond;
	Node    *then;
	Node    *els;
	Node    *init;
	Node    *step;
	Node    *body;
	Node    *args;
	int      nargs;
	i64      val;
	Var     *var;
	Var     *tmp;
	Member  *member;
	Str      name;
	isize    pos;
};

void add_type(Node *n);

typedef struct Func Func;
struct Func {
	Str   name;
	Func *next;
	Var  *params[6];
	int   nparams;
	Var  *locals;
	Type *ret;
	int   stack_size;
	Node *body;
	i64   addr;
	isize pos;
};

void type_func(Func *f);
void type_set_fn(Func *f);

typedef struct {
	Func *funcs;
	Var  *globals;
	Str  *strs;
	int   nstrs;
} Unit;

Unit parse(Token *tok);
Func *find_func(Str name);

#define ELF_BASE 0x400000
#define EHDR_SZ  64
#define PHDR_SZ  56
#define SEG_GAP  0x1000

typedef struct {
	Buf   code;
	Buf   rodata;
	Buf   data;
	isize bss_size;
	i64   entry;
	int   nph;
} Image;

extern bool opt_no_bounds;

Image codegen(Unit *u);

void write_elf(const char *path, Image *img);

#endif
