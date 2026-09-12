#include "doorbell.h"
#include "sched.h"

static doorbell_channel_t* doorbells;

void doorbell_init(void) {
    doorbells = (doorbell_channel_t*)DOORBELL_BASE;
    for (int i = 0; i < DOORBELL_COUNT; i++) {
        doorbells[i].data = 0;
        doorbells[i].acl_mask = 0;
        doorbells[i].owner_id = -1;
        doorbells[i].rpos = 0;
        doorbells[i].wpos = 0;
        doorbells[i].ring_count = 0;
        doorbells[i].waiting_task = -1;
        for (int j = 0; j < DOORBELL_RING_SIZE; j++) {
            doorbells[i].ring_buf[j] = 0;
        }
    }
}

void doorbell_acl_set(int channel, int task_id, int allow) {
    if (channel < 0 || channel >= DOORBELL_COUNT) return;
    if (task_id < 0 || task_id >= 64) return;
    if (allow) {
        doorbells[channel].acl_mask |= (1ULL << task_id);
    } else {
        doorbells[channel].acl_mask &= ~(1ULL << task_id);
    }
}

int doorbell_write(int channel, uint64_t msg, int sender_id) {
    if (channel < 0 || channel >= DOORBELL_COUNT) return -1;
    
    /* Check ACL */
    if (sender_id >= 0 && sender_id < 64) {
        if (!(doorbells[channel].acl_mask & (1ULL << sender_id))) {
            return -1; /* Not authorized */
        }
    }
    
    /* Write to ring buffer */
    if (doorbells[channel].ring_count >= DOORBELL_RING_SIZE) {
        return -1; /* Ring buffer full */
    }
    
    doorbells[channel].ring_buf[doorbells[channel].wpos] = msg;
    doorbells[channel].wpos = (doorbells[channel].wpos + 1) % DOORBELL_RING_SIZE;
    doorbells[channel].ring_count++;
    doorbells[channel].data = msg; /* Legacy field */
    mfence();
    
    /* Wake up waiting task */
    doorbell_notify(channel);
    
    return 0;
}

uint64_t doorbell_poll(int channel) {
    if (channel < 0 || channel >= DOORBELL_COUNT) return 0;
    if (doorbells[channel].ring_count == 0) return 0;
    
    uint64_t val = doorbells[channel].ring_buf[doorbells[channel].rpos];
    doorbells[channel].rpos = (doorbells[channel].rpos + 1) % DOORBELL_RING_SIZE;
    doorbells[channel].ring_count--;
    doorbells[channel].data = 0;
    
    return val;
}

void doorbell_notify(int channel) {
    if (channel < 0 || channel >= DOORBELL_COUNT) return;
    int tid = doorbells[channel].waiting_task;
    if (tid >= 0) {
        task_t* t = sched_get_task(tid);
        if (t && t->state == TASK_STATE_BLOCKED) {
            sched_wake(t);
        }
        doorbells[channel].waiting_task = -1;
    }
}

void doorbell_send_ipi(int core_id, int vector) {
    /* Write to LAPIC ICR (Interrupt Command Register) */
    volatile uint32_t* lapic_base = (volatile uint32_t*)(LAPIC_BASE + 0x00);
    
    /* ICR: offset 0x300 (low) and 0x310 (high) */
    volatile uint32_t* icr_low  = (volatile uint32_t*)(LAPIC_BASE + 0x300);
    volatile uint32_t* icr_high = (volatile uint32_t*)(LAPIC_BASE + 0x310);
    
    /* Set destination in high 32 bits */
    *icr_high = (uint32_t)(core_id << 24);
    
    /* Set vector and delivery mode in low 32 bits:
     * Bit 0-7: vector
     * Bit 8-10: delivery mode (000 = Fixed)
     * Bit 11: destination mode (0 = physical)
     * Bit 14: assert (1)
     * Bit 18-19: destination shorthand (00 = no shorthand) */
    *icr_low = (uint32_t)(vector | (1 << 14));
    
    /* Wait for send to complete */
    while (*icr_low & (1 << 12)) {
        __asm__ volatile("pause");
    }
    
    (void)lapic_base; /* Silence unused warning */
}