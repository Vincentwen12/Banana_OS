#include "gdt.h"
#include "vm.h"

/*
 * gdt.c — Ring 3 GDT setup and user-mode entry (W6).
 *
 * The boot GDT (boot.S) only described kernel code/data.  We install a full
 * 64-bit GDT here that adds Ring 3 user data/code segments so that:
 *   - switch_to_user() can iretq into Ring 3,
 *   - a Ring 3 `syscall` can return via sysretq (IA32_STAR set in syscall_init).
 *
 * Descriptor layout (base=0, limit=0, 64-bit):
 *   0x00 null, 0x08 kernel code, 0x10 kernel data,
 *   0x18 user data (DPL3) -> selector 0x1B,
 *   0x20 user code (DPL3) -> selector 0x23,
 *   0x28 TSS64 (RSP0 = kernel exception stack for user-mode faults).
 */
static uint64_t gdt[7];

/* Minimal 64-bit TSS (104 bytes). Only rsp0 is used: a user-mode fault
 * (handled by the IDT installed in idt.c) switches to this kernel stack. */
typedef struct {
    uint32_t rsvd0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t rsvd1;
    uint64_t ist[7];
    uint64_t rsvd2;
    uint16_t rsvd3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

static tss64_t* tss;

/* TSS + user-fault exception stack live in BSS (not the pmalloc hot zone):
 * the TSS.RSP0 stack must never be reallocated to a user page-table page by
 * a later vm_create()/elf_load(), or a user-mode fault would push its frame
 * over the pml4 and triple-fault. */
static uint8_t tss_storage[4096] __attribute__((aligned(16)));
static uint8_t tss_fault_stack[4096] __attribute__((aligned(16)));

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

static gdtr_t gdtr;

void gdt_init(void)
{
    gdt[0] = 0x0000000000000000ULL;  /* null */
    gdt[1] = 0x00209A0000000000ULL;  /* kernel code (DPL0), selector 0x08 */
    gdt[2] = 0x0000920000000000ULL;  /* kernel data (DPL0), selector 0x10 */
    gdt[3] = 0x0000F20000000000ULL;  /* user data   (DPL3), selector 0x18 */
    gdt[4] = 0x0020FA0000000000ULL;  /* user code   (DPL3), selector 0x20 */

    /* TSS64 at 0x28, type 0x89 (available). Allocated from BSS (see above);
     * RSP0 = top of a dedicated 4KB stack for user-mode fault handlers. */
    tss = (tss64_t*)tss_storage;
    tss->rsp0 = (uint64_t)(uintptr_t)(tss_fault_stack + sizeof(tss_fault_stack));
    {
        uint64_t base = (uint64_t)(uintptr_t)tss;
        uint64_t limit = (uint64_t)(sizeof(tss64_t) - 1);
        gdt[5] = (limit & 0xFFFF)
               | ((base & 0xFFFFFFULL) << 16)
               | (0x89ULL << 40)
               | (((limit >> 16) & 0xF) << 48)
               | (((base >> 24) & 0xFF) << 56);
        gdt[6] = (base >> 32);
    }

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt[0];

    __asm__ volatile("lgdt (%0)" :: "r"(&gdtr) : "memory");

    /* Reload CS with a far return to the kernel code selector. */
    __asm__ volatile(
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : : "rax", "memory");

    /* Reload the data segment registers. */
    __asm__ volatile(
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        : : : "ax", "memory");

    /* Load TR so user-mode exceptions have a kernel stack to switch to. */
    if (tss)
        __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28) : "memory");
}

/*
 * user_run — drop into Ring 3 at `entry` with `user_rsp` and address space
 * `vmctx`.  Only returns when the program exits (via the exit syscall, which
 * eventually unwinds back to the kernel dispatch loop).
 */
void user_run(uint64_t entry, uint64_t user_rsp, void* vmctx)
{
    vm_context_t* ctx = (vm_context_t*)vmctx;
    if (ctx)
        vm_switch(ctx);
    switch_to_user(entry, user_rsp);
}