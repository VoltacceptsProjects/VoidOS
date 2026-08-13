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
KERNEL_SRCS = kernel/kernel.c kernel/vga.c kernel/ui.c kernel/apps.c kernel/fs.c kernel/disk.c kernel/cpuinfo.c kernel/smbios.c kernel/pci.c kernel/meminfo.c kernel/keyboard.c kernel/mouse.c kernel/ps2.c kernel/acpi.c kernel/aml.c kernel/battery.c kernel/int64_div.c kernel/serial.c kernel/iwlwifi.c
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

# Same idea as the .vapp staging above: the 9260 firmware blob lives at
# firmware/ in the repo (not committed if you'd rather keep it out of
# git - it's redistributable per Intel's linux-firmware license, but
# it's still a 1.5MB binary) and gets copied onto the ISO so
# iwlwifi_find_firmware_module() can find it as a Multiboot module at
# boot. See grub.cfg for the matching "module" line.
FIRMWARE_SRCS = $(wildcard firmware/*.ucode)
FIRMWARE_STAGED = $(patsubst firmware/%,iso/boot/firmware/%,$(FIRMWARE_SRCS))

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

iso/boot/firmware/%.ucode: firmware/%.ucode
	mkdir -p iso/boot/firmware
	cp $< $@

$(ISO): $(BIN) iso/boot/grub/grub.cfg $(VAPP_STAGED) $(FIRMWARE_STAGED)
	grub-mkrescue -o $@ iso

clean:
	rm -f $(BOOT_OBJ) $(KERNEL_OBJS) $(BIN) $(ISO)
	rm -rf iso/boot/vapps iso/boot/firmware

# -serial stdio mirrors COM1 to this terminal, so serial_writestring()
# output (PCI network-device scan, and every driver log after it) is
# visible without any real hardware or UART adapter.
run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio

.PHONY: all clean run
