#ifndef AP_H
#define AP_H

#include "axion.h"

void     ap_init(void);
void     ap_start_all(void);
int      ap_get_core_id(void);
int      ap_online_count(void);
void     ap_core_online(int apic_id);
int      ap_get_state(int core_idx);
void     ap_set_state(int core_idx, int state);

#endif