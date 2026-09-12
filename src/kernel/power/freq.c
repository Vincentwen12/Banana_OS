#include "freq.h"

/* Internal state */
static uint32_t freq_current_khz = FREQ_DEFAULT_KHZ;
static uint32_t freq_min_khz     = FREQ_MIN_KHZ;
static uint32_t freq_max_khz     = FREQ_MAX_KHZ;
static bool     sim_mode         = false;

void freq_init(void) {
    uint64_t status = rdmsr(IA32_PERF_STATUS);

    /*
     * Detect simulation mode: on QEMU (no KVM), unimplemented MSRs
     * typically return 0 or all-ones.  Valid IA32_PERF_STATUS has
     * non-zero performance ratio in bits 15:0 on real hardware.
     * Also check IA32_MPERF for additional confidence.
     */
    if (status == 0 || status == 0xFFFFFFFFFFFFFFFFULL) {
        sim_mode = true;
    } else {
        uint64_t mperf = rdmsr(IA32_MPERF);
        if (mperf == 0 || mperf == 0xFFFFFFFFFFFFFFFFULL) {
            sim_mode = true;
        }
    }

    if (sim_mode) {
        freq_current_khz = FREQ_SIM_KHZ;
        freq_min_khz     = FREQ_MIN_KHZ;
        freq_max_khz     = FREQ_MAX_KHZ;
    } else {
        /*
         * On real hardware: IA32_PERF_STATUS bits 15:0 hold the
         * current performance ratio (multiplier * bus_clock).
         * For simplicity, derive frequency from the ratio assuming
         * a 100 MHz bus clock, then clamp to known bounds.
         */
        uint32_t ratio = (uint32_t)(status & 0xFFFF);
        uint32_t hw_khz = ratio * 100000;
        if (hw_khz < FREQ_MIN_KHZ || hw_khz > FREQ_MAX_KHZ) {
            hw_khz = FREQ_DEFAULT_KHZ;
        }
        freq_current_khz = hw_khz;
    }
}

void freq_set(uint32_t target_khz) {
    /* Clamp to allowed range */
    if (target_khz < FREQ_MIN_KHZ)
        target_khz = FREQ_MIN_KHZ;
    if (target_khz > FREQ_MAX_KHZ)
        target_khz = FREQ_MAX_KHZ;

    if (sim_mode) {
        freq_current_khz = target_khz;
    } else {
        /*
         * IA32_PERF_CTL bits 15:0 = target ratio (multiplier).
         * bits 31:16 = reserved, bits 63:32 = voltage ID (optional).
         * Ratio = target_khz / 100000 (assuming 100 MHz bus).
         */
        uint32_t ratio = target_khz / 100000;
        uint64_t val = (uint64_t)ratio;
        wrmsr(IA32_PERF_CTL, val);
        freq_current_khz = target_khz;
    }
}

uint32_t freq_get(void) {
    return freq_current_khz;
}

void freq_boost(void) {
    freq_set(FREQ_MAX_KHZ);
}

void freq_drop(void) {
    freq_set(FREQ_MIN_KHZ);
}