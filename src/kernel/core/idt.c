/*
 * idt.c — install a minimal 256-entry IDT so faults print a diagnostic and
 * halt instead of triple-faulting into a silent reboot.
 *
 * The IDT (4KB) is allocated from the Ω hot zone at runtime (idt_init runs
 * after mm_init) to keep the kernel binary under the 80KB budget.
 */
#include "vga.h"
#include "printk.h"
#include "mm.h"
#include "mm/vm.h"
#include "sched.h"
#include "syscall.h"
/* Defined in idt.S: 256 exception stubs packed at 8-byte stride. */
extern uint8_t thunk_base[];

typedef struct {
    uint16_t off0;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  attr;
    uint16_t off1;
    uint32_t off2;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

/* Hexadecimal printer (kernel has no printf). */
static void print_hex(uint64_t v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[20];
    int i = 0;
    if (v == 0) { vga_putc('0'); return; }
    while (v) { buf[i++] = hexd[v & 0xF]; v >>= 4; }
    while (i) vga_putc(buf[--i]);
}

/* Dump an exception frame and return (the stub halts afterwards).
 * Frame layout (see idt.S): [rax][rcx][rdx][rbx][rbp][rsi][rdi][r8..r15]
 *                            [vec][err][RIP][CS][RFLAGS][RSP][SS]. */
void exc_print_c(uint64_t* fr)
{
    static const char* names[15] = {
        "RAX","RCX","RDX","RBX","RBP","RSI","RDI",
        "R8","R9","R10","R11","R12","R13","R14","R15"
    };
    uint64_t vec = fr[15], err = fr[16], rip = fr[17], cs = fr[18], rsp = fr[20];
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    vga_puts("\n=== KERNEL EXCEPTION ===\n");
    vga_puts("vector=0x"); print_hex(vec);
    vga_puts(" err=0x");   print_hex(err);
    vga_puts("\nRIP=0x");  print_hex(rip);
    vga_puts(" CS=0x");    print_hex(cs);
    vga_puts("\nRSP=0x");  print_hex(rsp);
    vga_puts("\nCR2=0x");  print_hex(cr2);
    for (int i = 0; i < 15; i++) {
        vga_puts("\n"); vga_puts(names[i]); vga_puts("=0x");
        print_hex(fr[i]);
    }
    /* Dump the faulting kernel stack (RSP .. RSP+0x80) so the caller's
     * locals / saved registers are visible for diagnosis. */
    vga_puts("\n[stack]");
    for (int i = 0; i < 16; i++) {
        uint64_t* p = (uint64_t*)(rsp + (uint64_t)i * 8);
        vga_puts("\n0x"); print_hex((uint64_t)(uintptr_t)p);
        vga_puts("="); print_hex(p[0]);
    }
    vga_puts("\nSystem halted.\n");
}

/* User-mode fault (called from idt.S .Luser): map the exception vector to a
 * signal, kill the current task and set syscall_exit_request so the assembly
 * entry unwinds back to the kernel (run command caller or wait4 parent).
 * Signal numbers follow Linux x86-64 (SIGFPE=8, SIGILL=4, SIGSEGV=11).
 * W7 (Task 2.5): if the task has a handler for the mapped signal, deliver it
 * (sigframe + syscall_sig_request) instead of killing; idt.S iretq to the
 * handler. fault_rip/fault_rsp are the user RIP/RSP from the exception frame. */

/* Defined in signal.c: build the sigframe and arm the delivery globals.
 * Returns 1 if delivery was armed (handler present & not blocked). */
extern int signal_deliver_now(int sig, uint64_t fault_rip, uint64_t fault_rsp);
void exc_kill_user(uint64_t vec, uint64_t fault_rip, uint64_t fault_rsp)
{
    int sig = 11;  /* SIGSEGV (default) */
    if (vec == 0)        sig = 8;   /* #DE -> SIGFPE */
    else if (vec == 6)   sig = 4;   /* #UD -> SIGILL */
    else if (vec == 13 || vec == 14 || vec == 8) sig = 11; /* #GP/#PF/#DF */

    /* 记录到日志环形缓冲（dmesg 可见） */
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    printk(KERN_WARNING, "user fault vec=%u sig=%u pid=%u rip=%x cr2=%x\n",
           (unsigned)vec, (unsigned)sig,
           current_task ? (unsigned)current_task->pid : 0,
           (uint64_t)fault_rip, cr2);

    /* W7: 有 handler 且未阻塞 -> 投递信号（idt.S 检查 syscall_sig_request
     * 后覆写 iretq 帧的 RIP/RSP，把原 faulting RIP/RSP 存进 sigframe）。 */
    if (current_task && signal_deliver_now(sig, fault_rip, fault_rsp))
        return;

    if (current_task) {
        current_task->exit_code = sig;
        sched_mark_exited(current_task, sig);
    }

    /* The idt.S unwind path clears this right after restoring the kernel
     * context (same protocol as the exit syscall). */
    syscall_exit_request = 1;
}

void idt_init(void)
{
    /* Allocate the IDT page from the Ω hot zone (kernel binary stays small). */
    void* idt_mem = vm_alloc_page();
    if (!idt_mem) return;   /* OOM: keep the old (bootloader) IDT */
    idt_entry_t* idt = (idt_entry_t*)idt_mem;

    for (int i = 0; i < 256; i++) {
        /* Vectors 0-31 get real per-vector thunks; 32-255 (masked interrupts)
         * share the generic gate right after the last real thunk. */
        int ti = (i < 32) ? i : 32;
        uint64_t addr = (uint64_t)(uintptr_t)thunk_base + (uint64_t)ti * 8;
        idt[i].off0 = (uint16_t)(addr & 0xFFFF);
        idt[i].sel  = 0x08;                 /* kernel code segment */
        idt[i].ist  = 0;
        idt[i].attr = 0x8E;                 /* present, DPL0, 64-bit int gate */
        idt[i].off1 = (uint16_t)((addr >> 16) & 0xFFFF);
        idt[i].off2 = (uint32_t)(addr >> 32);
        idt[i].zero = 0;
    }

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr;
    idtr.limit = (uint16_t)(256 * 16 - 1);
    idtr.base  = (uint64_t)(uintptr_t)idt;

    __asm__ volatile("lidt %0" :: "m"(idtr) : "memory");
}
