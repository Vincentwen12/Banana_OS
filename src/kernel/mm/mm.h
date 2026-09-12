#ifndef MM_H
#define MM_H

#include "axion.h"

typedef struct {
    uint64_t base_addr;         /* 区域起始物理地址 */
    uint64_t total_pages;       /* 总页数 */
    uint64_t used_pages;        /* 已用页数 */
    uint64_t* bitmap;           /* 位图指针 (指向全局数组) */
    int      bitmap_groups;     /* 位图组数 */
    uint64_t compressed_equiv;  /* 压缩等效容量 (仅温区/回旋镖池) */
    uint64_t evict_count;       /* 驱逐计数 */
} zone_t;

void     mm_init(void);
void*    pmalloc(void);
void     pfree(void* addr);
void*    pmalloc_contig(int pages);
void     pfree_contig(void* addr, int pages);
void     mm_stats(uint64_t* total, uint64_t* used, uint64_t* free_pages);
zone_t*  mm_zone_stats(int zone_id);   /* 0=hot, 1=warm, 2=boomerang */
void     mm_stats_full(uint64_t* total_phys, uint64_t* equiv_total,
                       uint64_t* hot_used, uint64_t* warm_used, uint64_t* boom_used,
                       uint64_t* warm_equiv, uint64_t* boom_equiv);

/* W5: mmap/munmap/brk for user-space memory management */
void*    mmap_user(uint64_t addr, uint64_t length);
int      munmap_user(uint64_t addr, uint64_t length);
uint64_t brk_user(uint64_t new_brk);

#endif