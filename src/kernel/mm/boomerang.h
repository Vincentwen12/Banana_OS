#ifndef BOOMERANG_H
#define BOOMERANG_H

#include "axion.h"
#include "compress.h"

/* 回旋镖池条目元数据 */
typedef struct {
    uint32_t original_addr;   /* 原始热区页地址 (低32位) */
    uint32_t valid_bytes;     /* 压缩后有效字节数 */
    void*    base_page;       /* 基准页指针 */
    uint64_t timestamp;       /* 驱逐时间戳 */
    bool     in_boomerang;    /* true=在回旋镖池, false=在温区 */
    bool     used;            /* 条目是否在用 */
} boomerang_entry_t;

void    boomerang_init(void);
int     boomerang_evict_cold(void);     /* 返回驱逐页数 */
void*   boomerang_restore(uint64_t addr); /* 从温区/回旋镖池恢复到热区 */
void    boomerang_stats(uint64_t* warm_entries, uint64_t* boom_entries,
                        uint64_t* warm_equiv, uint64_t* boom_equiv);
void    boomerang_manual_compress(uint64_t addr); /* 手动压缩指定地址页 */

#endif