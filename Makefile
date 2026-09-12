# Axion-Ban Kernel Makefile
# 交叉编译: MinGW GCC (生成汇编) + x86_64-elf-as (汇编) + x86_64-elf-ld (链接)

AS        = ./x86_64-elf/bin/as
LD        = ./x86_64-elf/bin/ld
OBJCOPY   = ./x86_64-elf/bin/objcopy
CC        = gcc                   # MinGW GCC

QEMU      = D:/qemu/qemu-system-x86_64

ASFLAGS64 = --64
CFLAGS    = -m64 -mno-red-zone -nostdlib -ffreestanding \
            -fno-builtin -fno-pic -fno-asynchronous-unwind-tables \
            -fno-exceptions -Wall -O2 -mcmodel=large -mabi=sysv \
            -Isrc/include -Isrc/kernel
LDFLAGS   = -T linker.ld -nostdlib

BOOT_S    = src/boot/boot.S
C_SOURCES = src/kernel/vga.c \
            src/kernel/keyboard.c \
            src/kernel/timer.c \
            src/kernel/mm.c \
            src/kernel/hotness.c \
            src/kernel/sched.c \
            src/kernel/doorbell.c \
            src/kernel/shell.c \
            src/kernel/panic.c \
            src/kernel/kmain.c

OBJS = $(BOOT_S:.S=.o) $(C_SOURCES:.c=.o)

.PHONY: all run debug iso clean

all: kernel.bin

# 汇编文件
src/boot/boot.o: $(BOOT_S)
	$(AS) $(ASFLAGS64) -o $@ $<

# C 文件编译: MinGW GCC → 汇编 → 清理 → x86_64-elf-as
%.o: %.c
	$(CC) -S $(CFLAGS) -o $@.tmp.s $<
	@powershell -File clean_asm.ps1 $@.tmp.s
	$(AS) $(ASFLAGS64) -o $@ $@.tmp.s
	@rm -f $@.tmp.s

kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# 生成 ISO 镜像
iso: kernel.bin
	@mkdir -p iso/boot
	cp kernel.bin iso/boot/
	grub-mkrescue -o axion.iso iso/ 2>NUL || \
	powershell -Command "Write-Host 'ISO creation requires grub-mkrescue (install GRUB2 or use WSL)'"

# QEMU 运行 (Multiboot 内核直接加载)
run: kernel.bin
	$(QEMU) -kernel kernel.bin -m 256M -nographic -no-reboot

run-gui: kernel.bin
	$(QEMU) -kernel kernel.bin -m 256M

# 调试模式
debug: kernel.bin
	$(QEMU) -kernel kernel.bin -m 256M -nographic -no-reboot -s -S

clean:
	rm -f src/boot/*.o src/kernel/*.o
	rm -f src/kernel/*.tmp.s
	rm -f kernel.bin axion.iso
	rm -rf iso/boot