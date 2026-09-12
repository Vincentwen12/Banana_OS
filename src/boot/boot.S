/* Axion-Ban 内核引导 - Multiboot2 + 64位长模式切换 */
/* 由 GRUB 加载，运行在 32 位保护模式，负责切换到 64 位长模式 */

/* PVH ELF Note (QEMU 10.x 需要, 告知入口点)
 * 必须放在 .multiboot 段中确保在第一个 LOAD segment 内 */
.pushsection .multiboot.pvh_note, "a", @note
.align 4
    .long 4                /* namesz = 4 ("Xen\0") */
    .long 4                /* descsz = 4 */
    .long 18               /* type = XEN_ELFNOTE_PHYS32_ENTRY */
    .asciz "Xen"
    .long _start           /* 32-bit 入口点 */
.popsection

.set MULTIBOOT2_MAGIC,     0xE85250D6
.set MULTIBOOT2_ARCH,      0         /* i386 */
.set MULTIBOOT2_HEADER_LEN, (mb2_end - mb2_start)
.set MULTIBOOT2_CHECKSUM,  -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LEN)

.set IA32_EFER,            0xC0000080
.set IA32_EFER_LME,        (1 << 8)

.set CR4_PAE,              (1 << 5)
.set CR0_PE,               (1 << 0)
.set CR0_PG,               (1 << 31)

.set PAGE_PRESENT,         0x001
.set PAGE_RW,              0x002
.set PAGE_HUGE,            0x080

.set STACK_SIZE,           0x4000

.section .multiboot
.align 8
mb2_start:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long MULTIBOOT2_HEADER_LEN
    .long MULTIBOOT2_CHECKSUM

    /* 结束标记 */
    .align 8
    .short 0    /* type = end */
    .short 0    /* flags */
    .long  8    /* size */
mb2_end:

.section .text
.code32
.globl _start
_start:
    cli
    cld

    /* Debug: signal that _start is reached */
    movw $0x3F8, %dx
    movb $'1', %al
    outb %al, %dx
    movb $'6', %al
    outb %al, %dx

    /* 保存 Multiboot 信息 (magic in eax, info in ebx) */
    movl %eax, boot_magic
    movl %ebx, boot_info

    /* 设置临时 32 位栈 */
    movl $stack_top, %esp

    /* 构建 4 级页表: identity map 0-2GB + LAPIC (使用 1GB 大页) */
    /* 先清零整个 PML4 */
    movl $pml4, %edi
    movl $0, %eax
    movl $1024, %ecx      /* 512 entries x 2 dwords */
    rep stosl

    /* PML4[0] → PDPT0 (使用 1GB 大页, 覆盖 0-512GB) */
    movl $pdpt0, %eax
    orl  $(PAGE_PRESENT | PAGE_RW), %eax
    movl %eax, pml4
    movl $0, pml4 + 4

    /* 清零 PDPT0 */
    movl $pdpt0, %edi
    movl $0, %eax
    movl $1024, %ecx
    rep stosl

    /* PDPT0[0] = 1GB page at physical 0GB (0-1GB identity) */
    movl $(PAGE_PRESENT | PAGE_RW | PAGE_HUGE), %eax
    movl %eax, pdpt0
    movl $0, pdpt0 + 4

    /* PDPT0[1] = 1GB page at physical 1GB (1-2GB identity) */
    movl $(0x40000000 | PAGE_PRESENT | PAGE_RW | PAGE_HUGE), %eax
    movl %eax, pdpt0 + 8
    movl $0, pdpt0 + 12

    /* PDPT0[3] = 1GB page at physical 3GB (3-4GB, includes LAPIC @ 0xFEE00000) */
    movl $(0xC0000000 | PAGE_PRESENT | PAGE_RW | PAGE_HUGE | 0x010), %eax  /* PCD */
    movl %eax, pdpt0 + 24
    movl $0, pdpt0 + 28

    /* 加载 CR3 */
    movl $pml4, %eax
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
    orl  $(CR0_PG | CR0_PE), %eax
    movl %eax, %cr0

    /* 加载 64 位 GDT 并远跳转到长模式 */
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_start

halt:
    movw $0x3F8, %dx
    addb $5, %dl
1:  inb  %dx, %al
    testb $0x20, %al
    jz   1b
    movb $'H', %al
    subb $5, %dl
    outb %al, %dx
    hlt
    jmp halt

.code64
long_mode_start:
    /* Debug: signal that long mode is reached */
    movw $0x3F8, %dx
    movb $'6', %al
    outb %al, %dx
    movb $'4', %al
    outb %al, %dx

    /* 重新加载段寄存器 */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 设置 64 位栈 */
    movq $stack_top, %rsp
    movq %rsp, %rbp

    /* 初始化 FPU / SSE
     * x86-64 长模式要求 SSE2 可用，但 CR0.TS 和 CR0.EM 可能被固件置位。
     * 不清除会导致第一条 SSE 指令触发 #NM 异常 → triple fault 重启。
     */
    movq %cr0, %rax
    andq $~((1<<2)|(1<<3)), %rax   /* 清除 CR0.EM (bit 2) 和 CR0.TS (bit 3) */
    orq  $(1<<1), %rax             /* 设置 CR0.MP (bit 1) */
    movq %rax, %cr0
    fninit                         /* 初始化 x87 FPU */
    /* 启用 SSE (CR4.OSFXSR + CR4.OSXMMEXCPT) */
    movq %cr4, %rax
    orq  $((1<<9)|(1<<10)), %rax   /* OSFXSR (bit 9), OSXMMEXCPT (bit 10) */
    movq %rax, %cr4

    /* 清零 BSS */
    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    shrq $3, %rcx          /* 按 8 字节对齐清零 */
    xorq %rax, %rax
    rep stosq

    /* 调用 C 内核入口 */
    movl boot_magic(%rip), %edi
    movq boot_info(%rip), %rsi
    call kmain

    /* kmain 返回则挂起 */
    cli
    hlt
    jmp .

/* 64 位 GDT */
.align 16
gdt64:
    .quad 0x0000000000000000    /* null */
    .quad 0x00209A0000000000    /* 64-bit code */
    .quad 0x0000920000000000    /* 64-bit data */
gdt64_end:

.align 16
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

/* 页表段 (放在 .data 段中，避免被 BSS 清零覆盖) */
.section .data
.align 4096
.globl pml4
pml4:
    .skip 4096
pdpt0:
    .skip 4096
pdpt1:
    .skip 4096
pdpt2:
    .skip 4096
pdpt3:
    .skip 4096
pd3:
    .skip 4096

/* BSS 段 */
.section .bss
.align 16
stack_bottom:
    .skip STACK_SIZE
stack_top:

.align 8
boot_magic:
    .skip 4
boot_info:
    .skip 8