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
typedef struct { float x, y; } Vec2f;
typedef struct { double x, y; } Vec2d;
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
float c_v2_dot(Vec2f v) { return v.x * 3.0f + v.y * 4.0f; }
Vec2f c_v2_mk(float x, float y) { Vec2f v = {x, y}; return v; }
Vec2d c_v2d_add(Vec2d a, Vec2d b) { Vec2d r = {a.x + b.x, a.y + b.y}; return r; }
Vec2d qela_v2d_zero(void);
Vec2d c_call_qela_v2d(void) { Vec2d r = qela_v2d_zero(); r.x = r.x + 1.0; r.y = r.y + 2.0; return r; }
typedef struct { int64_t a, b, c, d; } Big;
int64_t c_big_sum(Big b) { return b.a + b.b + b.c + b.d; }
Big c_big_inc(Big b) { b.a = b.a + 100; return b; }
Big c_six_ret(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g) { Big r = {a, b, c, 0}; r.d = d + e + f + g; return r; }
int64_t c_spill7(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h, int64_t i) { return a + b + c + d + e + f + g + h + i; }
int64_t c_big_early(Big b, int64_t x, int64_t y, int64_t z, int64_t w) { return b.a + b.b + b.c + b.d + x + y + z + w; }
int64_t c_six_one(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, double x, int64_t g) { return a + b + c + d + e + f + g + (int64_t)x; }
double c_fsum9(float a, float b, float c, float d, float e, float f, float g, float h, float i) { return a + b + c + d + e + f + g + h + i; }
Big qela_big_sum(Big b);
Big qela_big_round(Big b);
int64_t c_call_qela_big(void) { Big b = {1, 2, 3, 4}; Big s = qela_big_sum(b); Big r = qela_big_round(b); return s.a + s.b + s.c + s.d + r.a * 1000; }
int64_t qela_spill_q(long a, long b, long c, long d, long e, long f, long g, long h, long i);
int64_t c_call_qela_spill(void) { return qela_spill_q(1, 2, 3, 4, 5, 6, 7, 8, 9); }
EOF

cat > carch.c <<'EOF'
#include <stdint.h>
int64_t c_arch_add(int64_t x, int64_t y) { return x + y; }
EOF

cat > main.qela <<'EOF'
struct Pair { a i64, b i64 }
struct Vec2f { x f32, y f32 }
struct Vec2d { x f64, y f64 }

extern fn c_add(a i64, b i64) i64;
extern fn c_pair_sum(p Pair) i64;
extern fn c_mk_pair(x i64, y i64) Pair;
extern fn c_fmul(x f64, y f64) f64;
extern fn c_call_qela() i64;
extern fn c_set_bss(v i64) void;
extern fn c_get_bss() i64;
extern fn c_arch_add(a i64, b i64) i64;
extern fn c_v2_dot(v Vec2f) f32;
extern fn c_v2_mk(x f32, y f32) Vec2f;
extern fn c_v2d_add(a Vec2d, b Vec2d) Vec2d;
extern fn c_call_qela_v2d() Vec2d;
extern var c_global i64;
struct Big { a i64, b i64, c i64, d i64 }

extern fn c_big_sum(b Big) i64;
extern fn c_big_inc(b Big) Big;
extern fn c_six_ret(a i64, b i64, c i64, d i64, e i64, f i64, g i64) Big;
extern fn c_spill7(a i64, b i64, c i64, d i64, e i64, f i64, g i64, h i64, i i64) i64;
extern fn c_big_early(b Big, x i64, y i64, z i64, w i64) i64;
extern fn c_six_one(a i64, b i64, c i64, d i64, e i64, f i64, x f64, g i64) i64;
extern fn c_fsum9(a f32, b f32, c f32, d f32, e f32, f f32, g f32, h f32, i f32) f64;
extern fn c_call_qela_big() i64;
extern fn c_call_qela_spill() i64;
extern fn qela_big_sum(b Big) Big {
	return b;
}
extern fn qela_big_round(b Big) Big {
	b.a = 777;
	return b;
}
extern fn qela_spill_q(a i64, b i64, c i64, d i64, e i64, f i64, g i64, h i64, i i64) i64 {
	return a + b + c + d + e + f + g + h + i;
}

fn qela_ping() i64 { return 7; }

extern fn qela_v2d_zero() Vec2d {
	var v Vec2d;
	v.x = 0.0;
	v.y = 0.0;
	return v;
}

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
	var v1 Vec2f = Vec2f{x: 1.0 as f32, y: 2.0 as f32};
	if ((c_v2_dot(v1) as i64) != 11) { fails = fails + 1; }
	var v2 Vec2f = c_v2_mk(3.0 as f32, 4.0 as f32);
	if ((v2.x as i64) != 3 || (v2.y as i64) != 4) { fails = fails + 1; }
	var vd1 Vec2d = Vec2d{x: 1.0, y: 2.0};
	var vd2 Vec2d = Vec2d{x: 3.0, y: 4.0};
	var vd3 Vec2d = c_v2d_add(vd1, vd2);
	if (vd3.x != 4.0 || vd3.y != 6.0) { fails = fails + 1; }
	var vd4 Vec2d = c_call_qela_v2d();
	if (vd4.x != 1.0 || vd4.y != 2.0) { fails = fails + 1; }

	// Parameters and returns past the register budgets, and aggregates
	// wider than 16 bytes by value, must match gcc's SysV exactly.
	var big Big = Big{a: 1, b: 2, c: 3, d: 4};
	if (c_big_sum(big) != 10) { fails = fails + 1; }
	var bc Big = c_big_inc(big);
	if (bc.a != 101 || bc.d != 4) { fails = fails + 1; }
	if (c_spill7(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45) { fails = fails + 1; }
	if (c_big_early(big, 10, 20, 30, 40) != 110) { fails = fails + 1; }
	if (c_six_one(1, 2, 3, 4, 5, 6, 2.0, 9) != 32) { fails = fails + 1; }
	var sr Big = c_six_ret(1, 2, 3, 4, 5, 6, 7);
	if (sr.a != 1 || sr.d != 22) { fails = fails + 1; }
	var fs f64 = c_fsum9(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5);
	if (fs < 49.4 || fs > 49.6) { fails = fails + 1; }
	// C calling Qela with the same shapes (a Big by value both ways).
	if (c_call_qela_big() != 777010) { fails = fails + 1; }
	if (c_call_qela_spill() != 45) { fails = fails + 1; }
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
	"$cc" -c -fno-pic -fno-asynchronous-unwind-tables -fno-stack-protector cbits.c -o cbits.o || exit 1
	"$cc" -c -fno-pic -fno-asynchronous-unwind-tables -fno-stack-protector carch.c -o carch.o || exit 1
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

