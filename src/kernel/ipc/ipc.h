#ifndef IPC_H
#define IPC_H

#include "axion.h"

typedef struct {
    int     channel_id;
    int     task_a;         /* Endpoint A task ID */
    int     task_b;         /* Endpoint B task ID */
    int     doorbell_a;     /* Doorbell channel for A->B */
    int     doorbell_b;     /* Doorbell channel for B->A */
    void*   shm_addr;       /* Shared memory address */
    int     shm_size;       /* Shared memory size */
    int     used;
} ipc_channel_t;

void ipc_init(void);
int  ipc_create_channel(int task_a, int task_b);
int  ipc_send(int channel, const void* data, int len);
int  ipc_recv(int channel, void* buf, int len);
int  ipc_peek(int channel);

#endif