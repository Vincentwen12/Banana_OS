#include "policy.h"
#include "freq.h"
#include "timer.h"

static policy_t current_policy;
static uint64_t last_switch_ms;
static bool     manual_mode;
static float    last_load;

void policy_init(void)
{
    current_policy = POLICY_BALANCED;
    last_switch_ms = 0;
    manual_mode    = false;
    last_load      = 0.0f;
}

void policy_apply(float load)
{
    policy_t target;

    last_load = load;

    if (manual_mode)
        return;

    /* Determine target policy based on load */
    if (load == 0.0f)
        target = POLICY_IDLE;
    else if (load < LOAD_LIGHT_THRESHOLD)
        target = POLICY_LIGHT;
    else if (load < LOAD_BALANCED_THRESHOLD)
        target = POLICY_BALANCED;
    else
        target = POLICY_PERFORMANCE;

    /* No change needed */
    if (target == current_policy)
        return;

    /* Hysteresis: don't switch if too soon */
    if (timer_ms() - last_switch_ms < POLICY_HYST_MS)
        return;

    /* Apply the policy change */
    current_policy = target;
    last_switch_ms = timer_ms();

    switch (target) {
    case POLICY_IDLE:
        freq_drop();
        break;
    case POLICY_PERFORMANCE:
        freq_boost();
        break;
    case POLICY_LIGHT:
    case POLICY_BALANCED:
        freq_set(FREQ_DEFAULT_KHZ);
        break;
    }
}

void policy_set_manual(policy_t p)
{
    manual_mode    = true;
    current_policy = p;
    last_switch_ms = timer_ms();

    switch (p) {
    case POLICY_IDLE:
        freq_drop();
        break;
    case POLICY_PERFORMANCE:
        freq_boost();
        break;
    case POLICY_LIGHT:
    case POLICY_BALANCED:
        freq_set(FREQ_DEFAULT_KHZ);
        break;
    }
}

policy_t policy_get(void)
{
    return current_policy;
}

const char *policy_name(policy_t p)
{
    switch (p) {
    case POLICY_IDLE:
        return "IDLE";
    case POLICY_LIGHT:
        return "LIGHT";
    case POLICY_BALANCED:
        return "BALANCED";
    case POLICY_PERFORMANCE:
        return "PERFORMANCE";
    default:
        return "UNKNOWN";
    }
}

float policy_get_load(void)
{
    return last_load;
}

int policy_is_manual(void)
{
    return manual_mode ? 1 : 0;
}