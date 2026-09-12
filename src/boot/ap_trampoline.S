/* AP Trampoline - 从实模式切换到长模式
 * 加载到物理地址 0x8000 (AP_TRAMPOLINE_ADDR)
 * 由 BSP 通过 INIT-SIPI-SIPI 序列唤醒 AP 核心
 * 大小限制: < 4KB (单页)
 */

.set CR4_PAE,              (1 << 5)
.set CR0_PE,               (1 << 0)
.set CR0_PG,               (1 << 31)
.set IA32_EFER,            0xC0000080
.set IA32_EFER_LME,        (1 << 8)

/* 数据区偏移 (在 trampoline 页末尾) */
.set DATA_OFFSET,          0x0F00
.set DATA_PML4,            DATA_OFFSET + 0
.set DATA_ENTRY,           DATA_OFFSET + 8
.set DATA_STACK,           DATA_OFFSET + 16
.set DATA_APIC_ID,         DATA_OFFSET + 24

.section .text
.code16
.globl ap_trampoline_start
ap_trampoline_start:
    cli
    cld

    /* DS=0x800, so DS:offset = 0x8000 + offset (trampoline loaded at 0x8000) */
    movw $0x800, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* 加载 32 位保护模式 GDT */
    lgdtl ap_gdt32_ptr

    /* 启用保护模式 */
    movl %cr0, %eax
    orl  $CR0_PE, %eax
    movl %eax, %cr0

    /* 远跳转进入 32 位保护模式 */
    ljmpl $0x08, $0x8000 + (ap32 - ap_trampoline_start)

.code32
ap32:
    /* 设置 32 位段选择子 */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 从数据区加载 PML4 地址 */
    movl $0x8000 + DATA_PML4, %esi
    movl (%esi), %eax
    movl %eax, %cr3

    /* 启用 PAE */
    movl %cr4, %eax
    orl  $CR4_PAE, %eax
    movl %eax, %cr4

    /* 启用长模式 */
    movl $IA32_EFER, %ecx
    rdmsr
    orl  $IA32_EFER_LME, %eax
    wrmsr

    /* 启用分页 */
    movl %cr0, %eax
    orl  $CR0_PG, %eax
    movl %eax, %cr0

    /* 通过 ljmp 进入 64 位长模式 (使用 32-bit GDT 中的 64-bit 段选择子 0x18) */
    ljmp $0x18, $0x8000 + (ap64 - ap_trampoline_start)

.code64
ap64:
    /* 设置 64 位段选择子 */
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 从数据区加载栈指针 */
    movq $0x8000 + DATA_STACK, %rsi
    movq (%rsi), %rsp
    movq %rsp, %rbp

    /* 从数据区加载 APIC ID (作为第一个参数) */
    movq $0x8000 + DATA_APIC_ID, %rsi
    movl (%rsi), %edi

    /* 从数据区加载入口函数地址并跳转 */
    movq $0x8000 + DATA_ENTRY, %rsi
    movq (%rsi), %rax
    call *%rax

    /* 不应返回 */
    cli
    hlt
    jmp .

/* ========================================
 * 32 位保护模式 GDT (含 64-bit 段)
 * ======================================== */
.align 16
ap_gdt32:
    .quad 0x0000000000000000    /* null */
    .quad 0x00CF9A000000FFFF    /* 32-bit code: base=0, limit=4GB, DPL=0 */
    .quad 0x00CF92000000FFFF    /* 32-bit data: base=0, limit=4GB, DPL=0 */
    .quad 0x00209A0000000000    /* 64-bit code (selector 0x18) */
    .quad 0x0000920000000000    /* 64-bit data (selector 0x20) */
ap_gdt32_end:

ap_gdt32_ptr:
    .word ap_gdt32_end - ap_gdt32 - 1
    .long 0x8000 + (ap_gdt32 - ap_trampoline_start)

/* ========================================
 * 64 位长模式 GDT
 * ======================================== */
.align 16
ap_gdt64:
    .quad 0x0000000000000000    /* null */
    .quad 0x00209A0000000000    /* 64-bit code */
    .quad 0x0000920000000000    /* 64-bit data */
ap_gdt64_end:

ap_gdt64_ptr:
    .word ap_gdt64_end - ap_gdt64 - 1
    .long 0x8000 + (ap_gdt64 - ap_trampoline_start)

.globl ap_trampoline_end
ap_trampoline_end:
    nop