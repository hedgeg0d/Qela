#!/bin/sh
# Enforces the core invariants: under 1 MiB, no libc, no dynamic loader.
set -eu

BIN="${1:-qela}"
LIMIT=$((1024 * 1024))
WARN=$((512 * 1024))

size=$(stat -c %s "$BIN")
printf 'size: %s bytes (%s KiB)\n' "$size" "$((size / 1024))"

fail=0

if [ "$size" -gt "$LIMIT" ]; then
	printf 'FAIL: exceeds 1 MiB budget by %s bytes\n' "$((size - LIMIT))"
	fail=1
elif [ "$size" -gt "$WARN" ]; then
	printf 'WARN: over the 512 KiB target, %s bytes of headroom left\n' \
		"$((LIMIT - size))"
fi

if readelf -d "$BIN" 2>/dev/null | grep -q NEEDED; then
	printf 'FAIL: has dynamic dependencies\n'
	fail=1
fi

if readelf -l "$BIN" 2>/dev/null | grep -q INTERP; then
	printf 'FAIL: requests a dynamic loader\n'
	fail=1
fi

[ "$fail" -eq 0 ] && printf 'ok: static, freestanding, within budget\n'
exit "$fail"
