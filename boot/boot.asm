; boot.asm - Multiboot entry point for VoidOS
; GRUB loads this in 32-bit protected mode with paging disabled,
; per the Multiboot 1 specification.

MBALIGN     equ  1<<0              ; align loaded modules on page boundaries
MEMINFO     equ  1<<1              ; provide memory map
VIDMODE     equ  1<<2              ; request a video mode (needed for UEFI:
                                    ; there is no legacy 0xB8000 VGA text
                                    ; buffer there, so we ask GRUB for a
                                    ; linear framebuffer instead)
MBFLAGS     equ  MBALIGN | MEMINFO | VIDMODE
MAGIC       equ  0x1BADB002        ; multiboot magic number
CHECKSUM    equ -(MAGIC + MBFLAGS)

section .multiboot
align 4
    dd MAGIC
    dd MBFLAGS
    dd CHECKSUM
    ; the following 5 fields are ignored unless MBALIGN's "address fields"
    ; bit (1<<16) is set in MBFLAGS - we leave them zeroed since we don't
    ; use them
    dd 0                            ; header_addr
    dd 0                            ; load_addr
    dd 0                            ; load_end_addr
    dd 0                            ; bss_end_addr
    dd 0                            ; entry_addr
    ; video mode request fields (valid because VIDMODE bit is set)
    ; VoidOS's GUI (sidebar + cards) needs real screen space, so ask for
    ; 1024x768x32 specifically instead of leaving it up to the loader -
    ; this matters when booting via a direct multiboot loader that skips
    ; grub.cfg (e.g. `qemu -kernel`). Bootloaders are free to substitute
    ; the closest mode they support if this exact one isn't available.
    dd 0                            ; mode_type: 0 = linear graphics (RGB)
    dd 1024                         ; width
    dd 768                          ; height
    dd 32                           ; depth (bits per pixel)

section .bss
align 16
stack_bottom:
    resb 16384                     ; 16 KiB stack
stack_top:

section .text
global _start
extern kernel_main
_start:
    mov esp, stack_top             ; set up the stack

    push ebx                       ; multiboot info struct pointer
    push eax                       ; multiboot magic value

    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

global cpuid_supported
; int cpuid_supported(void) -> returns 1 if CPUID instruction is available
cpuid_supported:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x200000
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    shr eax, 21
    and eax, 1
    ret

global do_cpuid
; void do_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *regs)
; regs[0]=eax regs[1]=ebx regs[2]=ecx regs[3]=edx
do_cpuid:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    mov eax, [ebp+8]      ; leaf
    mov ecx, [ebp+12]     ; subleaf
    cpuid
    mov esi, [ebp+16]     ; regs ptr
    mov [esi], eax
    mov [esi+4], ebx
    mov [esi+8], ecx
    mov [esi+12], edx
    pop esi
    pop ebx
    pop ebp
    ret
