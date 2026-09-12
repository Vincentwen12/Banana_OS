#include "cstate.h"

/*
 * Core-local state — no locking needed since each core calls its own
 * functions.  These are in .bss, so each core has its own copy.
 *
 * MWAIT wakeup path:
 *   MONITOR arms the address range; MWAIT puts the core into low-power
 *   state.  When another core writes to the monitored cache line, the
 *   cache-coherency protocol invalidates it, the core wakes up, and
 *   execution resumes at the instruction immediately after MWAIT.
 *   No interrupt handler is needed — MWAIT returns inline.
 */
static void *wakeup_addr = (void *)0;
static int   core_state  = CSTATE_ACTIVE;

void cstate_init(void)
{
    wakeup_addr = (void *)0;
    core_state  = CSTATE_ACTIVE;
}

void cstate_enter_light(void)
{
    /* Safety: don't MONITOR address 0 (real-mode IVT) */
    if (!wakeup_addr) {
        __asm__ volatile("pause");
        return;
    }

    core_state = CSTATE_C1E;

    __asm__ volatile("monitor" : : "a"(wakeup_addr), "c"(0), "d"(0));
    __asm__ volatile("mwait"   : : "a"(MWAIT_C1E), "c"(0));

    /*
     * MWAIT returned inline — a write to the monitored cache line
     * (e.g. doorbell channel data) triggered cache-coherency wakeup.
     */
    core_state = CSTATE_ACTIVE;
}

void cstate_enter_deep(void)
{
    if (!wakeup_addr) {
        __asm__ volatile("pause");
        return;
    }

    core_state = CSTATE_DEEP;

    __asm__ volatile("monitor" : : "a"(wakeup_addr), "c"(0), "d"(0));
    __asm__ volatile("mwait"   : : "a"(MWAIT_C6), "c"(0));

    /* Woken by cache-coherency write to monitored address */
    core_state = CSTATE_ACTIVE;
}

/*
 * Convenience: set wakeup address then enter deep C-state.
 * This is the primary API for AP cores that want to sleep until
 * their doorbell channel is written to.
 */
void cstate_enter_deep_with_wakeup(void *addr)
{
    cstate_set_wakeup_addr(addr);
    cstate_enter_deep();
}

void cstate_set_wakeup_addr(void *addr)
{
    wakeup_addr = addr;
}

int cstate_get_state(void)
{
    return core_state;
}

const char *cstate_state_name(int state)
{
    switch (state) {
    case CSTATE_C1E:
        return "C1E";
    case CSTATE_DEEP:
        return "Deep C-state";
    default:
        return "Active";
    }
}