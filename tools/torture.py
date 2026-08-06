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
        # Qela has no unsigned literal syntax: every integer literal is i64.
        if kind == "int":
            return qwrap(node[2], 64, True)
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

        if op == "+":   return w(lhs + rhs)
        if op == "-":   return w(lhs - rhs)
        if op == "*":   return w(lhs * rhs)
        # x86 idiv truncates toward zero and takes the sign of the dividend,
        # unlike Python's floor division and modulo.
        if op == "/":
            if rhs == 0: return 0
            q = abs(lhs) // abs(rhs)
            return w(-q if (lhs < 0) != (rhs < 0) else q)
        if op == "%":
            if rhs == 0: return 0
            r = abs(lhs) % abs(rhs)
            return w(-r if lhs < 0 else r)
        if op == "&":   return w(lhs & rhs)
        if op == "|":   return w(lhs | rhs)
        if op == "^":   return w(lhs ^ rhs)
        # shl/sar take the count from CL, masked to 6 bits for 64-bit
        # operands. The sign of the count never flips the direction.
        if op == "<<":  return w(lhs << (rhs & 63))
        if op == ">>":  return w(lhs >> (rhs & 63))
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
        if k < 12: return self.binop(depth, vars_used)
        if k < 14: return self.unary(depth, vars_used)
        return self.cast(depth, vars_used)

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

    # Only widths that survive exactly in a signed i64, so every operation
    # around the cast keeps signed 64-bit semantics in both the program and
    # the model. u64 would need unsigned comparison and shift modelling.
    CAST_TYPES = ["i8", "u8", "i16", "u16", "i32", "u32"]

    def cast(self, depth: int, vused: set) -> tuple:
        return ("as", self.expr(depth + 1, vused), self.rng.choice(self.CAST_TYPES))


class Program:
    """Declarations plus assignments, kept structured so that shrinking can
    recompute the expected value instead of reusing a stale one."""

    def __init__(self, inits, stmts):
        self.inits = inits   # [(name, initial value)]
        self.stmts = stmts   # [(target, expression node)]

    def render(self):
        vars_ = dict(self.inits)
        body = [f"    var {n} int = {v};" for n, v in self.inits]
        rv = self.inits[-1][0]
        for tgt, node in self.stmts:
            vars_[tgt] = qwrap(qeval(node, vars_), 64, True)
            body.append(f"    {tgt} = {emit(node, set())};")
            rv = tgt
        src = "fn main() int {\n" + "\n".join(body) + f"\n    return {rv};\n}}\n"
        return src, vars_.get(rv, 0) & 0xff

    def without(self, i):
        return Program(self.inits, self.stmts[:i] + self.stmts[i + 1:])


def generate(seed: int, stmts: int = 6, max_depth: int = 4) -> Program:
    rng = random.Random(seed)
    gen = Gen(rng, max_depth)

    nvars = rng.randint(2, 4)
    inits = [(f"v{i}", rng.randint(0, 20)) for i in range(nvars)]
    gen.vars = [n for n, _ in inits]

    body = []
    for _ in range(stmts):
        tgt = rng.choice(gen.vars)
        body.append((tgt, gen.expr(0, set())))

    return Program(inits, body)


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
            src, expected = cand.render()
            got = compile_and_run(src, compiler, timeout)
            if got is not None and got != expected:
                best = cand
                changed = True
                break
    return best


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
        prog = generate(seed, args.stmts, args.max_depth)
        src, expected = prog.render()
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
                src, expected = prog.render()
                got = compile_and_run(src, comp, args.timeout)
            save_regression(seed, src, expected, got, args.regress_dir)
            fail += 1

        if (i+1) % 100 == 0:
            print(f"  [{i+1}] pass={ok} fail={fail} crash={crash}")

    print(f"\n{ok} passed, {fail} failed, {crash} crashes")
    return 1 if (fail or crash) else 0

if __name__ == "__main__":
    sys.exit(main())
