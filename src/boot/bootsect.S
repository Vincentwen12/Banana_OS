/* Axion-Ban 引导扇区 - 16位实模式 CHS 加载器 */
/* 策略: int 13h AH=02h (CHS) 循环读取内核到 0x10000，进入保护模式后拷贝到 0x100000 */
/* 容量: KERNEL_SECTORS = 1007 扇区 ≈ 503KB (SPT=63, HPC=16 上限; W6 生产级强化放宽) */

.set KERNEL_LOAD_SEG, 0x1000   /* 临时加载段地址 (0x10000)，缓冲占 0x10000-0x8FD00 */
.set KERNEL_LOAD_OFF, 0x0000   /* 临时加载偏移 */
.set KERNEL_BASE,     0x100000  /* 最终运行地址 */
.set KERNEL_ENTRY,    0x100020  /* 内核入口点 (_start) */
.set KERNEL_SECTORS,  1007      /* 内核总扇区数 (16 磁头 × 63 扇区 - 引导扇区 = 1007) */
.set KERNEL_DWORDS,   KERNEL_SECTORS * 512 / 4   /* rep movsl 拷贝双字数 */
.set DISK_NUMBER,     0x80

.code16
.globl _start16
_start16:
    cli
    cld

    /* Debug: signal boot sector reached */
    movb $'B', %al
    call chr_out
    movb $'S', %al
    call chr_out

    /* 设置段寄存器: DS=0x07C0 指向引导扇区 (标签即物理地址偏移) */
    movw $0x07C0, %ax
    movw %ax, %ds
    xorw %ax, %ax
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp

    /* 保存引导磁盘号 */
    movb %dl, disk_num
    movb $'D', %al
    call chr_out
    movb disk_num, %al
    call hex_print

    /* 重置磁盘控制器 */
    movb disk_num, %dl
    movb $0x00, %ah
    int  $0x13

    /* === CHS 加载内核: LBA1..KERNEL_SECTORS → 0x10000 起线性缓冲 (SPT=63, HPC=16) ===
     * 磁头 0 自扇区 2 起（扇区 1 为引导扇区），track_left 跟踪当前磁头剩余扇区；
     * 单次读取按 min(63, 当前磁头剩余, 64KB 窗口剩余, 总剩余) 截断，
     * 保证任何一次 int13h 调用都不跨 64KB 段边界、不跳扇区。 */
    movw $KERNEL_LOAD_SEG, %ax
    movw %ax, %es
    xorw %bx, %bx                 /* 缓冲段内偏移 */
    movw $KERNEL_SECTORS, %di     /* 剩余扇区数 */
    movw $62, track_left          /* 磁头 0: 自扇区 2 起共 62 扇区 */
    movb $0, %ch                  /* 柱面 0 */
    movb $0, %dh                  /* 磁头 0 */
    movb $2, %cl                  /* 起始扇区 2 */
    jmp  load_compute

load_compute:
    /* ax = window_left = (0x10000 - bx) >> 9
     * 0x10000 放不进 16 位立即数 → 用负号技巧 (0 - bx) mod 65536；bx==0 时为 0 → 特判整窗可用 */
    xorw %ax, %ax
    subw %bx, %ax
    shrw $9, %ax
    jnz 1f
    movw $63, %ax                 /* bx==0 → 整窗 128 扇区, 交给下面 min(63) 截断 */
1:
    /* ax = min(ax, 63) */
    cmpw $63, %ax
    jbe 2f
    movw $63, %ax
2:
    /* ax = min(ax, track_left) */
    cmpw track_left, %ax
    jbe 3f
    movw track_left, %ax
3:
    /* ax = min(ax, di) */
    cmpw %di, %ax
    jbe 4f
    movw %di, %ax
4:
    testw %ax, %ax
    jz   disk_error
    movw %ax, load_count

    movb $0x02, %ah               /* int13h AH=02h: CHS read */
    movb disk_num, %dl
    int  $0x13
    jc   disk_error

    /* Debug: progress marker per chunk */
    movb $'.', %al
    call chr_out

    /* 推进缓冲: bx += count*512；进位则 es += 0x1000 (下一个 64KB 窗口) */
    movw load_count, %ax
    shlw $9, %ax
    addw %ax, %bx
    jnc  5f
    movw %es, %ax
    addw $0x1000, %ax
    movw %ax, %es
5:
    /* 剩余扇区 -= count */
    movw load_count, %ax
    subw %ax, %di
    jz   load_done
    /* 当前磁头剩余 -= count */
    movw load_count, %ax
    subw %ax, track_left
    jnz  1f                     /* 未读完本磁头 → 同磁头继续 */

    /* 本磁头读完 → 下一磁头 (HPC=16 后转下一柱面, 安全兜底) */
    incb %dh
    movb $1, %cl
    movw $63, track_left
    cmpb $16, %dh
    jb   load_compute
    movb $0, %dh
    incb %ch
    jmp  load_compute
1:
    /* 同磁头继续（可能因 64KB 窗口截断）: 必须推进扇区号 cl += count，
     * 否则下一次 int13h 从同一扇区重读，后续数据整体错位。 */
    addb load_count, %cl
    jmp  load_compute

load_done:
    /* Debug: load complete */
    movb $'L', %al
    call chr_out

    /* 启用 A20 线 */
    inb   $0x92, %al
    orb   $0x02, %al
    outb  %al, $0x92

    /* 加载 GDT (DS=0x07C0，标签即正确偏移) */
    movw $gdt32_ptr, %si
    lgdt (%si)

    movl %cr0, %eax
    orl  $1, %eax
    movl %eax, %cr0

    /* 32-bit 远跳转切换到 32-bit 保护模式 (0x66 前缀 + JMP FAR) */
    .byte 0x66
    .byte 0xEA
    .long prot_mode + 0x7C00
    .word 0x0008

    /* 如果跳转失败会到这里 */
    hlt
    jmp .

disk_error:
    /* Debug: disk error on serial, output error code */
    movb $'E', %al
    call chr_out
    movb %ah, %al
    call hex_print
    movw $disk_err_msg, %si       /* DS=0x07C0 */
    call print_str
    hlt
    jmp .

/* 串口输出单字符: AL 经 0x3F8 */
chr_out:
    pushw %ax
    pushw %dx
    movw $0x3F8, %dx
    outb %al, %dx
    popw %dx
    popw %ax
    ret

/* 串口输出 AL 的两位十六进制 */
hex_print:
    pushw %ax
    pushw %bx
    pushw %dx
    movb %al, %bl
    shrb $4, %al
    andb $0x0F, %al
    addb $'0', %al
    cmpb $'9', %al
    jbe 1f
    addb $7, %al
1:  movw $0x3F8, %dx
    outb %al, %dx
    movb %bl, %al
    andb $0x0F, %al
    addb $'0', %al
    cmpb $'9', %al
    jbe 2f
    addb $7, %al
2:  outb %al, %dx
    popw %dx
    popw %bx
    popw %ax
    ret

/* 16位字符串打印 (VGA teletype) */
print_str:
    lodsb
    testb %al, %al
    jz    print_done
    movb  $0x0E, %ah
    movb  $0x00, %bh
    int   $0x10
    jmp   print_str
print_done:
    ret

/* 32位保护模式入口 */
.code32
prot_mode:
    /* Debug: prot_mode reached (32 位下内联串口输出, 不能 call 16 位子程序) */
    movw $0x3F8, %dx
    movb $'P', %al
    outb %al, %dx

    /* 重新加载段寄存器 */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* 设置 32 位栈 */
    movl $0x90000, %esp

    /* 拷贝内核: 0x10000 → 0x100000 (KERNEL_SECTORS 扇区 = 双字数) */
    movl $0x10000, %esi
    movl $KERNEL_BASE, %edi
    movl $KERNEL_DWORDS, %ecx
    rep movsl

    /* 跳转到内核入口 */
    movl $0, %eax
    movl $0, %ebx
    ljmp $0x08, $KERNEL_ENTRY

/* 32 位 GDT */
.align 16
gdt32:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF    /* 32位代码段: 基址0, 限长4GB */
    .quad 0x00CF92000000FFFF    /* 32位数据段: 基址0, 限长4GB */
gdt32_end:

.align 16
gdt32_ptr:
    .word gdt32_end - gdt32 - 1
    .long gdt32 + 0x7C00

/* 临时变量 - 位于引导扇区内 */
.align 4
load_count:
    .word 0           /* 本次 CHS 读取的扇区数 */
track_left:
    .word 62          /* 当前磁头剩余扇区数 */

disk_err_msg:
    .asciz "Disk error!\r\n"

disk_num:
    .byte 0

.org 510
    .word 0xAA55
