#include "boomerang.h"
#include "mm.h"
#include "hotness.h"
#include "timer.h"
#include "vga.h"
#include "port.h"

/* 元数据存储在 zone 页面的偏移 0 处, 压缩数据紧随其后 */
#define META_OFFSET  0
#define DATA_OFFSET  sizeof(boomerang_entry_t)

/* 零页: 用作压缩基准页 */
static uint8_t zero_page[4096];

/* 简单的字节拷贝 */
static void bcopy(const void* src, void* dst, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void bzero(void* dst, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}

/* 在区域位图中查找空闲槽位, 返回槽位索引 */
static int find_free_slot(zone_t* z) {
    for (int g = 0; g < z->bitmap_groups; g++) {
        if (z->bitmap[g] == ~0ULL) continue;
        int bit = __builtin_ctzll(~z->bitmap[g]);
        return g * 64 + bit;
    }
    return -1;
}

/* 获取 zone 中指定槽位的入口指针 (元数据存储在页面开头) */
static boomerang_entry_t* get_entry(zone_t* z, int slot) {
    uint64_t addr = z->base_addr + (uint64_t)slot * PAGE_SIZE;
    return (boomerang_entry_t*)(uint64_t)addr;
}

/* 获取 zone 中指定槽位的压缩数据指针 */
static void* get_data(zone_t* z, int slot) {
    uint64_t addr = z->base_addr + (uint64_t)slot * PAGE_SIZE + DATA_OFFSET;
    return (void*)(uint64_t)addr;
}

static void update_compressed_equiv(zone_t* z) {
    float ratio = compress_ratio();
    if (ratio > 0.0f) {
        z->compressed_equiv = (uint64_t)((float)z->used_pages * ratio);
    } else {
        z->compressed_equiv = z->used_pages * 5 / 2;
    }
}

void boomerang_init(void) {
    /* zero_page 已在 BSS 中清零，无需额外初始化 */
    /* 温区和回旋镖区位图已在 mm_init() 中清零 */
}

int boomerang_evict_cold(void) {
    zone_t* hot  = mm_zone_stats(0);
    zone_t* warm = mm_zone_stats(1);
    zone_t* boom = mm_zone_stats(2);
    int evicted = 0;

    for (int i = 0; i < HOT_ZONE_PAGES; i++) {
        int g = i / 64;
        int b = i % 64;

        if (!(hot->bitmap[g] & (1ULL << b))) continue;
        if (hotness_get(g) >= 2) continue;

        int dest_slot;
        zone_t* dest_zone;

        dest_slot = find_free_slot(warm);
        if (dest_slot >= 0) {
            dest_zone = warm;
        } else {
            dest_slot = find_free_slot(boom);
            if (dest_slot >= 0) {
                dest_zone = boom;
            } else {
                break;
            }
        }

        uint64_t hot_addr = HEAP_BASE + (uint64_t)i * PAGE_SIZE;

        /* 压缩热区页, 数据写入目标页的 DATA_OFFSET 之后 */
        compressed_page_t cpage;
        compress_xor((void*)hot_addr, zero_page, &cpage);

        void* dest_data = get_data(dest_zone, dest_slot);
        int max_data = PAGE_SIZE - DATA_OFFSET;
        int copy_bytes = cpage.valid_bytes;
        if (copy_bytes > max_data) copy_bytes = max_data;
        bcopy(cpage.xor_data, dest_data, copy_bytes);

        /* 填充元数据 (存储在目标页开头) */
        boomerang_entry_t* entry = get_entry(dest_zone, dest_slot);
        entry->original_addr = (uint32_t)hot_addr;
        entry->valid_bytes   = copy_bytes;
        entry->base_page     = zero_page;
        entry->timestamp     = timer_ms();
        entry->in_boomerang  = (dest_zone == boom);
        entry->used          = true;

        /* 标记目标区域页槽已使用 */
        dest_zone->bitmap[dest_slot / 64] |= (1ULL << (dest_slot % 64));
        dest_zone->used_pages++;

        /* 释放热区页 */
        pfree((void*)hot_addr);

        update_compressed_equiv(dest_zone);
        evicted++;
    }

    return evicted;
}

void* boomerang_restore(uint64_t addr) {
    uint32_t target = (uint32_t)addr;
    int found_slot = -1;
    zone_t* src_zone = NULL;

    /* 搜索温区 */
    zone_t* warm = mm_zone_stats(1);
    for (int i = 0; i < WARM_ZONE_PAGES; i++) {
        boomerang_entry_t* e = get_entry(warm, i);
        if (e->used && e->original_addr == target) {
            found_slot = i;
            src_zone = warm;
            break;
        }
    }

    /* 搜索回旋镖区 */
    if (found_slot < 0) {
        zone_t* boom = mm_zone_stats(2);
        for (int i = 0; i < BOOMERANG_PAGES; i++) {
            boomerang_entry_t* e = get_entry(boom, i);
            if (e->used && e->original_addr == target) {
                found_slot = i;
                src_zone = boom;
                break;
            }
        }
    }

    if (found_slot < 0) return NULL;

    boomerang_entry_t* entry = get_entry(src_zone, found_slot);
    void* src_data = get_data(src_zone, found_slot);

    /* 在热区分配新页 */
    void* hot_page = pmalloc();
    if (hot_page == NULL) {
        boomerang_evict_cold();
        hot_page = pmalloc();
        if (hot_page == NULL) return NULL;
    }

    /* 重建 compressed_page_t 并解压 */
    compressed_page_t cpage;
    cpage.base_page_ptr = entry->base_page;
    cpage.valid_bytes   = entry->valid_bytes;
    cpage.original_addr = entry->original_addr;
    bcopy(src_data, cpage.xor_data, entry->valid_bytes);

    decompress_xor(&cpage, entry->base_page, hot_page);

    /* 释放源区域页槽 */
    pfree((void*)(uint64_t)(src_zone->base_addr + (uint64_t)found_slot * PAGE_SIZE));

    update_compressed_equiv(src_zone);
    entry->used = false;

    return hot_page;
}

void boomerang_stats(uint64_t* warm_entries_out, uint64_t* boom_entries_out,
                     uint64_t* warm_equiv, uint64_t* boom_equiv) {
    uint64_t w_count = 0;
    uint64_t b_count = 0;

    zone_t* warm = mm_zone_stats(1);
    zone_t* boom = mm_zone_stats(2);

    for (int i = 0; i < WARM_ZONE_PAGES; i++) {
        if (get_entry(warm, i)->used) w_count++;
    }
    for (int i = 0; i < BOOMERANG_PAGES; i++) {
        if (get_entry(boom, i)->used) b_count++;
    }

    *warm_entries_out = w_count;
    *boom_entries_out = b_count;
    *warm_equiv = w_count * 5 / 2;
    *boom_equiv = b_count * 5 / 2;
}

void boomerang_manual_compress(uint64_t addr) {
    uint64_t offset = addr - HEAP_BASE;
    uint64_t page_idx = offset / PAGE_SIZE;

    if (page_idx >= HOT_ZONE_PAGES) {
        vga_puts("Address not in hot zone\n");
        return;
    }

    zone_t* hot = mm_zone_stats(0);
    int g = (int)(page_idx / 64);
    int b = (int)(page_idx % 64);

    if (!(hot->bitmap[g] & (1ULL << b))) {
        vga_puts("Page not allocated\n");
        return;
    }

    if (hotness_get((uint32_t)g) >= 2) {
        vga_puts("Page too hot to compress\n");
        return;
    }

    zone_t* warm = mm_zone_stats(1);
    zone_t* boom = mm_zone_stats(2);
    int dest_slot;
    zone_t* dest_zone;

    dest_slot = find_free_slot(warm);
    if (dest_slot >= 0) {
        dest_zone = warm;
    } else {
        dest_slot = find_free_slot(boom);
        if (dest_slot >= 0) {
            dest_zone = boom;
        } else {
            vga_puts("No free slots in warm/boomerang zones\n");
            return;
        }
    }

    compressed_page_t cpage;
    compress_xor((void*)addr, zero_page, &cpage);

    void* dest_data = get_data(dest_zone, dest_slot);
    int max_data = PAGE_SIZE - DATA_OFFSET;
    int copy_bytes = cpage.valid_bytes;
    if (copy_bytes > max_data) copy_bytes = max_data;
    bcopy(cpage.xor_data, dest_data, copy_bytes);

    boomerang_entry_t* entry = get_entry(dest_zone, dest_slot);
    entry->original_addr = (uint32_t)addr;
    entry->valid_bytes   = copy_bytes;
    entry->base_page     = zero_page;
    entry->timestamp     = timer_ms();
    entry->in_boomerang  = (dest_zone == boom);
    entry->used          = true;

    dest_zone->bitmap[dest_slot / 64] |= (1ULL << (dest_slot % 64));
    dest_zone->used_pages++;

    pfree((void*)addr);

    update_compressed_equiv(dest_zone);

    vga_puts("Page compressed and evicted\n");
}