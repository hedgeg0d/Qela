#!/bin/sh
# Each test declares its expectations in leading comments:
#   // expect-exit: 42
#   // expect-out: some text
set -u

QELA="${QELA:-./qela}"
OUT="tests/out"
mkdir -p "$OUT"

pass=0
fail=0

for src in tests/*.qela; do
	name=$(basename "$src" .qela)
	bin="$OUT/$name"

	want_exit=$(sed -n 's|^// expect-exit: ||p' "$src")
	want_out=$(sed -n 's|^// expect-out: ||p' "$src")
	[ -z "$want_exit" ] && want_exit=0

	if ! "$QELA" "$src" -o "$bin" 2>"$OUT/$name.err"; then
		printf 'FAIL %s: compile error\n' "$name"
		sed 's/^/     /' "$OUT/$name.err"
		fail=$((fail + 1))
		continue
	fi

	got_out=$("$bin" 2>&1)
	got_exit=$?

	if [ "$got_exit" != "$want_exit" ]; then
		printf 'FAIL %s: exit %s, want %s\n' "$name" "$got_exit" "$want_exit"
		fail=$((fail + 1))
	elif [ "$got_out" != "$want_out" ]; then
		printf 'FAIL %s: output %s, want %s\n' "$name" "$got_out" "$want_out"
		fail=$((fail + 1))
	else
		size=$(stat -c %s "$bin")
		printf 'ok   %-16s %s bytes\n' "$name" "$size"
		pass=$((pass + 1))
	fi
done

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
