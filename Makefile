CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pie -no-pie \
         -fno-builtin -nostdlib -Wall -Wextra -Ikernel -O2
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# kernel/int64_div.c provides __udivdi3/__umoddi3/__divdi3/__moddi3/
# __udivmoddi4 ourselves (see that file), so aml.c's genuine 64-bit
# AML Divide/Mod math links with plain ld - no -lgcc/multilib needed.
KERNEL_SRCS = kernel/kernel.c kernel/vga.c kernel/ui.c kernel/apps.c kernel/fs.c kernel/disk.c kernel/cpuinfo.c kernel/smbios.c kernel/pci.c kernel/meminfo.c kernel/keyboard.c kernel/mouse.c kernel/ps2.c kernel/acpi.c kernel/aml.c kernel/battery.c kernel/int64_div.c
KERNEL_OBJS = $(KERNEL_SRCS:.c=.o)
BOOT_OBJ = boot/boot.o

BIN = iso/boot/voidos.bin
ISO = voidos.iso

all: $(ISO)

boot/boot.o: boot/boot.asm
	$(AS) $(ASFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(BOOT_OBJ) $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(ISO): $(BIN) iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ iso

clean:
	rm -f $(BOOT_OBJ) $(KERNEL_OBJS) $(BIN) $(ISO)

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO)

.PHONY: all clean run
