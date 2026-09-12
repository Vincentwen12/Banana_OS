#include "mm.h"
#include "vga.h"
#include "printk.h"

/* 三个区域的位图数组 */
static uint64_t hot_bitmap[NUM_PAGE_GROUPS];                 /* 4096 组 */
static uint64_t warm_bitmap[WARM_ZONE_PAGES / 64];           /* 2048 组 */
static uint64_t boomerang_bitmap[BOOMERANG_PAGES / 64];      /* 1024 组 */

/* 三个区域实例 */
static zone_t hot_zone = {
    .base_addr        = HEAP_BASE,
    .total_pages      = HOT_ZONE_PAGES,
    .used_pages       = 0,
    .bitmap           = hot_bitmap,
    .bitmap_groups    = NUM_PAGE_GROUPS,
    .compressed_equiv = 0,
    .evict_count      = 0
};

static zone_t warm_zone = {
    .base_addr        = WARM_BASE,
    .total_pages      = WARM_ZONE_PAGES,
    .used_pages       = 0,
    .bitmap           = warm_bitmap,
    .bitmap_groups    = WARM_ZONE_PAGES / 64,
    .compressed_equiv = 0,
    .evict_count      = 0
};

static zone_t boomerang_zone = {
    .base_addr        = BOOMERANG_BASE,
    .total_pages      = BOOMERANG_PAGES,
    .used_pages       = 0,
    .bitmap           = boomerang_bitmap,
    .bitmap_groups    = BOOMERANG_PAGES / 64,
    .compressed_equiv = 0,
    .evict_count      = 0
};

/* 热页释放缓存（W6.5 优化 #3）：最多 16 个最近释放的 hot 区页。
 * pfree 入缓存、pmalloc 优先复用，减少位图扫描；条目含 valid 标志。 */
#define HOT_CACHE_SLOTS 16
typedef struct {
    uint16_t group;   /* 页所在组（hot_bitmap 下标） */
    uint16_t bit;     /* 组内位 */
    uint8_t  valid;
} hot_cache_entry_t;

static hot_cache_entry_t hot_free_cache[HOT_CACHE_SLOTS];

/* [shadow] 物理页线性访问安全检测：内核用「线性=物理」恒等访问分配页。
 * 若用户任务的 user 映射占据了某物理页的恒等线性槽（用户程序已抬到
 * 0x4000000000 之上，正常布局下不会发生），则内核写该线性地址会写到 user
 * 页而非目标物理页，破坏内核内存（run9 fork 覆写源即此）。分配候选页前
 * 检查当前 CR3 下线性地址是否解析回同一物理页；被遮蔽则跳过该物理页。
 * 这是兜底：主修复是抬升用户装载基址（见 elf/loader.c）。 */
static int paddr_linear_ok(uint64_t p)
{
    const uint64_t M = 0x000FFFFFFFFFF000ULL;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* l4 = (uint64_t*)(cr3 & M);
    int i4 = (int)((p >> 39) & 0x1FF), i3 = (int)((p >> 30) & 0x1FF);
    int i2 = (int)((p >> 21) & 0x1FF), i1 = (int)((p >> 12) & 0x1FF);
    if (!(l4[i4] & 1)) return 0;
    uint64_t* l3 = (uint64_t*)(l4[i4] & M);
    if (!(l3[i3] & 1)) return 0;
    if (l3[i3] & 0x80) return ((l3[i3] & M) + (p & 0x3FFFFFFFULL)) == p;
    uint64_t* l2 = (uint64_t*)(l3[i3] & M);
    if (!(l2[i2] & 1)) return 0;
    if (l2[i2] & 0x80) return ((l2[i2] & M) + (p & 0x1FFFFFULL)) == p;
    uint64_t* l1 = (uint64_t*)(l2[i2] & M);
    if (!(l1[i1] & 1)) return 0;
    return (l1[i1] & M) == p;
}

void mm_init(void) {
    int i;

    /* 清零热区位图 */
    for (i = 0; i < NUM_PAGE_GROUPS; i++) {
        hot_bitmap[i] = 0;
    }
    /* 清零温区位图 */
    for (i = 0; i < WARM_ZONE_PAGES / 64; i++) {
        warm_bitmap[i] = 0;
    }
    /* 清零回旋镖位图 */
    for (i = 0; i < BOOMERANG_PAGES / 64; i++) {
        boomerang_bitmap[i] = 0;
    }

    hot_zone.used_pages = 0;
    warm_zone.used_pages = 0;
    boomerang_zone.used_pages = 0;
    hot_zone.compressed_equiv = 0;
    warm_zone.compressed_equiv = 0;
    boomerang_zone.compressed_equiv = 0;
    hot_zone.evict_count = 0;
    warm_zone.evict_count = 0;
    boomerang_zone.evict_count = 0;

    /* 清空热页缓存（重启后位图已清零，缓存条目必须同步失效） */
    for (i = 0; i < HOT_CACHE_SLOTS; i++)
        hot_free_cache[i].valid = 0;
}

/* W7: 分配 pages 个连续物理页（热区）。用于每任务内核栈等需要连续地址
 * 的内核结构。返回起始地址，失败返回 NULL。不经过热页缓存。 */
void* pmalloc_contig(int pages)
{
    if (pages <= 1) return pmalloc();
    for (uint64_t base = 0; base + (uint64_t)pages <= HOT_ZONE_PAGES; base += 64) {
        int ok = 1;
        for (int i = 0; i < pages; i++) {
            uint64_t idx = base + (uint64_t)i;
            if (hot_bitmap[idx / 64] & (1ULL << (idx % 64))) { ok = 0; break; }
            /* [shadow] 连续区内任一页线性被遮蔽则整段不可用。 */
            uint64_t pa = (uint64_t)HEAP_BASE + idx * PAGE_SIZE;
            if (!paddr_linear_ok(pa)) { ok = 0; break; }
        }
        if (!ok) continue;
        for (int i = 0; i < pages; i++) {
            uint64_t idx = base + (uint64_t)i;
            hot_bitmap[idx / 64] |= (1ULL << (idx % 64));
            /* 该页可能正缓存在 hot_free_cache（pfree 后未消费）：必须作废
             * 对应条目，否则后续 pmalloc() 会把它再次返回 → 同一物理页被
             * 分配两次 → 用户堆互相覆盖（glibc: corrupted size vs. prev_size）。 */
            for (int ci = 0; ci < HOT_CACHE_SLOTS; ci++) {
                if (hot_free_cache[ci].valid &&
                    hot_free_cache[ci].group == (uint16_t)(idx / 64) &&
                    hot_free_cache[ci].bit == (uint16_t)(idx % 64))
                    hot_free_cache[ci].valid = 0;
            }
        }
        hot_zone.used_pages += (uint64_t)pages;
        return (void*)((uint64_t)HEAP_BASE + base * PAGE_SIZE);
    }
    return NULL;
}

/* W7: 释放 pmalloc_contig 分配的连续页。 */
void pfree_contig(void* addr, int pages)
{
    for (int i = 0; i < pages; i++)
        pfree((void*)((uint64_t)addr + (uint64_t)i * PAGE_SIZE));
}

void* pmalloc(void) {
    int g, bit;

    /* 热页缓存快速路径：最近释放的页优先复用，避免每次扫描位图。
     * 缓存存 hot 区页号（group, bit）；pfree 时入缓存、pmalloc 命中时
     * 置位并出缓存，位图与缓存保持一致（pfree 已清位图）。 */
    for (int ci = 0; ci < HOT_CACHE_SLOTS; ci++) {
        if (hot_free_cache[ci].valid) {
            g = hot_free_cache[ci].group;
            bit = hot_free_cache[ci].bit;
            uint64_t pa = (uint64_t)HEAP_BASE +
                          ((uint64_t)g * 64 + (uint64_t)bit) * PAGE_SIZE;
            hot_free_cache[ci].valid = 0;
            hot_bitmap[g] |= (1ULL << bit);
            hot_zone.used_pages++;
            /* [shadow] 候选页线性被当前页表遮蔽（恒等槽被用户映射占用）：
             * 内核无法安全线性访问它 → 占用该页并继续找下一个。 */
            if (!paddr_linear_ok(pa)) continue;
            return (void*)pa;
        }
    }

    for (g = 0; g < NUM_PAGE_GROUPS; g++) {
        if (hot_bitmap[g] == ~0ULL)
            continue;
        bit = __builtin_ctzll(~hot_bitmap[g]);
        hot_bitmap[g] |= (1ULL << bit);
        hot_zone.used_pages++;
        uint64_t pa = (uint64_t)HEAP_BASE +
                      ((uint64_t)g * 64 + (uint64_t)bit) * PAGE_SIZE;
        /* [shadow] 同上：被遮蔽则跳过（页已占用，不再返回）。 */
        if (!paddr_linear_ok(pa)) continue;
        return (void*)pa;
    }

    return NULL;
}

void pfree(void* addr) {
    uint64_t offset;
    uint64_t page_idx;
    int g, bit;

    /* 判断地址属于哪个区域 */
    if ((uint64_t)addr >= BOOMERANG_BASE) {
        offset = (uint64_t)addr - BOOMERANG_BASE;
        page_idx = offset / PAGE_SIZE;
        g = page_idx / 64;
        bit = page_idx % 64;
        if (boomerang_bitmap[g] & (1ULL << bit)) {
            boomerang_bitmap[g] &= ~(1ULL << bit);
            boomerang_zone.used_pages--;
        }
    } else if ((uint64_t)addr >= WARM_BASE) {
        offset = (uint64_t)addr - WARM_BASE;
        page_idx = offset / PAGE_SIZE;
        g = page_idx / 64;
        bit = page_idx % 64;
        if (warm_bitmap[g] & (1ULL << bit)) {
            warm_bitmap[g] &= ~(1ULL << bit);
            warm_zone.used_pages--;
        }
    } else {
        offset = (uint64_t)addr - HEAP_BASE;
        page_idx = offset / PAGE_SIZE;
        g = page_idx / 64;
        bit = page_idx % 64;
        if (hot_bitmap[g] & (1ULL << bit)) {
            hot_bitmap[g] &= ~(1ULL << bit);
            hot_zone.used_pages--;
            /* 释放的 hot 页入缓存：找第一个空槽（满则丢弃） */
            for (int ci = 0; ci < HOT_CACHE_SLOTS; ci++) {
                if (!hot_free_cache[ci].valid) {
                    hot_free_cache[ci].group = (uint16_t)g;
                    hot_free_cache[ci].bit   = (uint16_t)bit;
                    hot_free_cache[ci].valid = 1;
                    break;
                }
            }
        }
    }
}

void mm_stats(uint64_t* total, uint64_t* used, uint64_t* free_pages) {
    *total = hot_zone.total_pages;
    *used = hot_zone.used_pages;
    *free_pages = hot_zone.total_pages - hot_zone.used_pages;
}

zone_t* mm_zone_stats(int zone_id) {
    if (zone_id == 0)
        return &hot_zone;
    if (zone_id == 1)
        return &warm_zone;
    if (zone_id == 2)
        return &boomerang_zone;
    return NULL;
}

void mm_stats_full(uint64_t* total_phys, uint64_t* equiv_total,
                   uint64_t* hot_used, uint64_t* warm_used, uint64_t* boom_used,
                   uint64_t* warm_equiv, uint64_t* boom_equiv) {
    *total_phys  = hot_zone.total_pages + warm_zone.total_pages + boomerang_zone.total_pages;
    *equiv_total = hot_zone.total_pages + warm_zone.compressed_equiv + boomerang_zone.compressed_equiv;
    *hot_used    = hot_zone.used_pages;
    *warm_used   = warm_zone.used_pages;
    *boom_used   = boomerang_zone.used_pages;
    *warm_equiv  = warm_zone.compressed_equiv;
    *boom_equiv  = boomerang_zone.compressed_equiv;
}

/* W5: mmap — allocate physical pages for user-space mapping.
 * Since we use identity mapping (physical = virtual) in Ring 0,
 * this simply allocates pages and returns the starting address.
 * In W6+, this will set up proper page table entries.
 */
void* mmap_user(uint64_t addr, uint64_t length)
{
    if (length == 0) return (void*)0;

    uint64_t num_pages = (length + 0xFFF) / 0x1000;
    void* first_page = (void*)0;

    for (uint64_t i = 0; i < num_pages; i++) {
        void* page = pmalloc();
        if (!page) {
            /* OOM — free already allocated pages */
            return (void*)0;
        }
        if (i == 0) first_page = page;
    }

    /* Return first page address (identity-mapped) */
    return first_page;
}

/* W5: munmap — free pages allocated by mmap_user.
 * Currently just calls pfree on the base address.
 * W6+ will track page ranges properly.
 */
int munmap_user(uint64_t addr, uint64_t length)
{
    if (addr == 0 || length == 0) return -1;

    uint64_t num_pages = (length + 0xFFF) / 0x1000;
    uint64_t page_addr = addr;

    for (uint64_t i = 0; i < num_pages; i++) {
        pfree((void*)page_addr);
        page_addr += PAGE_SIZE;
    }

    return 0;
}

/* W5: brk — adjust the program break (heap end).
 * Returns the new program break on success.
 */
static uint64_t user_brk = 0;

uint64_t brk_user(uint64_t new_brk)
{
    /* Initialize default brk if not set */
    if (user_brk == 0) {
        user_brk = 0x600000;  /* Default: 6MB */
    }

    if (new_brk == 0) {
        return user_brk;  /* Return current break */
    }

    if (new_brk > user_brk) {
        /* Extend heap: allocate pages */
        uint64_t needed = (new_brk - user_brk + 0xFFF) / 0x1000;
        for (uint64_t i = 0; i < needed; i++) {
            void* page = pmalloc();
            if (!page) return user_brk;  /* Can't extend further */
        }
    }

    user_brk = new_brk;
    return user_brk;
}