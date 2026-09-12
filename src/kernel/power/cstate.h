#ifndef POWER_CSTATE_H
#define POWER_CSTATE_H

#include "axion.h"

/* C-state sub-states for MWAIT */
#define MWAIT_C1         0x00  /* C1 (halt) */
#define MWAIT_C1E        0x01  /* C1E (enhanced halt) */
#define MWAIT_C3         0x10  /* C3 (deep sleep) */
#define MWAIT_C6         0x20  /* C6 (deep power down) */

/* Core state strings */
#define CSTATE_ACTIVE    0
#define CSTATE_C1E       1
#define CSTATE_DEEP      2

/* Thresholds */
#define CSTATE_IDLE_MS   500    /* Enter deep C-state after 500ms idle */
#define CSTATE_LIGHT_MS  50     /* Enter light C-state after 50ms idle */

void     cstate_init(void);
void     cstate_enter_light(void);
void     cstate_enter_deep(void);
void     cstate_enter_deep_with_wakeup(void* addr);
void     cstate_set_wakeup_addr(void* addr);
int      cstate_get_state(void);
const char* cstate_state_name(int state);

#endif /* POWER_CSTATE_H */