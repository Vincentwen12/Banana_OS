#include "ipc.h"
#include "doorbell.h"
#include "sched.h"

/* Simple memory copy */
static void mem_copy(void* dst, const void* src, int len) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < len; i++) d[i] = s[i];
}

static ipc_channel_t channels[IPC_MAX_CHANNELS];
static int next_doorbell = 0;  /* Rotating doorbell channel allocator */

void ipc_init(void) {
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        channels[i].channel_id = i;
        channels[i].task_a = -1;
        channels[i].task_b = -1;
        channels[i].doorbell_a = -1;
        channels[i].doorbell_b = -1;
        channels[i].shm_addr = NULL;
        channels[i].shm_size = 0;
        channels[i].used = 0;
    }
    next_doorbell = 0;
}

int ipc_create_channel(int task_a, int task_b) {
    /* Find free channel */
    int ch = -1;
    for (int i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (!channels[i].used) {
            ch = i;
            break;
        }
    }
    if (ch < 0) return -1;
    
    /* Allocate doorbell channels */
    int db_a = next_doorbell++;
    int db_b = next_doorbell++;
    if (db_a >= DOORBELL_COUNT || db_b >= DOORBELL_COUNT) {
        next_doorbell -= 2;
        return -1;
    }
    
    /* Set ACL: task_a can write to db_a, task_b can write to db_b */
    doorbell_acl_set(db_a, task_a, 1);
    doorbell_acl_set(db_b, task_b, 1);
    doorbell_acl_set(db_a, task_b, 0);
    doorbell_acl_set(db_b, task_a, 0);
    
    /* Allocate shared memory page (simplified: use a static offset) */
    void* shm = (void*)(IPC_SHM_BASE + ch * 0x1000);
    
    channels[ch].channel_id = ch;
    channels[ch].task_a = task_a;
    channels[ch].task_b = task_b;
    channels[ch].doorbell_a = db_a;
    channels[ch].doorbell_b = db_b;
    channels[ch].shm_addr = shm;
    channels[ch].shm_size = 0x1000; /* 4KB per channel */
    channels[ch].used = 1;
    
    return ch;
}

int ipc_send(int channel, const void* data, int len) {
    if (channel < 0 || channel >= IPC_MAX_CHANNELS) return -1;
    if (!channels[channel].used) return -1;
    if (len > channels[channel].shm_size) return -1;
    
    /* Copy data to shared memory */
    mem_copy(channels[channel].shm_addr, data, len);
    
    /* Send doorbell notification (use doorbell_a as default) */
    doorbell_write(channels[channel].doorbell_a, (uint64_t)len, channels[channel].task_a);
    
    return len;
}

int ipc_recv(int channel, void* buf, int len) {
    if (channel < 0 || channel >= IPC_MAX_CHANNELS) return -1;
    if (!channels[channel].used) return -1;
    
    /* Poll doorbell */
    uint64_t msg_len = doorbell_poll(channels[channel].doorbell_a);
    if (msg_len == 0) return 0; /* No message */
    
    int copy_len = (int)msg_len;
    if (copy_len > len) copy_len = len;
    if (copy_len > channels[channel].shm_size) copy_len = channels[channel].shm_size;
    
    /* Copy from shared memory */
    mem_copy(buf, channels[channel].shm_addr, copy_len);
    
    return copy_len;
}

int ipc_peek(int channel) {
    if (channel < 0 || channel >= IPC_MAX_CHANNELS) return -1;
    if (!channels[channel].used) return -1;
    /* Check if doorbell has pending data */
    /* We can't easily peek without consuming, but for simplicity return 0 */
    return 0;
}