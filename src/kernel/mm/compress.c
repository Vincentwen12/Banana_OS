#include "compress.h"

static void*      base_pages[MAX_TASKS];   /* 每个任务的基准页 */
static compress_stats_t stats;             /* 全局压缩统计 */

void compress_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        base_pages[i] = NULL;
    }
    stats.total_original   = 0;
    stats.total_compressed = 0;
    stats.compress_count   = 0;
}

void compress_set_base(int task_id, void* page_addr) {
    if (task_id >= MAX_TASKS) {
        return;
    }
    base_pages[task_id] = page_addr;
}

int32_t compress_xor(void* src_page, void* base_page, compressed_page_t* dst) {
    uint8_t* src  = (uint8_t*)src_page;
    uint8_t* base = (uint8_t*)base_page;
    uint8_t* xord = dst->xor_data;
    uint32_t last_nonzero = 0;

    for (uint32_t i = 0; i < 4096; i++) {
        xord[i] = src[i] ^ base[i];
        if (xord[i] != 0) {
            last_nonzero = i;
        }
    }

    dst->valid_bytes  = last_nonzero + 1;
    dst->base_page_ptr = base_page;
    dst->original_addr = (uint32_t)(uint64_t)src_page;

    stats.total_original   += 4096;
    stats.total_compressed += dst->valid_bytes;
    stats.compress_count++;

    return (int32_t)dst->valid_bytes;
}

void decompress_xor(compressed_page_t* src, void* base_page, void* dst_page) {
    uint8_t* xord = src->xor_data;
    uint8_t* base = (uint8_t*)base_page;
    uint8_t* dst  = (uint8_t*)dst_page;
    uint32_t valid = src->valid_bytes;

    for (uint32_t i = 0; i < valid; i++) {
        dst[i] = xord[i] ^ base[i];
    }
    for (uint32_t i = valid; i < 4096; i++) {
        dst[i] = base[i];
    }
}

float compress_ratio(void) {
    if (stats.total_compressed == 0) {
        return 0.0f;
    }
    return (float)stats.total_original / (float)stats.total_compressed;
}

compress_stats_t* compress_get_stats(void) {
    return &stats;
}