OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS((1 << 0) | (1 << 2));  /* R-X */
    data   PT_LOAD FLAGS((1 << 1) | (1 << 2));  /* RW- */
    pvh_note PT_NOTE FLAGS(4);                    /* R-- */
}

SECTIONS
{
    /* 内核加载到 1MB 物理地址 (Multiboot 约定) */
    . = 0x100000;

    .text : {
        *(.multiboot)
        *(.text)
        *(.text.*)
    } :text

    .pvh_note : {
        *(.multiboot.pvh_note)
    } :pvh_note

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :text

    /* Syscall trampoline — W5-era Ring 0 bridge. W6 user programs (hello.elf,
     * bash) use the real `syscall` instruction and never reference this, so it
     * just needs to stay inside the kernel image. 位置跟随 .rodata 自动 4KB
     * 对齐，避免手工地址随 .rodata 增长反复重叠（0x118000→0x11a000 历史）。 */
    . = ALIGN(4096);
    .trampoline : {
        *(.trampoline)
    } :data

    .data : {
        *(.data)
        *(.data.*)
    } :data

    /* 页表段: 不可清零 */
    .pgtable : {
        *(.pgtable)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss)
        *(COMMON)
        *(.bss.*)
        __bss_end = .;
    } :data

    /* 4KB 对齐 */
    . = ALIGN(4096);
    kernel_end = .;
}