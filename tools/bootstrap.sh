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

printf '\nShipping binary: %s\n' "$OUT/s2"
