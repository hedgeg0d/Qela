// Qela's extern ABI puts floats in integer registers (a Qela float is raw
// bits in a GPR), while SysV -- what raylib and libc expect -- wants them in
// XMM registers. So the two float-touching calls get a shim: the float comes
// across as an integer, is reinterpreted here, and raylib is called with the
// real float. Everything else (ints, pointers, bool, Color by value) crosses
// directly and needs no glue.
#include <raylib.h>
#include <string.h>

long long gl_frame_time_bits(void) {
    float f = GetFrameTime();
    long long b;
    memcpy(&b, &f, 4);
    return b;
}

void gl_draw_circle(int x, int y, int r, Color c) {
    DrawCircle(x, y, (float)r, c);
}
