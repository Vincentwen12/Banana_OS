#ifndef TIMER_H
#define TIMER_H

#include "axion.h"

void timer_init(void);
uint64_t timer_ms(void);
uint64_t timer_us(void);
void timer_mdelay(uint64_t ms);
void load_sample(uint64_t* active_ticks, uint64_t* total_ticks);
void load_tick(int has_task);

#endif