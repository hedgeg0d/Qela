#!/bin/sh
# Each test declares its expectations in leading comments:
#   // expect-exit: 42
#   // expect-out: some text
#   // expect-compile-error  the compiler must reject the file
#   // stage1-only        a feature stage0 does not have; skipped under ./qela
#   // interp-skip        nothing for `qela irun` to run: asm, naked, entry,
#                         extern -- machine-level by definition
#   // interp-todo        the interpreter does not support this yet
#
# TARGET=interp runs each test with `qela irun` instead of compiling it: one
# process, no binary, so there is no size to report.
set -u

QELA="${QELA:-./qela}"
TARGET="${TARGET:-x86_64}"
QEMU="${QEMU:-}"
TEST_TIMEOUT="${QELA_TEST_TIMEOUT:-10}"
# A program that uses the runtime ABI (eval/interpreted/dynamic) spawns the
# compiler itself as its child; when the runner points at S2, the tests find
# it through QELAPATH rather than whatever `qela` happens to be installed.
if [ "$QELA" != "./qela" ]; then
	export QELAPATH="$QELA"
fi
if [ "$TARGET" = "arm64" ]; then
	QELA="$QELA --target arm64"
elif [ "$TARGET" = "riscv64" ]; then
	QELA="$QELA --target riscv64"
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

	if [ "$TARGET" != "x86_64" ] && grep -q '^// x86-only' "$src"; then
		printf 'skip %-16s x86-only\n' "$name"
		continue
	fi
	if [ "$TARGET" != "arm64" ] && grep -q '^// arm64-only' "$src"; then
		printf 'skip %-16s arm64-only\n' "$name"
		continue
	fi
	if [ "$TARGET" = "riscv64" ] && grep -q '^// riscv-only' "$src"; then
		printf 'skip %-16s riscv-only\n' "$name"
		continue
	fi
	if [ "$TARGET" = "interp" ]; then
		if grep -q '^// x86-only' "$src"; then
			printf 'skip %-16s x86-only\n' "$name"
			continue
		fi
		if grep -q '^// interp-skip' "$src"; then
			printf 'skip %-16s interp-skip\n' "$name"
			continue
		fi
		if grep -q '^// interp-todo' "$src"; then
			printf 'skip %-16s interp-todo\n' "$name"
			continue
		fi
	fi

	want_exit=$(sed -n 's|^// expect-exit: ||p' "$src")
	want_out=$(sed -n 's|^// expect-out: ||p' "$src")
	[ -z "$want_exit" ] && want_exit=0

	want_reject=""
	if grep -q '^// expect-compile-error arm64' "$src"; then
		want_reject=1
		[ "$TARGET" = "arm64" ] || want_reject=0
	elif grep -q '^// expect-compile-error riscv64' "$src"; then
		want_reject=1
		[ "$TARGET" = "riscv64" ] || want_reject=0
	elif grep -q '^// expect-compile-error x86' "$src"; then
		want_reject=1
		[ "$TARGET" != "arm64" ] && [ "$TARGET" != "riscv64" ] || want_reject=0
	elif grep -q '^// expect-compile-error' "$src"; then
		want_reject=1
	fi
	if [ "$TARGET" = "interp" ]; then
		# One process does both halves, so --no-warn keeps compile
		# diagnostics out of what the program itself printed; the compiled
		# path gets the same separation by sending them to a file.
		if [ "$want_reject" = "1" ]; then
			if $QELA irun "$src" >/dev/null 2>&1; then
				printf 'FAIL %s: ran, but was expected to be rejected\n' "$name"
				fail=$((fail + 1))
			else
				printf 'ok   %-16s rejected\n' "$name"
				pass=$((pass + 1))
			fi
			continue
		fi
		got_out=$(timeout "$TEST_TIMEOUT" $QELA irun --no-warn "$src" 2>&1)
		got_exit=$?
		if [ "$got_exit" != "$want_exit" ]; then
			printf 'FAIL %s: exit %s, want %s\n' "$name" "$got_exit" "$want_exit"
			fail=$((fail + 1))
		elif [ "$got_out" != "$want_out" ]; then
			printf 'FAIL %s: output %s, want %s\n' "$name" "$got_out" "$want_out"
			fail=$((fail + 1))
		else
			printf 'ok   %-16s interp\n' "$name"
			pass=$((pass + 1))
		fi
		continue
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
		got_out=$(timeout "$TEST_TIMEOUT" "$QEMU" "$bin" 2>&1)
	else
		got_out=$(timeout "$TEST_TIMEOUT" "$bin" 2>&1)
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
