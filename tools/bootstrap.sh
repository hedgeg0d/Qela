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
rm -rf "$tmp2"
printf '    ok\n'

printf '\nShipping binary: %s\n' "$OUT/s2"
