#ifndef GDT_H
#define GDT_H

#include "axion.h"

/*
 * 64-bit GDT selectors (Ring 3 support for W6).
 *
 * Layout:
 *   0x00  null
 *   0x08  kernel code (64-bit, DPL=0)
 *   0x10  kernel data (64-bit, DPL=0)
 *   0x18  user data   (64-bit, DPL=3)   -> SS (selector 0x1B = 0x18|RPL3)
 *   0x20  user code   (64-bit, DPL=3)   -> CS (selector 0x23 = 0x20|RPL3)
 *
 *   IA32_STAR = (0x10 << 48) | (0x08 << 32)
 *     syscall: CS=STAR[47:32]=0x08, SS=STAR[47:32]+8=0x10
 *     sysret : CS=STAR[63:48]+16=0x20, SS=STAR[63:48]+8=0x18
 */
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_DS   0x1B   /* user data  (RPL=3) */
#define GDT_USER_CS   0x23   /* user code  (RPL=3) */

void gdt_init(void);

/* Run entry (Ring 3) with the given user stack and address space.  This
 * function only returns when the program exits via the exit syscall. */
void user_run(uint64_t entry, uint64_t user_rsp, void* vmctx);

/* Assembly (syscall_entry.S). */
void switch_to_user(uint64_t entry, uint64_t user_rsp);
void enter_user(uint64_t entry, uint64_t user_rsp);

/*
 * Kernel context saved by switch_to_user() just before iretq into Ring 3.
 * The exit syscall path restores these and returns to the kernel caller of
 * user_run() (the saved RSP holds that return address).
 */
extern uint64_t user_ret_rsp;
extern uint64_t user_ret_rbx;
extern uint64_t user_ret_rbp;
extern uint64_t user_ret_r12;
extern uint64_t user_ret_r13;
extern uint64_t user_ret_r14;
extern uint64_t user_ret_r15;

#endif /* GDT_H */