QELA ?= ../../build/bootstrap/s2

all: kernel

kernel: user.elf
	$(QELA) . --base 0x100000 -o kernel.elf

user.elf: ../minios_user/hello.qela
	$(QELA) --base 0x1000000 ../minios_user/hello.qela -o user.elf

run: kernel
	qemu-system-x86_64 -kernel kernel.elf -serial stdio -display none -no-reboot

clean:
	rm -f kernel.elf user.elf

.PHONY: all run clean
