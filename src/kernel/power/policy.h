#ifndef POWER_POLICY_H
#define POWER_POLICY_H

#include "axion.h"

/* Policy levels */
typedef enum {
    POLICY_IDLE        = 0,
    POLICY_LIGHT       = 1,
    POLICY_BALANCED    = 2,
    POLICY_PERFORMANCE = 3
} policy_t;

/* Policy engine constants */
#define POLICY_SAMPLE_MS    100    /* Load sample window (ms) */
#define POLICY_HYST_MS      100    /* Hysteresis: keep policy at least 100ms */

/* Load thresholds */
#define LOAD_IDLE_THRESHOLD    0.0f
#define LOAD_LIGHT_THRESHOLD   0.2f
#define LOAD_BALANCED_THRESHOLD 0.7f

void     policy_init(void);
void     policy_apply(float load);
void     policy_set_manual(policy_t p);
policy_t policy_get(void);
const char* policy_name(policy_t p);
float    policy_get_load(void);
int      policy_is_manual(void);

#endif /* POWER_POLICY_H */