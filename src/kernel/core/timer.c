#include "timer.h"

static uint64_t tsc_start = 0;
static uint64_t tsc_per_ms = 0;

/* Load sampling for power policy */
static uint64_t load_active_ticks = 0;
static uint64_t load_total_ticks = 0;
static uint64_t load_last_reset_ms = 0;

void timer_init(void) {
    tsc_start = rdtsc();
    /* 假定 CPU 频率约 2GHz, TSC 每 tick 约 0.5ns */
    /* 1ms = 2,000,000 ticks */
    tsc_per_ms = 2000000;
}

uint64_t timer_ms(void) {
    uint64_t now = rdtsc();
    return (now - tsc_start) / tsc_per_ms;
}

uint64_t timer_us(void) {
    uint64_t now = rdtsc();
    return (now - tsc_start) / (tsc_per_ms / 1000);
}

void timer_mdelay(uint64_t ms) {
    uint64_t target = timer_ms() + ms;
    while (timer_ms() < target) {
        __asm__ volatile("pause");
    }
}

void load_sample(uint64_t* active_ticks, uint64_t* total_ticks) {
    *active_ticks = load_active_ticks;
    *total_ticks = load_total_ticks;
}

void load_tick(int has_task) {
    load_total_ticks++;
    if (has_task) load_active_ticks++;
    
    /* Reset every 100ms */
    uint64_t now_ms = timer_ms();
    if (now_ms - load_last_reset_ms >= 100) {
        load_active_ticks = 0;
        load_total_ticks = 0;
        load_last_reset_ms = now_ms;
    }
}