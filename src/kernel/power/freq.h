#ifndef POWER_FREQ_H
#define POWER_FREQ_H

#include "axion.h"

/* Intel MSR for frequency control */
#define IA32_PERF_STATUS   0x198  /* Read: current performance state */
#define IA32_PERF_CTL      0x199  /* Write: target performance state */
#define IA32_PLATFORM_INFO 0xCE   /* Platform info: max/min ratios */
#define IA32_APERF         0xE8   /* Actual performance counter */
#define IA32_MPERF         0xE7   /* Max performance counter */

/* Default frequency ranges (kHz) */
#define FREQ_MIN_KHZ       800000   /* 800 MHz */
#define FREQ_MAX_KHZ       3000000  /* 3.0 GHz */
#define FREQ_DEFAULT_KHZ   2000000  /* 2.0 GHz */

/* Simulated frequency when MSR unavailable (QEMU) */
#define FREQ_SIM_KHZ       2000000

void  freq_init(void);
void  freq_set(uint32_t target_khz);
uint32_t freq_get(void);
void  freq_boost(void);
void  freq_drop(void);

#endif /* POWER_FREQ_H */