static unsigned char flags[512];
long qmain(void) {
	long i = 0;
	while (i < 512) { flags[i] = 1; i = i + 1; }
	long count = 0;
	i = 2;
	while (i < 512) {
		if (flags[i] != 0) {
			count = count + 1;
			long j = i + i;
			while (j < 512) { flags[j] = 0; j = j + i; }
		}
		i = i + 1;
	}
	return count & 255;
}
