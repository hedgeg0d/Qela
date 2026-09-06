#!/bin/sh
# Each test declares its expectations in leading comments:
#   // expect-exit: 42
#   // expect-out: some text
#   // expect-compile-error  the compiler must reject the file
#   // stage1-only        a feature stage0 does not have; skipped under ./qela
set -u

QELA="${QELA:-./qela}"
TARGET="${TARGET:-x86_64}"
QEMU="${QEMU:-}"
if [ "$TARGET" = "arm64" ]; then
	QELA="$QELA -target arm64"
fi
OUT="tests/out"
mkdir -p "$OUT"

pass=0
fail=0

for src in tests/*.qela; do
	name=$(basename "$src" .qela)
	bin="$OUT/$name"

	if grep -q '^// stage1-only' "$src" && [ "$QELA" = "./qela" ]; then
		printf 'skip %-16s stage1-only\n' "$name"
		continue
	fi

	if [ "$TARGET" = "arm64" ] && grep -q '^// x86-only' "$src"; then
		printf 'skip %-16s x86-only\n' "$name"
		continue
	fi
	if [ "$TARGET" != "arm64" ] && grep -q '^// arm64-only' "$src"; then
		printf 'skip %-16s arm64-only\n' "$name"
		continue
	fi

	want_exit=$(sed -n 's|^// expect-exit: ||p' "$src")
	want_out=$(sed -n 's|^// expect-out: ||p' "$src")
	[ -z "$want_exit" ] && want_exit=0

	want_reject=""
	if grep -q '^// expect-compile-error arm64' "$src"; then
		want_reject=1
		[ "$TARGET" = "arm64" ] || want_reject=0
	elif grep -q '^// expect-compile-error x86' "$src"; then
		want_reject=1
		[ "$TARGET" != "arm64" ] || want_reject=0
	elif grep -q '^// expect-compile-error' "$src"; then
		want_reject=1
	fi
	if [ "$want_reject" = "1" ]; then
		if $QELA "$src" -o "$bin" >/dev/null 2>&1; then
			printf 'FAIL %s: compiled, but was expected to be rejected\n' "$name"
			fail=$((fail + 1))
		else
			printf 'ok   %-16s rejected\n' "$name"
			pass=$((pass + 1))
		fi
		continue
	fi

	if ! $QELA "$src" -o "$bin" 2>"$OUT/$name.err"; then
		printf 'FAIL %s: compile error\n' "$name"
		sed 's/^/     /' "$OUT/$name.err"
		fail=$((fail + 1))
		continue
	fi

	if [ -n "$QEMU" ]; then
		got_out=$("$QEMU" "$bin" 2>&1)
	else
		got_out=$("$bin" 2>&1)
	fi
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
