#ifndef COMPRESS_H
#define COMPRESS_H

#include "axion.h"

/* 压缩页结构: XOR 差分 + 末尾零字节截断 */
typedef struct {
    void*    base_page_ptr;   /* 基准页地址 */
    uint8_t  xor_data[4096];  /* XOR 差分数据 */
    uint32_t valid_bytes;     /* 有效字节数 (压缩后实际大小) */
    uint32_t original_addr;   /* 原始页地址 (用于回迁) */
} compressed_page_t;

/* 全局压缩统计 */
typedef struct {
    uint64_t total_original;    /* 累计原始字节数 */
    uint64_t total_compressed;  /* 累计压缩后字节数 */
    uint32_t compress_count;    /* 压缩次数 */
} compress_stats_t;

void    compress_init(void);
void    compress_set_base(int task_id, void* page_addr);
int32_t compress_xor(void* src_page, void* base_page, compressed_page_t* dst);
void    decompress_xor(compressed_page_t* src, void* base_page, void* dst_page);
float   compress_ratio(void);
compress_stats_t* compress_get_stats(void);

#endif