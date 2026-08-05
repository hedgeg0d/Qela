#!/bin/sh
# Keeps stage1 sources inside the bootstrap subset. Rules: docs/BOOTSTRAP.md.
set -u

fail=0

report() {
	printf '\033[31m%s\033[0m %s\n' "$1" "$2"
	fail=1
}

# Drop comments and string bodies so diagnostic text is not mistaken for code.
strip() {
	sed -e 's|//.*||' -e 's|"[^"]*"|""|g' "$1"
}

for f in srcql/*.qela std/*.qela; do
	[ -e "$f" ] || continue

	for kw in comptime match defer; do
		if strip "$f" | grep -qw "$kw"; then
			report "$f:" "uses '$kw', forbidden in stage1 sources"
		fi
	done

	case "$f" in
	std/*) ;;
	*)
		if strip "$f" | grep -qE '(^|[^_[:alnum:]])syscall *\('; then
			report "$f:" "raw syscall, wrap it in std/sys.qela"
		fi
		;;
	esac

	strip "$f" | grep -o '^fn [a-zA-Z_0-9]*([^)]*)' | while read -r sig; do
		args=$(printf '%s' "$sig" | sed 's|^fn [a-zA-Z_0-9]*(||;s|)$||')
		[ -z "$args" ] && continue
		n=$(printf '%s' "$args" | tr ',' '\n' | grep -c .)
		[ "$n" -gt 6 ] && printf '\033[31m%s:\033[0m %s parameters in %s (max 6)\n' \
			"$f" "$n" "$sig"
	done
done

[ "$fail" -eq 0 ] && printf 'ok: stage1 sources are within the bootstrap subset\n'
exit "$fail"
