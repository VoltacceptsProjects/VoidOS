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

# .vapp packages live in vapps/ at the repo root (kept in sync with the
# online application directory - see vapps/README.md and
# tools/sync-vapps.sh) and get staged into iso/boot/vapps/ so
# grub-mkrescue bundles them onto the ISO. grub.cfg loads each one as a
# Multiboot module; VoidOS installs them into VoidFS at boot. This is
# how packages get onto a machine with no network stack of its own.
VAPP_SRCS = $(wildcard vapps/*.vapp)
VAPP_STAGED = $(patsubst vapps/%,iso/boot/vapps/%,$(VAPP_SRCS))

all: $(ISO)

boot/boot.o: boot/boot.asm
	$(AS) $(ASFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(BOOT_OBJ) $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

iso/boot/vapps/%.vapp: vapps/%.vapp
	mkdir -p iso/boot/vapps
	cp $< $@

$(ISO): $(BIN) iso/boot/grub/grub.cfg $(VAPP_STAGED)
	grub-mkrescue -o $@ iso

clean:
	rm -f $(BOOT_OBJ) $(KERNEL_OBJS) $(BIN) $(ISO)
	rm -rf iso/boot/vapps

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO)

.PHONY: all clean run
