#include "comp.h"

static const char *keywords[] = {"fn",  "let",   "var",      "if",       "else",
                                 "while", "for", "in",       "break",    "continue",
                                 "return", "as", "struct", "enum",
                                 "match",  "defer", "import", NULL};

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_ident0(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident(char c) { return is_ident0(c) || is_digit(c); }

static bool is_keyword(Str s) {
	for (int i = 0; keywords[i]; i++)
		if (str_eq(s, str_from_cstr(keywords[i]))) return true;
	return false;
}

static const char *puncts[] = {
    "<<=", ">>=", "=>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=",
    "/=",  "%=",  "&=", "|=", "^=", "<<", ">>", "..", NULL};

static isize lex_base;

static Token *push(Token **tail, TokKind kind, isize pos) {
	Token *t = anew(Token);
	*t = (Token){.kind = kind, .pos = pos + lex_base};
	(*tail)->next = t;
	*tail = t;
	return t;
}

static i64 read_escape(Str src, isize *i) {
	char c = src.p[(*i)++];
	switch (c) {
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case '0': return 0;
	case '\\': return '\\';
	case '"': return '"';
	case '\'': return '\'';
	default: error_at(lex_base + *i - 1, "unknown escape sequence");
	}
}

Token *lex(Str src, isize base) {
	lex_base = base;
	Token head = {0};
	Token *tail = &head;

	isize i = 0;
	while (i < src.n) {
		if (is_space(src.p[i])) {
			i++;
			continue;
		}
		if (src.p[i] == '/' && i + 1 < src.n && src.p[i + 1] == '/') {
			while (i < src.n && src.p[i] != '\n') i++;
			continue;
		}
		if (src.p[i] == '/' && i + 1 < src.n && src.p[i + 1] == '*') {
			isize start = i;
			i += 2;
			while (i + 1 < src.n && !(src.p[i] == '*' && src.p[i + 1] == '/')) i++;
			if (i + 1 >= src.n) error_at(lex_base + start, "unterminated block comment");
			i += 2;
			continue;
		}

		if (is_digit(src.p[i])) {
			isize start = i;
			i64 val = 0;
			if (src.p[i] == '0' && i + 1 < src.n &&
			    (src.p[i + 1] == 'x' || src.p[i + 1] == 'X')) {
				i += 2;
				if (i >= src.n) error_at(lex_base + start, "malformed hex literal");
				while (i < src.n) {
					char c = src.p[i];
					int d;
					if (is_digit(c)) d = c - '0';
					else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
					else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
					else break;
					val = val * 16 + d;
					i++;
				}
			} else {
				while (i < src.n && is_digit(src.p[i])) val = val * 10 + (src.p[i++] - '0');
			}
			if (i < src.n && is_ident(src.p[i])) error_at(lex_base + i, "invalid digit in number");
			Token *t = push(&tail, TK_NUM, start);
			t->val = val;
			t->text = (Str){src.p + start, i - start};
			continue;
		}

		if (is_ident0(src.p[i])) {
			isize start = i;
			while (i < src.n && is_ident(src.p[i])) i++;
			Str text = (Str){src.p + start, i - start};
			Token *t = push(&tail, is_keyword(text) ? TK_KW : TK_IDENT, start);
			t->text = text;
			continue;
		}

		if (src.p[i] == '"') {
			isize start = i++;
			char *buf = anew_n(char, src.n - i + 1);
			isize n = 0;
			while (i < src.n && src.p[i] != '"') {
				if (src.p[i] == '\n') error_at(lex_base + start, "unterminated string literal");
				if (src.p[i] == '\\') {
					i++;
					buf[n++] = (char)read_escape(src, &i);
				} else {
					buf[n++] = src.p[i++];
				}
			}
			if (i >= src.n) error_at(lex_base + start, "unterminated string literal");
			i++;
			buf[n] = 0;
			Token *t = push(&tail, TK_STR, start);
			t->str = (Str){buf, n};
			continue;
		}

		isize plen = 0;
		for (int k = 0; puncts[k]; k++) {
			isize l = str_from_cstr(puncts[k]).n;
			if (i + l <= src.n && memcmp(src.p + i, puncts[k], (usize)l) == 0) {
				plen = l;
				break;
			}
		}
		if (!plen) {
			static const char *singles = "+-*/%()[]{};,=<>!&|^~.:";
			bool ok = false;
			for (const char *s = singles; *s; s++)
				if (*s == src.p[i]) ok = true;
			if (!ok) error_at(lex_base + i, "unexpected character");
			plen = 1;
		}
		Token *t = push(&tail, TK_PUNCT, i);
		t->text = (Str){src.p + i, plen};
		i += plen;
	}

	push(&tail, TK_EOF, src.n);
	return head.next;
}
