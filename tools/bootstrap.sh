#!/bin/sh
# Self-hosting gate (M7):
#   S1a = stage0(stage1), S2 = S1a(stage1), S3 = S2(stage1), then S2 == S3.
# S2 is what ships. Fails until stage1 is complete.
set -u

STAGE0="${STAGE0:-./qela}"
ENTRY="${ENTRY:-srcql/main.qela}"
OUT="${OUT:-build/bootstrap}"

mkdir -p "$OUT"

step() { printf '\033[1m==> %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m: %s\n' "$1"; exit 1; }

[ -x "$STAGE0" ] || fail "no stage0 at $STAGE0 (run make)"
[ -f "$ENTRY" ] || fail "no entry point at $ENTRY"

# A stale blob would ship an old standard library.
step "regenerate the embedded stdlib"
python3 tools/genblob.py || fail "genblob failed"

# An unfinished stage1 can exit 0 without writing anything, so check the file.
# Anything after $3 is passed to the compiler: the stage0 step uses it to
# define BOOTSTRAP=1, which drops srcql/interp.qela from S1a. See
# docs/BOOTSTRAP.md -- stage0 cannot compile the interpreter, and S1a never
# ships, so only S2 and S3 carry it and the S2 == S3 fixed point still covers
# its sources.
compile() {
	cc="$1"
	name="$2"
	out="$3"
	shift 3
	rm -f "$out"
	"$cc" "$ENTRY" "$@" -o "$out" || fail "$name failed to compile stage1"
	[ -s "$out" ] || fail "$name produced no $out (stage1 is still incomplete)"
	chmod +x "$out"
	printf '    %s bytes\n' "$(stat -c %s "$out")"
}

step "S1a = stage0($ENTRY)"
compile "$STAGE0" stage0 "$OUT/s1a" -D BOOTSTRAP=1

step "S2 = S1a($ENTRY)"
compile "$OUT/s1a" S1a "$OUT/s2"

step "S3 = S2($ENTRY)"
compile "$OUT/s2" S2 "$OUT/s3"

step "cmp S2 S3"
if ! cmp "$OUT/s2" "$OUT/s3"; then
	printf '\nS2 and S3 differ: look for non-determinism.\n'
	printf 'See docs/BOOTSTRAP.md, section "Determinism".\n'
	cmp -l "$OUT/s2" "$OUT/s3" 2>/dev/null | head -20
	exit 1
fi

printf '\n\033[32mM7 GATE PASSED\033[0m: S2 == S3, %s bytes\n' \
	"$(stat -c %s "$OUT/s2")"

step "test corpus under S2"
QELA="$OUT/s2" tools/run-tests.sh || fail "S2 does not pass the test corpus"

step "test corpus interpreted (qela irun)"
QELA="$OUT/s2" TARGET=interp tools/run-tests.sh ||
	fail "the interpreter does not pass the test corpus"

# The compiler run by its own interpreter has to emit what the compiled one
# emits, byte for byte. hello keeps this cheap; interpreting the compiler
# compiling the whole compiler is the same check at ~550x the wall time and
# is run by hand (docs/STATUS.md records the last measurement).
step "self-interpretation"
"$OUT/s2" tests/hello.qela -o "$OUT/hello.compiled" >/dev/null 2>&1 ||
	fail "S2 cannot compile tests/hello.qela"
"$OUT/s2" irun --no-warn srcql/main.qela tests/hello.qela -o "$OUT/hello.interp" >/dev/null 2>&1 ||
	fail "the interpreted compiler failed to compile tests/hello.qela"
cmp "$OUT/hello.compiled" "$OUT/hello.interp" ||
	fail "the interpreted compiler emitted different bytes"
printf '    ok\n'

step "embedded stdlib, outside the source tree"
root=$(pwd)
tmp=$(mktemp -d)
tmp2=$(mktemp -d)
cat > "$tmp/embed.qela" <<'EOF'
import "std/io.qela";
import "std/fmt.qela";
fn main() int {
	var b Buf;
	fmt_i64(&b, 6 * 7);
	if (b.n == 2 && b.p[0] == '4' && b.p[1] == '2') { return 0; }
	return 1;
}
EOF
( cd "$tmp" && "$root/$OUT/s2" embed.qela -o embed && ./embed ) ||
	fail "the embedded stdlib does not resolve without std/ on disk"
rm -rf "$tmp"
printf '    ok\n'

step "coroutines"
cat > "$tmp2/coro.qela" <<'EOF'
import "std/coro.qela";
import "std/fmt.qela";
var log Buf;
fn worker(id i64, rounds i64) {
	var i i64 = 0;
	while (i < rounds) { fmt_i64(&log, id); coro_yield(); i = i + 1; }
}
fn main() int {
	spawn worker(1, 3);
	spawn worker(2, 3);
	spawn worker(3, 2);
	coro_run_all();
	if (log.n != 8) { return 1; }
	if (log.p[0] != '1' || log.p[1] != '2' || log.p[2] != '3') { return 2; }
	if (log.p[6] != '1' || log.p[7] != '2') { return 3; }
	return 0;
}
EOF
( cd "$tmp2" && "$root/$OUT/s2" coro.qela -o coro && ./coro ) ||
	fail "coroutines do not interleave as expected"
step "channels"
cat > "$tmp2/chan.qela" <<'EOF'
import "std/chan.qela";
struct Msg { id i64, amount i64, }
var ch Chan(i64);
var mch Chan(Msg);
var got i64 = 0;
var total i64 = 0;
fn producer(n i64, base i64) {
	var i i64 = 0;
	while (i < n) { ch <- base + i; i = i + 1; }
}
fn consumer(n i64) {
	var i i64 = 0;
	while (i < n) { got = got + <-ch; i = i + 1; }
}
fn sender(n i64) {
	var i i64 = 0;
	while (i < n) {
		var m Msg;
		m.id = i;
		m.amount = i * 3;
		mch <- m;
		i = i + 1;
	}
}
fn receiver(n i64) {
	var i i64 = 0;
	while (i < n) {
		var m Msg = <-mch;
		total = total + m.amount;
		i = i + 1;
	}
}
fn main() int {
	chan_init(&ch, 2);
	chan_init(&mch, 2);
	spawn producer(4, 10);
	spawn producer(3, 20);
	spawn consumer(7);
	spawn sender(5);
	spawn receiver(5);
	coro_run_all();
	if (chan_len(&ch) != 0) { return 1; }
	if (got != 109) { return 2; }
	if (total != 30) { return 3; }
	return 0;
}
EOF
( cd "$tmp2" && "$root/$OUT/s2" chan.qela -o chan && ./chan ) ||
	fail "channels do not deliver every value"
printf '    ok\n'

step "garbage collector"
cat > "$tmp2/gc.qela" <<'EOF'
import "std/gc.qela";
struct Node { val i64, next *Node, }
var head *Node;
fn push(v i64) {
	var n *Node = gc_alloc(sizeof(Node)) as *Node;
	n.val = v;
	n.next = head;
	head = n;
}
fn sum() i64 {
	var t i64 = 0;
	var p *Node = head;
	while (p as i64 != 0) { t = t + p.val; p = p.next; }
	return t;
}
fn main() int {
	var i i64 = 0;
	while (i < 50) { push(i); i = i + 1; }
	var before i64 = sum();
	i = 0;
	while (i < 2000) { var junk *u8 = gc_alloc(64); junk[0] = 1; i = i + 1; }
	var peak i64 = gc_live_bytes();
	gc_collect();
	var kept i64 = gc_live_bytes();
	if (sum() != before) { return 1; }
	if (kept >= peak) { return 2; }
	if (kept == 0) { return 3; }
	return 0;
}
EOF
( cd "$tmp2" && "$root/$OUT/s2" gc.qela -o gc && ./gc ) ||
	fail "the collector loses reachable objects or reclaims nothing"
printf '    ok\n'

step "run and fmt"
cat > "$tmp2/tool.qela" <<'EOF'
struct P{x int,y int,}
fn area(p P)int{
   var a int=p.x*p.y;
if(a>10){return a;}else{return 0;}
}
fn main()int{var p P=P{x:3,y:4};return area(p);}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" fmt tool.qela > one.qela &&
  "$root/$OUT/s2" fmt one.qela > two.qela &&
  cmp -s one.qela two.qela &&
  "$root/$OUT/s2" run one.qela; [ $? -eq 12 ] ) ||
	fail "fmt is not idempotent, or run does not forward the exit status"
printf '    ok\n'

step "interpolation and repl"
cat > "$tmp2/interp.qela" <<'EOF'
import "std/io.qela";
fn main() int {
	var n i64 = 42;
	var s str = "hi";
	write_str(STDOUT, "n = ${n}, s = ${s}, sum = ${1 + 2}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" fmt interp.qela > interp_fmt.qela &&
  "$root/$OUT/s2" fmt interp_fmt.qela > interp_fmt2.qela &&
  cmp -s interp_fmt.qela interp_fmt2.qela &&
  "$root/$OUT/s2" interp_fmt.qela -o interp &&
  [ "$(./interp)" = "n = 42, s = hi, sum = 3" ] &&
  [ "$(printf '1 + 2\n"x"\n' | "$root/$OUT/s2" repl)" = "3x" ] &&
  [ "$(printf 'rand_range(1, 7)\n' | "$root/$OUT/s2" repl)" = "5" ] ) ||
	fail "interpolation or the repl misbehaves"
printf '    ok\n'


step "eval() over the abi subprocess"
cat > "$tmp2/evaltest.qela" <<'EOF'
import "std/eval.qela";
import "std/io.qela";
fn main() int {
	var a i64 = eval("6 * 7");
	eval("var x = 100;");
	var b i64 = eval("x - 2");
	write_str(STDOUT, "a=${a} b=${b}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" evaltest.qela -o evaltest &&
  [ "$(QELAPATH="$root/$OUT/s2" ./evaltest)" = "a=42 b=98" ] ) ||
	fail "eval() does not round-trip through the abi subprocess"
printf '    ok\n'


step "interpreted/dynamic call-site trampolines"
cat > "$tmp2/trampoline.qela" <<'EOF'
import "std/io.qela";
fn interpreted square(x i64) i64 {
	return x * x;
}
fn dynamic add3(a i64, b i64, c i64) i64 {
	return a + b + c;
}
fn main() int {
	var s i64 = square(7);
	var t i64 = add3(1, 2, 3);
	var s2 i64 = square(9);
	write_str(STDOUT, "s=${s} t=${t} s2=${s2}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" trampoline.qela -o trampoline &&
  [ "$(QELAPATH="$root/$OUT/s2" ./trampoline)" = "s=49 t=6 s2=81" ] &&
  [ "$(QELAPATH="$root/$OUT/s2" "$root/$OUT/s2" irun trampoline.qela)" = "s=49 t=6 s2=81" ] ) ||
	fail "an interpreted/dynamic function misbehaves under AOT or irun"
printf '    ok\n'


step "eval fn: host functions callable from eval'd source"
cat > "$tmp2/evalfn.qela" <<'EOF'
import "std/eval.qela";
import "std/io.qela";
eval fn apply_discount(price i64) i64 {
	return price - 10;
}
fn main() int {
	var r i64 = eval("apply_discount(100)");
	write_str(STDOUT, "r=${r}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" evalfn.qela -o evalfn &&
  [ "$(QELAPATH="$root/$OUT/s2" ./evalfn)" = "r=90" ] ) ||
	fail "eval fn does not bridge a host function to eval'd source"
printf '    ok\n'


step "eval var: host globals readable and writable from eval'd source"
cat > "$tmp2/evalvar.qela" <<'EOF'
import "std/eval.qela";
import "std/io.qela";
eval var price i64 = 100;
var secret i64 = 12345;
fn main() int {
	var a i64 = eval("price * 2");
	var b i64 = eval("secret");
	eval("price = price + 50;");
	write_str(STDOUT, "a=${a} b=${b} price=${price}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" evalvar.qela -o evalvar &&
  [ "$(QELAPATH="$root/$OUT/s2" ./evalvar)" = "a=200 b=0 price=150" ] ) ||
	fail "eval var does not expose an exported global, leaks an unexported one, or double-applies a write"
printf '    ok\n'


step "interpreted functions calling each other"
cat > "$tmp2/interpchain.qela" <<'EOF'
import "std/io.qela";
fn interpreted square(x i64) i64 {
	return x * x;
}
fn interpreted quad(x i64) i64 {
	return square(square(x));
}
fn main() int {
	write_str(STDOUT, "r=${quad(2)}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" interpchain.qela -o interpchain &&
  [ "$(QELAPATH="$root/$OUT/s2" ./interpchain)" = "r=16" ] &&
  [ "$(QELAPATH="$root/$OUT/s2" "$root/$OUT/s2" irun interpchain.qela)" = "r=16" ] ) ||
	fail "one interpreted function cannot call another"
printf '    ok\n'


step "--interpreted and --jit global flags"
cat > "$tmp2/globalflag.qela" <<'EOF'
import "std/io.qela";
fn square(x i64) i64 {
	return x * x;
}
fn add3(a i64, b i64, c i64) i64 {
	return a + b + c;
}
fn main() int {
	var s i64 = square(7);
	var t i64 = add3(1, 2, 3);
	fmt_i64_to_fd(STDOUT, s);
	write_str(STDOUT, " ");
	fmt_i64_to_fd(STDOUT, t);
	write_str(STDOUT, "\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" --interpreted globalflag.qela -o globalflag_i &&
  [ "$(QELAPATH="$root/$OUT/s2" ./globalflag_i)" = "49 6" ] &&
  "$root/$OUT/s2" --jit globalflag.qela -o globalflag_j &&
  [ "$(QELAPATH="$root/$OUT/s2" ./globalflag_j)" = "49 6" ] ) ||
	fail "--interpreted or --jit does not force every function through the abi"
printf '    ok\n'


step "dynamic: real native jit, and a clean fallback when it can't"
cat > "$tmp2/jitreal.qela" <<'EOF'
import "std/io.qela";
fn dynamic pure_square(x i64) i64 {
	return x * x;
}
fn dynamic notpure(x i64) i64 {
	write_str(STDOUT, "side effect\n");
	return x + 1;
}
fn main() int {
	var s i64 = pure_square(7);
	var r i64 = notpure(41);
	write_str(STDOUT, "s=${s} r=${r}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" jitreal.qela -o jitreal &&
  [ "$(QELAPATH="$root/$OUT/s2" ./jitreal 2>/dev/null)" = "$(printf 'side effect\ns=49 r=42')" ] ) ||
	fail "a self-contained dynamic function doesn't jit correctly, or a non-self-contained one doesn't fall back"
printf '    ok\n'


step "parse_ast/run_ast: parse once, run repeatedly against fresh state"
cat > "$tmp2/asttest.qela" <<'EOF'
import "std/eval.qela";
import "std/io.qela";
eval var price i64 = 10;
fn main() int {
	var a *Ast = parse_ast("price * 2");
	var r1 i64 = run_ast(a);
	eval("price = 100;");
	var r2 i64 = run_ast(a);
	write_str(STDOUT, "r1=${r1} r2=${r2}\n");
	return 0;
}
EOF
( cd "$tmp2" &&
  "$root/$OUT/s2" asttest.qela -o asttest &&
  [ "$(QELAPATH="$root/$OUT/s2" ./asttest)" = "r1=20 r2=200" ] ) ||
	fail "parse_ast/run_ast does not cache and re-run against live state"
printf '    ok\n'


step "stdin compile and shebang"
cat > "$tmp2/btcrash.qela" <<'EOF'
fn deep(n i64) i64 {
	if (n == 0) { assert(n > 0, "recursion went wrong"); }
	return n;
}
fn main() int {
	return deep(0) as int;
}
EOF
( printf 'fn main() int { return 5; }\n' |
    "$root/$OUT/s2" run -; [ $? -eq 5 ] ) ||
	fail "qela run - does not compile and run stdin"
( cd "$tmp2" &&
  printf '#!%s run\nfn main() int { return 6; }\n' "$root/$OUT/s2" > sheb.qela &&
  chmod +x sheb.qela &&
  ./sheb.qela; [ $? -eq 6 ] ) ||
	fail "a shebang file does not run through qela"
printf '    ok\n'

step "panic backtrace"
if out=$(cd "$tmp2" && "$root/$OUT/s2" run btcrash.qela 2>&1); then
	fail "a failing assert must panic"
elif printf '%s' "$out" | grep -q 'recursion went wrong' &&
     printf '%s' "$out" | grep -q 'deep' &&
     printf '%s' "$out" | grep -q 'main'; then
	: # the assert message and both frame names are on stderr
else
	fail "panic backtrace is missing the message or a frame name"
fi
printf '    ok\n'

step "compiler flags"
# --backtrace on a plain compile, not just under qela run.
if out=$(cd "$tmp2" && "$root/$OUT/s2" btcrash.qela --backtrace -o btflag.bin 2>&1); then
	: # compiled
else
	fail "--backtrace compile failed"
fi
if out=$(cd "$tmp2" && ./btflag.bin 2>&1); then
	fail "a failing assert must panic"
elif printf '%s' "$out" | grep -q 'deep' && printf '%s' "$out" | grep -q 'main'; then
	: # the frame names are on stderr
else
	fail "--backtrace did not emit frame names"
fi
# --no-bounds-checks drops the check: the out-of-bounds read no longer panics.
cat > "$tmp2/nob.qela" <<'EOF'
fn main() int {
	var a [3]i64;
	var x i64 = a[7];
	return 0;
}
EOF
( cd "$tmp2" && "$root/$OUT/s2" nob.qela --no-bounds-checks -o nob.bin &&
  ./nob.bin; [ $? -eq 0 ] ) ||
	fail "--no-bounds-checks still panics on an out-of-bounds read"
# -g adds DWARF: the binary still runs and is larger than the plain one.
( cd "$tmp2" && "$root/$OUT/s2" nob.qela --no-bounds-checks -g -o nobg.bin &&
  ./nobg.bin; [ $? -eq 0 ] &&
  [ "$(stat -c %s nobg.bin)" -gt "$(stat -c %s nob.bin)" ] ) ||
	fail "-g did not produce a working, larger binary"
# --dump-std prints the embedded module source.
"$root/$OUT/s2" --dump-std fmt | grep -q 'fmt_str' ||
	fail "--dump-std did not print the embedded fmt module"
printf '    ok\n'

step "qela . project build"
mkdir -p "$tmp2/proj"
cat > "$tmp2/proj/plus.qela" <<'EOF'
fn add7(x i64) i64 {
	return x + 7;
}
EOF
cat > "$tmp2/proj/main.qela" <<'EOF'
import "std/str.qela";
fn main() int {
	var s str = "ab";
	if (!str_eq(s, "ab")) { return 1; }
	return add7(35) as int;
}
EOF
( cd "$tmp2/proj" && "$root/$OUT/s2" . && ./a.out; [ $? -eq 42 ] ) ||
	fail "qela . does not merge a directory into one program"
printf '    ok\n'

step "qela . lisp example"
( cd "$root/examples/lisp" && "$root/$OUT/s2" . -o "$tmp2/lisp" &&
  "$tmp2/lisp" tests.lisp | grep -q '^ok   procedure?' ) ||
	fail "the lisp example fails its own test script"
printf '    ok\n'

step "qela test"
cat > "$tmp2/tt.qela" <<'EOF'
import "std/sys.qela";
// expect-exit: 7
// expect-out: one line
// expect-out: two lines
fn main() int {
	write_str(1, "one line\n");
	write_str(1, "two lines\n");
	return 7;
}
EOF
cat > "$tmp2/tc.qela" <<'EOF'
// expect-compile-error
fn main() int { var x i64 = "no"; return 0; }
EOF
( "$root/$OUT/s2" test "$tmp2/tt.qela" &&
  "$root/$OUT/s2" test "$tmp2/tc.qela" &&
  "$root/$OUT/s2" test "$tmp2/tt.qela" "$tmp2/tc.qela" >/dev/null ) ||
	fail "qela test does not check expect-exit/expect-out/expect-compile-error"
printf '    ok\n'

step "language server"
QELA="$OUT/s2" python3 tools/lsp-test.py ||
	fail "the language server fails the scripted conversation"
printf '    ok\n'
rm -rf "$tmp2"

printf '\nShipping binary: %s\n' "$OUT/s2"
