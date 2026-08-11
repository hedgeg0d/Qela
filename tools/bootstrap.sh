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
compile() {
	rm -f "$3"
	"$1" "$ENTRY" -o "$3" || fail "$2 failed to compile stage1"
	[ -s "$3" ] || fail "$2 produced no $3 (stage1 is still incomplete)"
	chmod +x "$3"
	printf '    %s bytes\n' "$(stat -c %s "$3")"
}

step "S1a = stage0($ENTRY)"
compile "$STAGE0" stage0 "$OUT/s1a"

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
  [ "$(printf '1 + 2\n"x"\n' | "$root/$OUT/s2" repl)" = "3x" ] ) ||
	fail "interpolation or the repl misbehaves"
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
