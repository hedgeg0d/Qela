#!/bin/sh
# Byte-identity gate for codegen migration (M): every corpus program must
# compile to exactly the same bytes as the frozen reference compiler. Any
# drift means a lowering change altered emitted code -- the goal of the M
# phase is a byte-identical rewrite, so the build must stay clean here.
#
# The reference is tools/ref/s2, the S2 built before migration started. It is
# outside git; see tools/ref/README. Freeze a fresh one with:
#   mkdir -p tools/ref && cp build/bootstrap/s2 tools/ref/s2
#
# Expect-compile-error tests produce no binary and are skipped; everything
# else must match byte for byte.
set -u

QELA="${QELA:-./build/bootstrap/s2}"
REF="${REF:-./tools/ref/s2}"
OUT="tests/out-byte"

[ -x "$QELA" ] || { echo "no $QELA (run make build)" >&2; exit 1; }
[ -x "$REF" ] || { echo "no reference $REF (freeze one, see header)" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

pass=0
fail=0

for src in tests/*.qela; do
	name=$(basename "$src" .qela)

	if grep -q '^// expect-compile-error' "$src"; then
		printf 'skip %-16s expect-compile-error\n' "$name"
		continue
	fi

	if ! "$QELA" "$src" -o "$OUT/$name.new" 2>"$OUT/$name.new.err"; then
		printf 'FAIL %s: new compiler rejects it\n' "$name"
		fail=$((fail + 1))
		continue
	fi
	if ! "$REF" "$src" -o "$OUT/$name.ref" 2>"$OUT/$name.ref.err"; then
		printf 'FAIL %s: reference rejects it\n' "$name"
		fail=$((fail + 1))
		continue
	fi

	if cmp -s "$OUT/$name.new" "$OUT/$name.ref"; then
		printf 'ok   %-16s %s bytes\n' "$name" "$(stat -c %s "$OUT/$name.new")"
		pass=$((pass + 1))
	else
		printf 'DIFF %-16s bytes differ\n' "$name"
		cmp -l "$OUT/$name.new" "$OUT/$name.ref" 2>/dev/null | head -5
		fail=$((fail + 1))
	fi
done

rm -rf "$OUT"
printf '\n%s matched, %s differ\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
