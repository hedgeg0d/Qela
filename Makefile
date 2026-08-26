CC ?= gcc

CFLAGS := -std=gnu23 -Oz -ffreestanding -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter

LDFLAGS := -nostdlib -nostartfiles -static -no-pie \
           -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,noseparate-code

SRC := src/rt.c src/diag.c src/lex.c src/type.c src/parse.c src/codegen.c src/elf.c src/opt.c src/comptime.c src/main.c
OBJ := $(SRC:.c=.o) src/start.o
BIN := qela

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)
	strip --strip-all $@
	objcopy --remove-section=.comment $@

%.o: %.c src/qela.h src/sys.h src/comp.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/start.o: src/start.S
	$(CC) $(CFLAGS) -c -o $@ $<

check: $(BIN)
	@tools/check-size.sh $(BIN)

# The real compiler: stage1 compiled by itself, verified by the bootstrap
# gate. build/bootstrap/s2 is the shipped binary; ./qela never ships.
build: $(BIN)
	@tools/bootstrap.sh

PREFIX ?= /usr/local
DESTDIR ?=

install: build
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 0755 build/bootstrap/s2 $(DESTDIR)$(PREFIX)/bin/qela

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all check build install clean
