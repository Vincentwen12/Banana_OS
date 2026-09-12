#ifndef DOORBELL_H
#define DOORBELL_H

#include "axion.h"

#define DOORBELL_RING_SIZE 16

typedef struct {
    volatile uint64_t data;           /* Legacy direct data field */
    uint64_t acl_mask;                /* Bitmask: which task IDs can write */
    int      owner_id;                /* Owner task ID */
    uint64_t ring_buf[DOORBELL_RING_SIZE]; /* Ring buffer for messages */
    int      rpos;                    /* Read position */
    int      wpos;                    /* Write position */
    int      ring_count;              /* Number of pending messages */
    int      waiting_task;            /* Task ID blocked on this channel (-1 = none) */
    uint8_t  _pad[DOORBELL_CHANNEL_SIZE - 8 - 8 - 4 - 8*DOORBELL_RING_SIZE - 4 - 4 - 4 - 4];
} __attribute__((aligned(64))) doorbell_channel_t;

void     doorbell_init(void);
void     doorbell_acl_set(int channel, int task_id, int allow);
int      doorbell_write(int channel, uint64_t msg, int sender_id);
uint64_t doorbell_poll(int channel);
void     doorbell_notify(int channel);
void     doorbell_send_ipi(int core_id, int vector);

#endif