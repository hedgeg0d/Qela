long qmain(void) {
	long s = 0, i = 0;
	while (i < 1000) { s = s + i * 3 - (i & 7); i = i + 1; }
	return s & 255;
}
