#!/bin/sh
# The FFI gate: C objects (compiled by gcc, the only C toolchain use) are
# statically linked into a Qela binary by the compiler itself -- extern fn
# calls both ways, structs by value, floats, extern globals, bss, and a .a
# archive. Runs against the shipped S2 on x86_64, and under qemu on arm64
# and riscv64 when the cross toolchains are installed.
set -u
ROOT=$(dirname "$0")/..
S2="${1:-$ROOT/build/bootstrap/s2}"
case "$S2" in
	/*) ;;
	*) S2="$PWD/$S2" ;;
esac
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

cat > cbits.c <<'EOF'
#include <stdint.h>
typedef struct { int64_t a, b; } Pair;
int64_t c_add(int64_t x, int64_t y) { return x + y; }
int64_t c_pair_sum(Pair p) { return p.a + p.b; }
Pair c_mk_pair(int64_t x, int64_t y) { Pair p = {x, y}; return p; }
double c_fmul(double x, double y) { return x * y; }
int64_t c_global = 42;
int64_t qela_ping(void);
int64_t c_call_qela(void) { return qela_ping() + c_global; }
int64_t c_bss;
void c_set_bss(int64_t v) { c_bss = v; }
int64_t c_get_bss(void) { return c_bss; }
EOF

cat > carch.c <<'EOF'
#include <stdint.h>
int64_t c_arch_add(int64_t x, int64_t y) { return x + y; }
EOF

cat > main.qela <<'EOF'
struct Pair { a i64, b i64 }

extern fn c_add(a i64, b i64) i64;
extern fn c_pair_sum(p Pair) i64;
extern fn c_mk_pair(x i64, y i64) Pair;
extern fn c_fmul(x f64, y f64) f64;
extern fn c_call_qela() i64;
extern fn c_set_bss(v i64) void;
extern fn c_get_bss() i64;
extern fn c_arch_add(a i64, b i64) i64;
extern var c_global i64;

fn qela_ping() i64 { return 7; }

fn main() int {
	var fails i64 = 0;
	if (c_add(2, 3) != 5) { fails = fails + 1; }
	var p Pair = Pair{a: 100, b: 23};
	if (c_pair_sum(p) != 123) { fails = fails + 1; }
	var q Pair = c_mk_pair(7, 8);
	if (q.a != 7 || q.b != 8) { fails = fails + 1; }
	var f f64 = c_fmul(2.5, 4.0);
	if (f < 9.99 || f > 10.01) { fails = fails + 1; }
	if (c_global != 42) { fails = fails + 1; }
	if (c_call_qela() != 49) { fails = fails + 1; }
	c_set_bss(77);
	if (c_get_bss() != 77) { fails = fails + 1; }
	c_global = 5;
	if (c_call_qela() != 12) { fails = fails + 1; }
	if (c_arch_add(40, 2) != 42) { fails = fails + 1; }
	return fails as int;
}
EOF

cat > broken.qela <<'EOF'
extern fn c_add(a i64, b i64) i64;
fn main() int { return c_add(1, 2) as int; }
EOF

run_target() {
	target="$1"
	cc="$2"
	qemu="$3"
	[ -x "$(command -v "$cc")" ] || { printf '    skip %s (no %s)\n' "$target" "$cc"; return 0; }
	"$cc" -c -fno-pic -fno-asynchronous-unwind-tables cbits.c -o cbits.o || exit 1
	"$cc" -c -fno-pic -fno-asynchronous-unwind-tables carch.c -o carch.o || exit 1
	ar rcs libcarch.a carch.o || exit 1
	"$S2" --target "$target" main.qela cbits.o libcarch.a -o app || exit 1
	if [ -n "$qemu" ]; then
		"$qemu" ./app || exit 1
	else
		./app || exit 1
	fi
	if "$S2" --target "$target" broken.qela -o broken_bin 2>/dev/null; then
		exit 1
	fi
	printf '    ok   %s\n' "$target"
}

run_target x86_64 gcc ""
run_target arm64 aarch64-linux-gnu-gcc qemu-aarch64
run_target riscv64 riscv64-linux-gnu-gcc qemu-riscv64

