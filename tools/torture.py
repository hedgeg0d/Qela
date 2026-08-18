#!/usr/bin/env python3
"""Randomized differential tester for stage0 and S2.

Generates syntactically valid Qela programs: scalar expressions, function
calls with up to four parameters including recursion, if/else and bounded
while loops, structs small and large (below and above the 16-byte
by-value limit), fixed-size arrays, enums with payloads and exhaustive
match. Evaluates the expected result in Python, compiles with the given
compiler, runs, and compares exit codes. On mismatch, shrinks and saves a
minimal reproducer to tests/regress/<seed>.qela.

The model mirrors x86 semantics exactly: truncating division, 6-bit shift
counts, signed wrapping. A mismatch means a real compiler bug, so the model
must never be relaxed to make a failure go away.
"""
import argparse, os, random, subprocess, sys, tempfile
from typing import Optional

# ── Qela integer semantics ──────────────────────────────────────────────

WIDTHS = {
    "i8":(8,True),"u8":(8,False),"i16":(16,True),"u16":(16,False),
    "i32":(32,True),"u32":(32,False),"i64":(64,True),"u64":(64,False),
    "int":(64,True),"uint":(64,False),"usize":(64,False),"bool":(1,False),
}

def qwrap(v: int, bits: int, signed: bool) -> int:
    mask = (1 << bits) - 1
    v &= mask
    if signed and (v >> (bits - 1)):
        v -= (1 << bits)
    return v

def wrap(v: int) -> int:
    return qwrap(v, 64, True)

# ── helper functions: Qela text + Python mirrors ────────────────────────
#
# Each entry: name -> (qela source, arity, model callable). Struct helpers
# take a dict (field -> int), enum helpers a ("variant", [payload...]) tuple.

def _fact(n: int) -> int:
    r = 1
    while n > 1:
        r = wrap(r * n)
        n -= 1
    return r

def _gcd(a: int, b: int) -> int:
    while b != 0:
        r = abs(a) % abs(b)
        t = -r if a < 0 else r
        a, b = b, t
    return wrap(a)

def _tag_e(e) -> int:
    if e[0] == "A":   return 7
    if e[0] == "B":   return wrap(e[1][0] + 1)
    return wrap(e[1][0] * 2 + e[1][1])

def _opt_d(d) -> int:
    if d[0] == "None":  return 0
    return wrap(d[1][0])

HELPERS = {
    "add2":   ("fn add2(a int, b int) int { return a + b; }", 2,
               lambda a, b: wrap(a + b)),
    "sub2":   ("fn sub2(a int, b int) int { return a - b; }", 2,
               lambda a, b: wrap(a - b)),
    "mul2":   ("fn mul2(a int, b int) int { return a * b; }", 2,
               lambda a, b: wrap(a * b)),
    "max2":   ("fn max2(a int, b int) int { if (a > b) { return a; } else { return b; } }", 2,
               lambda a, b: a if a > b else b),
    "min2":   ("fn min2(a int, b int) int { if (a < b) { return a; } else { return b; } }", 2,
               lambda a, b: a if a < b else b),
    "sum3":   ("fn sum3(a int, b int, c int) int { return a + b + c; }", 3,
               lambda a, b, c: wrap(a + b + c)),
    "diff4":  ("fn diff4(a int, b int, c int, d int) int { return a - b - c - d; }", 4,
               lambda a, b, c, d: wrap(a - b - c - d)),
    "twice":  ("fn twice(n int) int { return n + n; }", 1,
               lambda n: wrap(n + n)),
    "zero":   ("fn zero() int { return 3; }", 0,
               lambda: 3),
    "mix5":   ("fn mix5(a int, b int, c int, d int, e int) int { return a + b - c + d - e; }", 5,
               lambda a, b, c, d, e: wrap(a + b - c + d - e)),
    "mix6":   ("fn mix6(a int, b int, c int, d int, e int, f int) int { return a - b + c - d + e - f; }", 6,
               lambda a, b, c, d, e, f: wrap(a - b + c - d + e - f)),
    "fact":   ("fn fact(n int) int { if (n <= 1) { return 1; } return n * fact(n - 1); }", 1,
               _fact),
    "gcd2":   ("fn gcd2(a int, b int) int { while (b != 0) { var t int = a % b; a = b; b = t; } return a; }", 2,
               _gcd),
    # Pair is 16 bytes: passed and returned in two argument registers.
    "pair_sum":  ("fn pair_sum(p Pair) int { return p.x + p.y; }", 1,
                  lambda p: wrap(p["x"] + p["y"])),
    "pair_make": ("fn pair_make(a int, b int) Pair { return Pair{x: a, y: b}; }", 2,
                  lambda a, b: {"x": wrap(a), "y": wrap(b)}),
    "pair_swap": ("fn pair_swap(p Pair) Pair { return Pair{x: p.y, y: p.x}; }", 1,
                  lambda p: {"x": p["y"], "y": p["x"]}),
    # Wide is 24 bytes: by reference.
    "wide_sum":  ("fn wide_sum(w Wide) int { return w.a + w.b + w.c; }", 1,
                  lambda w: wrap(w["a"] + w["b"] + w["c"])),
    "wide_make": ("fn wide_make(a int, b int, c int) Wide { return Wide{a: a, b: b, c: c}; }", 3,
                  lambda a, b, c: {"a": wrap(a), "b": wrap(b), "c": wrap(c)}),
    "wide_swap": ("fn wide_swap(w Wide) Wide { return Wide{a: w.b, b: w.a, c: w.c}; }", 1,
                  lambda w: {"a": w["b"], "b": w["a"], "c": w["c"]}),
    "bag_sum":   ("fn bag_sum(b Bag) int { return b.p + b.q + b.r + b.s; }", 1,
                  lambda b: wrap(b["p"] + b["q"] + b["r"] + b["s"])),
    "tag_e":   ("fn tag_e(e E) int {\n\tmatch (e) {\n\t\tA => { return 7; }\n\t\tB(v) => { return v + 1; }\n\t\tC(x, y) => { return x * 2 + y; }\n\t}\n}", 1,
                _tag_e),
    "opt_d":   ("fn opt_d(d D) int {\n\tmatch (d) {\n\t\tNone => { return 0; }\n\t\tSome(v) => { return v; }\n\t}\n}", 1,
                _opt_d),
}

# ── type definitions, always emitted ────────────────────────────────────

STRUCTS = [
    ("Pair", [("x", "int"), ("y", "int")]),               # 16 bytes, by value
    ("Wide", [("a", "int"), ("b", "int"), ("c", "int")]),  # 24 bytes, by ref
    ("Bag",  [("p", "int"), ("q", "int"), ("r", "int"), ("s", "int")]),
]
ENUMS = [
    ("E", [("A", 0), ("B", 1), ("C", 2)]),
    ("D", [("None", 0), ("Some", 1)]),
]

# ── expression evaluation ───────────────────────────────────────────────

def eval_expr(node, env: dict):
    """Scalar values are ints, structs are dicts, enums are
    ("variant", [payload...]) tuples, arrays are lists."""
    if not isinstance(node, tuple):
        return node
    kind = node[0]
    if kind == "int":
        return qwrap(node[2], 64, True)
    if kind == "chr":   return ord(node[1])
    if kind == "true":  return 1
    if kind == "false": return 0
    if kind == "var":   return env[node[1]]
    if kind == "arraylit":
        return [0, 0, 0, 0]
    lhs = eval_expr(node[1], env)
    if kind == "neg":   return wrap(-lhs)
    if kind == "not":   return 0 if lhs != 0 else 1
    if kind == "bnot":  return wrap(~lhs)
    if kind == "as":
        b, s = WIDTHS.get(node[2], (64, True))
        return qwrap(lhs, b, s)
    if kind == "call":
        return HELPERS[node[1]][2](*[eval_expr(a, env) for a in node[2]])
    if kind == "mem":
        return lhs[node[2]]
    if kind == "idx":
        return lhs[wrap(eval_expr(node[2], env)) & 3]
    if kind == "structlit":
        fields = dict(STRUCTS[0][1])  # placeholder, replaced below
        for sname, sfields in STRUCTS:
            if sname == node[1]:
                fields = {f: 0 for f, _ in sfields}
        for f, e in node[2]:
            fields[f] = eval_expr(e, env)
        return fields
    if kind == "enumlit":
        return (node[2], [eval_expr(a, env) for a in node[3]])

    op = kind
    if op in ("||", "&&"):
        if op == "||" and lhs != 0: return 1
        if op == "&&" and lhs == 0: return 0
        rhs = eval_expr(node[2], env)
        return 1 if rhs != 0 else 0

    rhs = eval_expr(node[2], env)
    if op == "+":   return wrap(lhs + rhs)
    if op == "-":   return wrap(lhs - rhs)
    if op == "*":   return wrap(lhs * rhs)
    # x86 idiv truncates toward zero and takes the sign of the dividend.
    if op == "/":
        if rhs == 0: return 0
        q = abs(lhs) // abs(rhs)
        return wrap(-q if (lhs < 0) != (rhs < 0) else q)
    if op == "%":
        if rhs == 0: return 0
        r = abs(lhs) % abs(rhs)
        return wrap(-r if lhs < 0 else r)
    if op == "&":   return wrap(lhs & rhs)
    if op == "|":   return wrap(lhs | rhs)
    if op == "^":   return wrap(lhs ^ rhs)
    if op == "<<":  return wrap(lhs << (rhs & 63))
    if op == ">>":  return wrap(lhs >> (rhs & 63))
    if op == "==":  return 1 if lhs == rhs else 0
    if op == "!=":  return 1 if lhs != rhs else 0
    if op == "<":   return 1 if lhs < rhs else 0
    if op == "<=":  return 1 if lhs <= rhs else 0
    if op == ">":   return 1 if lhs > rhs else 0
    if op == ">=":  return 1 if lhs >= rhs else 0
    return 0


def set_lvalue(target, value, env: dict):
    if target[0] == "var":
        env[target[1]] = value
    elif target[0] == "mem":
        eval_expr(target[1], env)[target[2]] = value
    elif target[0] == "idx":
        eval_expr(target[1], env)[wrap(eval_expr(target[2], env)) & 3] = value


def exec_block(stmts, env: dict):
    """Returns the value of an early return, or None."""
    for st in stmts:
        r = exec_stmt(st, env)
        if r is not None:
            return r
    return None


def exec_stmt(st, env: dict):
    kind = st[0]
    if kind == "decl":
        env[st[1]] = eval_expr(st[3], env)
        return None
    if kind == "asgn":
        set_lvalue(st[1], eval_expr(st[2], env), env)
        return None
    if kind == "if":
        if eval_expr(st[1], env) != 0:
            return exec_block(st[2], env)
        return exec_block(st[3], env)
    if kind == "while":
        while eval_expr(st[1], env) != 0:
            r = exec_block(st[2], env)
            if r is not None:
                return r
        return None
    if kind == "ret":
        return eval_expr(st[1], env)
    if kind == "match":
        scrut = eval_expr(st[1], env)
        for variant, binds, body in st[2]:
            if variant is None or scrut[0] == variant:
                for b, v in zip(binds, scrut[1]):
                    env[b] = v
                return exec_block(body, env)
    return None


# ── code emission ───────────────────────────────────────────────────────

BINOPS = ["+","-","*","&","|","^","<<",">>",
          "==","!=","<","<=",">",">="]
UNARY = ["-", "!"]

def emit(node, vars: set) -> str:
    if isinstance(node, tuple):
        kind = node[0]
        if kind == "int":
            return str(qwrap(node[2], 64, True))
        if kind == "chr":
            return repr(node[1])
        if kind == "true": return "true"
        if kind == "false":return "false"
        if kind == "var":
            vars.add(node[1])
            return node[1]
        if kind in ("neg","not","bnot"):
            op = {"neg":"-","not":"!","bnot":"~"}[kind]
            return f"({op}{emit(node[1], vars)})"
        if kind == "as":
            return f"({emit(node[1], vars)} as {node[2]})"
        if kind == "call":
            return f"{node[1]}({', '.join(emit(a, vars) for a in node[2])})"
        if kind == "mem":
            return f"{emit(node[1], vars)}.{node[2]}"
        if kind == "idx":
            return f"{emit(node[1], vars)}[{emit(node[2], vars)}]"
        if kind == "structlit":
            return (f"{node[1]}{{{', '.join(f + ': ' + emit(e, vars) for f, e in node[2])}}}")
        if kind == "enumlit":
            if node[3]:
                return f"{node[1]}.{node[2]}({', '.join(emit(a, vars) for a in node[3])})"
            return f"{node[1]}.{node[2]}"
        lhs = emit(node[1], vars)
        rhs = emit(node[2], vars)
        return f"({lhs} {kind} {rhs})"
    return str(node)


def render_stmt(st, ind: int) -> str:
    pad = "    " * ind
    kind = st[0]
    if kind == "decl":
        if st[3][0] == "arraylit":
            return f"{pad}var {st[1]} {st[2]};"
        return f"{pad}var {st[1]} {st[2]} = {emit(st[3], set())};"
    if kind == "asgn":
        return f"{pad}{emit(st[1], set())} = {emit(st[2], set())};"
    if kind == "ret":
        return f"{pad}return {emit(st[1], set())};"
    if kind == "if":
        out = [f"{pad}if ({emit(st[1], set())}) {{"]
        out += [render_stmt(s, ind + 1) for s in st[2]]
        out.append(f"{pad}}} else {{")
        out += [render_stmt(s, ind + 1) for s in st[3]]
        out.append(f"{pad}}}")
        return "\n".join(out)
    if kind == "while":
        out = [f"{pad}while ({emit(st[1], set())}) {{"]
        out += [render_stmt(s, ind + 1) for s in st[2]]
        out.append(f"{pad}}}")
        return "\n".join(out)
    if kind == "match":
        out = [f"{pad}match ({emit(st[1], set())}) {{"]
        for variant, binds, body in st[2]:
            if variant is None:
                head = "_ => {"
            elif binds:
                head = f"{variant}({', '.join(binds)}) => {{"
            else:
                head = f"{variant} => {{"
            out.append(f"{pad}    {head}")
            out += [render_stmt(s, ind + 2) for s in body]
            out.append(f"{pad}    }}")
        out.append(f"{pad}}}")
        return "\n".join(out)
    return f"{pad}{emit(st, set())};"


class Program:
    """Helpers, declarations and main-body statements. Shrinking drops
    statements and re-evaluates, so no expectation goes stale."""

    def __init__(self, helpers, decls, stmts, ret_expr):
        self.helpers = helpers
        self.decls = decls      # ("decl", name, ty, value-node)
        self.stmts = stmts
        self.ret_expr = ret_expr

    def run(self) -> int:
        env = {}
        for d in self.decls:
            exec_stmt(d, env)
        r = exec_block(self.stmts, env)
        if r is None:
            r = eval_expr(self.ret_expr, env)
        return wrap(r) & 0xff

    def render(self) -> str:
        out = []
        for sname, fields in STRUCTS:
            out.append(f"struct {sname} {{ {', '.join(f + ' ' + t for f, t in fields)}, }}")
        for ename, variants in ENUMS:
            parts = []
            for vname, arity in variants:
                parts.append(vname if arity == 0 else
                             f"{vname}({', '.join(['int'] * arity)})")
            out.append(f"enum {ename} {{ {', '.join(parts)}, }}")
        for h in self.helpers:
            out.append(HELPERS[h][0])
        out.append("fn main() int {")
        out += [render_stmt(d, 1) for d in self.decls]
        out += [render_stmt(s, 1) for s in self.stmts]
        out.append(f"    return {emit(self.ret_expr, set())};")
        out.append("}")
        return "\n".join(out) + "\n"

    def without(self, i):
        return Program(self.helpers, self.decls,
                       self.stmts[:i] + self.stmts[i + 1:], self.ret_expr)


# ── generator ───────────────────────────────────────────────────────────

class Gen:
    def __init__(self, rng: random.Random, max_depth: int = 4):
        self.rng = rng
        self.md = max_depth
        self.help = []          # scalar helper names usable in expressions
        self.agg = []           # struct/enum helper names usable in calls

    def leaf(self, vars_used: set, depth: int = 0) -> tuple:
        k = self.rng.randint(0, 10)
        if k == 0:
            return ("int", "i64", self.rng.randint(-1000, 1000))
        if k == 1:
            return ("int", "i64", self.rng.choice([0,1,2,3,10,42,100,127,255,-1]))
        if k == 2:
            return ("true",) if self.rng.random() < 0.5 else ("false",)
        if k == 3:
            return ("chr", self.rng.choice("abcdefg0123\n\t"))
        if k == 4:
            return ("var", self.rng.choice(["v0", "v1", "v2"]))
        if k == 5:
            return ("mem", ("var", "p"), self.rng.choice(["x", "y"]))
        if k == 6:
            return ("idx", ("var", "a"),
                    ("&", self.expr(depth + 1, vars_used), ("int", "i64", 3)))
        if k == 7 and self.help:
            name = self.rng.choice(self.help)
            args = []
            for _ in range(HELPERS[name][1]):
                arg = self.expr(depth + 1, vars_used)
                if name in ("fact", "gcd2"):
                    arg = ("&", arg, ("int", "i64", 15))
                args.append(arg)
            return ("call", name, args)
        if k == 8:
            return ("var", self.rng.choice(["v0", "v1", "v2"]))
        return self.agg_call(depth + 1)

    def agg_call(self, depth: int = 1) -> tuple:
        """A call whose argument is a struct, a struct literal or an enum."""
        cand = [h for h in ("tag_e", "opt_d", "pair_sum", "wide_sum", "bag_sum")
                if h in self.agg]
        if not cand:
            return ("int", "i64", 0)
        kind = self.rng.choice(cand)
        if kind == "tag_e":
            return ("call", "tag_e", [("var", "e")])
        if kind == "opt_d":
            if self.rng.random() < 0.5:
                return ("call", "opt_d", [("enumlit", "D", "None", [])])
            return ("call", "opt_d", [("enumlit", "D", "Some",
                                       [self.expr(depth, set())])])
        if kind == "pair_sum":
            r = self.rng.random()
            if "pair_make" in self.agg and r < 0.4:
                return ("call", "pair_sum",
                        [("call", "pair_make",
                          [self.expr(depth, set()), self.expr(depth, set())])])
            if r < 0.7:
                return ("call", "pair_sum", [("structlit", "Pair", [
                    ("x", self.expr(depth, set())), ("y", self.expr(depth, set()))])])
            return ("call", "pair_sum", [("var", "p")])
        if kind == "wide_sum":
            if "wide_make" in self.agg and self.rng.random() < 0.5:
                return ("call", "wide_sum",
                        [("call", "wide_make",
                          [self.expr(depth, set()), self.expr(depth, set()),
                           self.expr(depth, set())])])
            return ("call", "wide_sum", [("var", "w")])
        return ("call", "bag_sum", [("var", "b")])

    def expr(self, depth: int, vars_used: set) -> tuple:
        if depth >= self.md:
            return self.leaf(vars_used, depth)
        k = self.rng.randint(0, 17)
        if k < 6:  return self.leaf(vars_used, depth)
        if k < 13: return self.binop(depth, vars_used)
        if k < 15: return self.unary(depth, vars_used)
        if k < 17: return self.cast(depth, vars_used)
        return self.leaf(vars_used, depth)

    def binop(self, depth: int, vused: set) -> tuple:
        op = self.rng.choice(BINOPS)
        return (op, self.expr(depth + 1, vused), self.expr(depth + 1, vused))

    def unary(self, depth: int, vused: set) -> tuple:
        op = self.rng.choice(UNARY)
        return ({"-":"neg","!":"not"}[op], self.expr(depth + 1, vused))

    CAST_TYPES = ["i8", "u8", "i16", "u16", "i32", "u32"]

    def cast(self, depth: int, vused: set) -> tuple:
        return ("as", self.expr(depth + 1, vused), self.rng.choice(self.CAST_TYPES))

    def cond(self, depth: int) -> tuple:
        op = self.rng.choice(["<", "<=", ">", ">=", "!=", "=="])
        return (op, self.expr(depth, set()), self.expr(depth, set()))

    def struct_value(self, ty: str) -> tuple:
        if ty == "Pair":
            r = self.rng.random()
            if "pair_make" in self.agg and r < 0.4:
                return ("call", "pair_make",
                        [self.expr(0, set()), self.expr(0, set())])
            if "pair_swap" in self.agg and r < 0.7:
                return ("call", "pair_swap", [("var", "p")])
            return ("structlit", "Pair", [
                ("x", self.expr(0, set())), ("y", self.expr(0, set()))])
        if ty == "Wide":
            r = self.rng.random()
            if "wide_make" in self.agg and r < 0.5:
                return ("call", "wide_make",
                        [self.expr(0, set()), self.expr(0, set()),
                         self.expr(0, set())])
            if "wide_swap" in self.agg:
                return ("call", "wide_swap", [("var", "w")])
            return ("structlit", "Wide", [
                ("a", self.expr(0, set())), ("b", self.expr(0, set())),
                ("c", self.expr(0, set()))])
        return ("structlit", "Bag", [
            ("p", self.expr(0, set())), ("q", self.expr(0, set())),
            ("r", self.expr(0, set())), ("s", self.expr(0, set()))])

    def arm_expr(self, binds) -> tuple:
        e = self.expr(0, set())
        if binds and self.rng.random() < 0.6:
            b = self.rng.choice(binds)
            op = self.rng.choice(["+", "-", "*", "&", "|", "^"])
            return (op, e, ("var", b))
        return e

    def stmt(self, allow_flow: bool, depth: int) -> list:
        k = self.rng.randint(0, 9)
        if k < 3:
            tgt = ("var", self.rng.choice(["v0", "v1", "v2"]))
            return [("asgn", tgt, self.expr(0, set()))]
        if k == 3:
            return [("asgn", ("mem", ("var", "p"), self.rng.choice(["x", "y"])),
                     self.expr(0, set()))]
        if k == 4:
            tgt = self.rng.choice(["p", "w", "b"])
            ty = {"p": "Pair", "w": "Wide", "b": "Bag"}[tgt]
            return [("asgn", ("var", tgt), self.struct_value(ty))]
        if k == 5:
            idx = ("&", self.expr(0, set()), ("int", "i64", 3))
            return [("asgn", ("idx", ("var", "a"), idx), self.expr(0, set()))]
        if k == 6:
            variant, arity = self.rng.choice([("B", 1), ("C", 2)])
            args = [self.expr(0, set()) for _ in range(arity)]
            return [("asgn", ("var", "e"), ("enumlit", "E", variant, args))]
        if k == 7 and allow_flow:
            body_t = self.stmts(True, depth + 1, self.rng.randint(1, 2))
            body_f = self.stmts(True, depth + 1, self.rng.randint(1, 2))
            if self.rng.random() < 0.3:
                body_t.append(("ret", self.expr(0, set())))
            if self.rng.random() < 0.3:
                body_f.append(("ret", self.expr(0, set())))
            return [("if", self.cond(0), body_t, body_f)]
        if k == 8 and allow_flow and depth == 0:
            if self.rng.random() < 0.5:
                # The elidable pattern: the loop bound equals the array
                # length, and the index is the induction variable itself.
                body = self.stmts(True, depth + 1, self.rng.randint(1, 2))
                body.append(("asgn", ("idx", ("var", "a"), ("var", "i")),
                             self.expr(0, set())))
                body.append(("asgn", ("var", "i"),
                             ("+", ("var", "i"), ("int", "i64", 1))))
                return [("asgn", ("var", "i"), ("int", "i64", 0)),
                        ("while", ("<", ("var", "i"), ("int", "i64", 4)), body)]
            bound = self.rng.randint(1, 3)
            body = self.stmts(True, depth + 1, self.rng.randint(1, 3))
            body.append(("asgn", ("var", "i"),
                         ("+", ("var", "i"), ("int", "i64", 1))))
            return [("asgn", ("var", "i"), ("int", "i64", 0)),
                    ("while", ("<", ("var", "i"), ("int", "i64", bound)), body)]
        arms = []
        arms.append(("A", [], [("ret", self.arm_expr([]))]))
        arms.append(("B", ["mb0"], [("ret", self.arm_expr(["mb0"]))]))
        arms.append(("C", ["mb0", "mb1"], [("ret", self.arm_expr(["mb1"]))]))
        return [("match", ("var", "e"), arms)]

    def stmts(self, allow_flow: bool, depth: int, n: int) -> list:
        out = []
        for _ in range(n):
            out += self.stmt(allow_flow, depth)
        return out


def generate(seed: int, stmts: int = 8, max_depth: int = 4) -> Program:
    rng = random.Random(seed)
    gen = Gen(rng, max_depth)

    all_scalar = ["add2","sub2","mul2","max2","min2","sum3","diff4","twice",
                  "zero","mix5","mix6","fact","gcd2"]
    gen.help = rng.sample(all_scalar, rng.randint(2, 4))
    struct_help = rng.sample(["pair_sum", "pair_swap"], rng.randint(1, 2))
    big_help = rng.sample(["wide_sum", "wide_make", "wide_swap", "bag_sum"],
                          rng.randint(1, 2))
    enum_help = rng.sample(["tag_e", "opt_d"], rng.randint(1, 2))
    gen.agg = struct_help + big_help + enum_help
    helpers = gen.help + gen.agg

    decls = [
        ("decl", "v0", "int", ("int", "i64", rng.randint(0, 10))),
        ("decl", "v1", "int", ("int", "i64", rng.randint(0, 10))),
        ("decl", "v2", "int", ("int", "i64", rng.randint(0, 10))),
        ("decl", "p", "Pair", ("structlit", "Pair", [])),
        ("decl", "w", "Wide", ("structlit", "Wide", [])),
        ("decl", "b", "Bag", ("structlit", "Bag", [])),
        ("decl", "e", "E", ("enumlit", "E", "A", [])),
        ("decl", "a", "[4]int", ("arraylit",)),
        ("decl", "i", "int", ("int", "i64", 0)),
    ]

    body = gen.stmts(True, 0, stmts + rng.randint(0, 3))
    return Program(helpers, decls, body, gen.expr(0, set()))


# ── compile & run ───────────────────────────────────────────────────────

def compile_and_run(src: str, compiler: str, timeout: float = 10.0) -> Optional[int]:
    try:
        with tempfile.NamedTemporaryFile(mode="w", suffix=".qela", delete=False) as sf:
            sf.write(src); sf.flush()
            sp = sf.name
        bp = sp + ".bin"
        subprocess.run([compiler, sp, "-o", bp],
                       capture_output=True, timeout=timeout)
        if not os.path.isfile(bp) or os.path.getsize(bp) == 0:
            return None
        os.chmod(bp, 0o755)
        r = subprocess.run([bp], capture_output=True, timeout=timeout)
        return r.returncode
    except subprocess.TimeoutExpired:
        return None
    except Exception:
        return None
    finally:
        for f in [sp, bp]:
            if f and os.path.exists(f):
                try:
                    os.unlink(f)
                except OSError:
                    pass


def save_regression(seed: int, src: str, expected: int, got, regdir: str):
    os.makedirs(regdir, exist_ok=True)
    p = os.path.join(regdir, f"{seed}.qela")
    with open(p, "w") as f:
        f.write(f"// expect-exit: {expected}\n// got: {got}\n{src}")
    return p


# ── shrinker ────────────────────────────────────────────────────────────

def shrink(prog: Program, compiler: str, timeout: float) -> Program:
    """Drop statements while the mismatch survives. Each candidate is
    re-evaluated: removing a statement changes what the program should
    return, so the original expectation does not carry over."""
    best = prog
    changed = True
    while changed:
        changed = False
        for i in range(len(best.stmts)):
            cand = best.without(i)
            if not cand.stmts: continue
            src = cand.render()
            expected = cand.run()
            got = compile_and_run(src, compiler, timeout)
            if got is not None and got != expected:
                best = cand
                changed = True
                break
    return best


# ── main ────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("-n", type=int, default=100)
    p.add_argument("--max-depth", type=int, default=4)
    p.add_argument("--stmts", type=int, default=8, help="statements per program")
    p.add_argument("--compiler", default="./qela")
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--regress-dir", default="tests/regress")
    p.add_argument("--shrink", action="store_true")
    args = p.parse_args()

    comp = args.compiler
    ok = fail = crash = 0

    for i in range(args.n):
        seed = (args.seed + i) if args.seed is not None else random.randint(0, 2**31-1)
        prog = generate(seed, args.stmts, args.max_depth)
        src = prog.render()
        expected = prog.run()
        got = compile_and_run(src, comp, args.timeout)

        if got is None:
            crash += 1
            save_regression(seed, src, expected, "CRASH", args.regress_dir)
        elif got == expected:
            ok += 1
        else:
            print(f"FAIL seed={seed}: expected {expected}, got {got}")
            if args.shrink:
                prog = shrink(prog, comp, args.timeout)
                src = prog.render()
                expected = prog.run()
                got = compile_and_run(src, comp, args.timeout)
            save_regression(seed, src, expected, got, args.regress_dir)
            fail += 1

        if (i+1) % 100 == 0:
            print(f"  [{i+1}] pass={ok} fail={fail} crash={crash}")

    print(f"\n{ok} passed, {fail} failed, {crash} crashes")
    return 1 if (fail or crash) else 0

if __name__ == "__main__":
    sys.exit(main())
