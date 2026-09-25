#!/usr/bin/env python3
"""
diag-test.py — how diagnostics are rendered: escapes and cascades.

The escape part: the coloured diagnostics must be real ANSI escapes.

The escape byte used to be written as the text `\033`, which the language
does not know: the output carried a NUL and the characters "33[", so every
diagnostic rendered as garbage on a terminal. Piped runs never showed it,
because auto colour is off without a tty, which is why this check asks for
colour explicitly.

The colour decision is part of this too: "auto" must ask about the stream the
diagnostics go to (stderr), not about stdin. With stdin on a terminal and
stderr in a file the escapes used to be written into the file, and a run with
everything redirected still coloured its log.

The cascade part: a soft error poisons its node and the compile goes on, so
one run can report several independent mistakes. A check that does not know
about the poison type reports a second, useless error on top of the real
one -- seen in interpolation, in a member access, and in an index. The real
diagnostic must be there, the poison one must not.

QELA is the compiler to test, set by the caller.
"""
import os, pty, subprocess, sys, tempfile

qela = os.environ.get("QELA", "./build/bootstrap/s2")


def compile_colored(src: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".qela", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        p = subprocess.run([qela, "--color=always", path, "-o", path + ".bin"],
                           capture_output=True, text=True, timeout=120)
        return p.stdout + p.stderr
    finally:
        for p_ in (path, path + ".bin"):
            try:
                os.unlink(p_)
            except OSError:
                pass


CASCADE = ('import "std/io.qela";\n'
           'fn main() int { var counter i64 = 1; println("${countre}"); return 0; }\n')
BAD = 'import "std/io.qela";\nfn main() int { println("${nosuchvar}"); return 0; }\n'
WARN = 'import "std/io.qela";\nfn main() int { var unused i64 = 1; println("x"); return 0; }\n'

fails = []

out = compile_colored(BAD)
if "\x1b[31m" not in out:
    fails.append("an error is not coloured red")
if "\x1b[0m" not in out:
    fails.append("the reset is missing")
if "\x00" in out:
    fails.append("a NUL reached the output")

out = compile_colored(WARN)
if "\x1b[33m" not in out:
    fails.append("a warning is not coloured yellow")

out = compile_colored(CASCADE)
if "undefined function 'countre'" not in out:
    fails.append("the real error is missing")
if "cannot interpolate" in out:
    fails.append("a poisoned part reported a second, useless error")

def colored_with_stderr_tty():
    """The diagnostics' own stream is a terminal: colour is expected."""
    with tempfile.NamedTemporaryFile("w", suffix=".qela", delete=False) as f:
        f.write(BAD)
        path = f.name
    m, s = pty.openpty()
    try:
        subprocess.run([qela, path, "-o", path + ".bin"], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=s, timeout=60)
    finally:
        os.close(s)
    out = b""
    while True:
        try:
            chunk = os.read(m, 4096)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    os.close(m)
    try:
        os.unlink(path)
    except OSError:
        pass
    return out


def colored_with_stderr_pipe():
    """stdin is a terminal but stderr is not: no colour may appear."""
    with tempfile.NamedTemporaryFile("w", suffix=".qela", delete=False) as f:
        f.write(BAD)
        path = f.name
    m, s = pty.openpty()
    try:
        p = subprocess.run([qela, path, "-o", path + ".bin"], stdin=s,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    finally:
        os.close(s)
        os.close(m)
    try:
        os.unlink(path)
    except OSError:
        pass
    return p.stderr


if b"\x1b[" not in colored_with_stderr_tty():
    fails.append("a terminal on stderr got no colour")
if b"\x1b[" in colored_with_stderr_pipe():
    fails.append("escapes were written while stderr was not a terminal")

NARROW = ('import "std/io.qela";\n'
          'fn cube(n i32) i32 { return n * n * n; }\n'
          'fn main() int { return 0; }\n')
out = compile_colored(NARROW)
if "implicit conversion" not in out:
    fails.append("the narrowing error is missing")
if "both operands are narrower" not in out:
    fails.append("the width rule is not explained next to the error")

MEMBER = ('import "std/io.qela";\nstruct P { x i64 }\n'
          'fn main() int { var p P; println("${nosuchz.pos}"); return 0; }\n')
INDEX = ('import "std/io.qela";\n'
         'fn main() int { var a []i64; println("${nosuchz[0]}"); return 0; }\n')

DEREF = ('import "std/io.qela";\n'
         'fn main() int { var v i64 = *nosuchz; return 0; }\n')
for what, src in (("a member on a poisoned base", MEMBER),
                  ("an index on a poisoned value", INDEX),
                  ("a dereference of a poisoned value", DEREF)):
    out = compile_colored(src)
    if "undefined function 'nosuchz'" not in out:
        fails.append("the real error is missing for %s" % what)
    if "poison" in out:
        fails.append("a poisoned node reported a second error for %s" % what)

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("    ok   diagnostics: escapes, cascades")
