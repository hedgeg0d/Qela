static long fib(long n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
long qmain(void) { return fib(24) & 255; }
