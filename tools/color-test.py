#!/usr/bin/env python3
"""
color-test.py — the coloured diagnostics must be real ANSI escapes.

The escape byte used to be written as the text `\033`, which the language
does not know: the output carried a NUL and the characters "33[", so every
diagnostic rendered as garbage on a terminal. Piped runs never showed it,
because auto colour is off without a tty, which is why this check asks for
colour explicitly.

QELA is the compiler to test, set by the caller.
"""
import os, subprocess, sys, tempfile

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

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("    ok   coloured diagnostics")
