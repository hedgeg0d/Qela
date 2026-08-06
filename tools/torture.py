#!/usr/bin/env python3
"""Randomized differential tester for stage0.

Generates syntactically valid Qela programs with scalar types, expressions,
and simple control flow. Evaluates expected result in Python, compiles with
stage0, runs, and compares exit codes. On mismatch, shrinks and saves a
minimal reproducer to tests/regress/<seed>.qela.
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

def qeval(node, vars: dict) -> int:
    """Recursively evaluate a Python-tuple AST."""
    if isinstance(node, tuple):
        kind = node[0]
        if kind == "int":
            t = node[1]
            b, s = WIDTHS.get(t, (64, True))
            return qwrap(node[2], b, s)
        if kind == "chr":  return ord(node[1])
        if kind == "true": return 1
        if kind == "false":return 0
        if kind == "var":
            return vars.get(node[1], 0)

        lhs = qeval(node[1], vars)
        # unary — i64 width matches Qela's widening
        if kind == "neg":   return qwrap(-lhs, 64, True)
        if kind == "not":   return 0 if lhs != 0 else 1
        if kind == "bnot":  return qwrap(~lhs, 64, True)

        # cast
        if kind == "as":
            b, s = WIDTHS.get(node[2], (64, True))
            return qwrap(lhs, b, s)

        # binary — evaluate rhs, with short-circuit
        op = kind
        if op in ("||", "&&"):
            if op == "||" and lhs != 0: return 1
            if op == "&&" and lhs == 0: return 0
            rhs = qeval(node[2], vars)
            return 1 if rhs != 0 else 0

        rhs = qeval(node[2], vars)
        # All binary ops at i64 width — matches Qela's usual_conv widening.
        b, s = 64, True
        def w(v): return qwrap(v, b, s)
        wu = lambda v: qwrap(v, b, False)

        if op == "+":   return w(lhs + rhs)
        if op == "-":   return w(lhs - rhs)
        if op == "*":   return w(lhs * rhs)
        if op == "/":
            if rhs == 0: return 0
            return w(int(lhs / rhs))
        if op == "%":
            if rhs == 0: return 0
            return w(lhs % rhs)
        if op == "&":   return wu(lhs & rhs)
        if op == "|":   return wu(lhs | rhs)
        if op == "^":   return w(lhs ^ rhs)
        if op == "<<":
            sr = rhs & 63
            return w(lhs << sr if rhs >= 0 else lhs >> (-rhs & 63))
        if op == ">>":
            sr = rhs & 63
            if rhs < 0: return w(lhs << (-rhs & 63))
            return w(lhs >> sr) if s else wu(lhs >> sr)
        if op == "==":  return 1 if lhs == rhs else 0
        if op == "!=":  return 1 if lhs != rhs else 0
        if op == "<":   return 1 if lhs < rhs else 0
        if op == "<=":  return 1 if lhs <= rhs else 0
        if op == ">":   return 1 if lhs > rhs else 0
        if op == ">=":  return 1 if lhs >= rhs else 0
    return 0
    return 0


# ── Code emission ───────────────────────────────────────────────────────

BINOPS = ["+","-","*","&","|","^","<<",">>",
          "==","!=","<","<=",">",">="]
UNARY = ["-", "!"]
TYPES = ["i64","u64","i32","u32","i16","u16","i8","u8","int","uint","usize","bool"]

def emit(node, vars: set) -> str:
    """Emit Qela source from a tuple AST."""
    if isinstance(node, tuple):
        kind = node[0]
        if kind == "int":
            t = node[1]
            return str(node[2])
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

        lhs = emit(node[1], vars)
        rhs = emit(node[2], vars)
        op = node[0]
        return f"({lhs} {op} {rhs})"
    return str(node)


# ── Generator ───────────────────────────────────────────────────────────

class Gen:
    def __init__(self, rng: random.Random, max_depth: int = 5,
                 var_pool: list[str] = None):
        self.rng = rng; self.md = max_depth
        self.vars = var_pool or []

    def ty(self) -> str:
        return self.rng.choice(TYPES)

    def leaf(self, vars_used: set) -> tuple:
        k = self.rng.randint(0, 6)
        if k == 0:
            v = self.rng.randint(-1000, 1000)
            return ("int", self.ty(), v)
        if k == 1:
            v = self.rng.choice([0,1,2,10,42,100,127,255,-1])
            return ("int", self.ty(), v)
        if k == 2:
            return ("true",) if self.rng.random() < 0.5 else ("false",)
        if k == 3:
            return ("chr", self.rng.choice("abcdefg0123\n\t"))
        if k >= 4 and self.vars:
            name = self.rng.choice(self.vars)
            vars_used.add(name)
            return ("var", name)
        v = self.rng.randint(0, 50)
        return ("int", self.ty(), v)

    def expr(self, depth: int, vars_used: set) -> tuple:
        if depth >= self.md:
            return self.leaf(vars_used)
        k = self.rng.randint(0, 15)
        if k < 5:  return self.leaf(vars_used)
        if k < 13: return self.binop(depth, vars_used)
        return self.unary(depth, vars_used)

    def binop(self, depth: int, vused: set) -> tuple:
        op = self.rng.choice(BINOPS)
        ty = self.ty()
        lhs = self.expr(depth + 1, vused)
        rhs = self.expr(depth + 1, vused)
        return (op, lhs, rhs, ty)

    def unary(self, depth: int, vused: set) -> tuple:
        op = self.rng.choice(UNARY)
        ty = self.ty()
        return ({"-":"neg","!":"not","~":"bnot"}[op],
                self.expr(depth + 1, vused), ty)

    def cast(self, depth: int, vused: set) -> tuple:
        return ("as", self.expr(depth + 1, vused), self.ty())


def generate(seed: int, stmts: int = 6, max_depth: int = 4):
    rng = random.Random(seed)
    gen = Gen(rng, max_depth)

    # Pre-declare variables
    nvars = rng.randint(2, 4)
    decl_lines = []
    vars_available = []
    for i in range(nvars):
        name = f"v{i}"
        init = rng.randint(0, 20)
        decl_lines.append(f"    var {name} int = {init};")
        vars_available.append(name)

    gen.vars = list(vars_available)

    # Generate statement sequence: each computes a value and stores in a var
    body = list(decl_lines)
    eval_vars = {n: rng.randint(0, 20) for n in vars_available}
    rv = vars_available[-1]

    for i in range(stmts):
        tgt = rng.choice(vars_available)
        vu = set()
        node = gen.expr(0, vu)
        val = qeval(node, eval_vars)
        b, s = WIDTHS.get("int", (64, True))
        val = qwrap(val, b, s)
        eval_vars[tgt] = val

        src = emit(node, set())
        body.append(f"    {tgt} = {src};")
        rv = tgt

    return f'fn main() int {{\n{"\n".join(body)}\n    return {rv};\n}}\n', \
           eval_vars.get(rv, 0) & 0xff


# ── Compile & run ───────────────────────────────────────────────────────

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


# ── Shrinker ────────────────────────────────────────────────────────────

def shrink(src: str, expected: int, compiler: str, timeout: float) -> str:
    lines = src.split("\n")
    # Find body lines (between { and return)
    body_start = next(i+1 for i,l in enumerate(lines) if l.strip().endswith("{"))
    body_end = next(i for i in range(len(lines)-1,-1,-1) if "return " in lines[i])

    stmts = lines[body_start:body_end]
    ret = lines[body_end]

    best = list(stmts)
    changed = True
    while changed:
        changed = False
        for i in range(len(best)):
            c = best[:i] + best[i+1:]
            if not c: continue
            s = "\n".join(lines[:body_start] + c + [ret, "}\n"])
            if compile_and_run(s, compiler, timeout) == expected:
                best = c; changed = True; break

    return "\n".join(lines[:body_start] + best + [ret, "}\n"])


# ── Main ────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("-n", type=int, default=100)
    p.add_argument("--max-depth", type=int, default=4)
    p.add_argument("--stmts", type=int, default=6, help="statements per program")
    p.add_argument("--compiler", default="./qela")
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--regress-dir", default="tests/regress")
    p.add_argument("--shrink", action="store_true")
    args = p.parse_args()

    comp = args.compiler
    ok = fail = crash = 0

    for i in range(args.n):
        seed = (args.seed + i) if args.seed is not None else random.randint(0, 2**31-1)
        src, expected = generate(seed, args.stmts, args.max_depth)
        got = compile_and_run(src, comp, args.timeout)

        if got is None:
            crash += 1
            save_regression(seed, src, expected, "CRASH", args.regress_dir)
        elif got == expected:
            ok += 1
        else:
            print(f"FAIL seed={seed}: expected {expected}, got {got}")
            if args.shrink:
                src = shrink(src, expected, comp, args.timeout)
            save_regression(seed, src, expected, got, args.regress_dir)
            fail += 1

        if (i+1) % 100 == 0:
            print(f"  [{i+1}] pass={ok} fail={fail} crash={crash}")

    print(f"\n{ok} passed, {fail} failed, {crash} crashes")
    return 1 if (fail or crash) else 0

if __name__ == "__main__":
    sys.exit(main())
