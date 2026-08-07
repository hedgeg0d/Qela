#!/bin/sh
# M4 gate: emitted code must stay within 1.5x of gcc -Os.
#
# Each bench/NAME.qela has a bench/NAME.c doing the same work. Both are
# measured as pure machine code: gcc via the .text section, Qela as the file
# minus its 120 header bytes (these benches carry no string literals).
set -u

QELA="${QELA:-build/bootstrap/s2}"
CC="${CC:-gcc}"
OUT="${OUT:-build/bench}"
LIMIT_NUM=3
LIMIT_DEN=2

mkdir -p "$OUT"
[ -x "$QELA" ] || { printf 'no compiler at %s\n' "$QELA"; exit 1; }

fail=0
total_q=0
total_c=0

printf '%-10s %8s %8s %7s\n' bench qela 'gcc -Os' ratio
for src in bench/*.qela; do
	name=$(basename "$src" .qela)
	cref="bench/$name.c"
	[ -f "$cref" ] || continue

	"$QELA" "$src" -o "$OUT/$name" || { printf 'FAIL %s: compile\n' "$name"; fail=1; continue; }
	q=$(( $(stat -c %s "$OUT/$name") - 120 ))

	"$CC" -Os -c -o "$OUT/$name.o" "$cref" -ffreestanding -fno-stack-protector \
		-fno-asynchronous-unwind-tables -fomit-frame-pointer || { fail=1; continue; }
	c=$(size --format=sysv "$OUT/$name.o" | awk '/^\.text/ {print $2}')
	[ -z "$c" ] && c=0

	total_q=$((total_q + q))
	total_c=$((total_c + c))

	if [ "$c" -gt 0 ]; then
		pct=$((q * 100 / c))
		printf '%-10s %8s %8s %6s%%\n' "$name" "$q" "$c" "$pct"
		if [ $((q * LIMIT_DEN)) -gt $((c * LIMIT_NUM)) ]; then fail=1; fi
	else
		printf '%-10s %8s %8s %7s\n' "$name" "$q" "$c" '-'
	fi
done

[ "$total_c" -gt 0 ] && printf '%-10s %8s %8s %6s%%\n' TOTAL "$total_q" "$total_c" \
	"$((total_q * 100 / total_c))"

if [ "$fail" -eq 0 ]; then
	printf 'ok: within 1.5x of gcc -Os\n'
else
	printf 'FAIL: over the 1.5x budget\n'
fi
exit "$fail"
