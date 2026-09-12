#include "hotness.h"

/* 16 个页的热度 (4-bit) 打包进一个 uint64_t */
#define HOTNESS_WORDS (NUM_PAGE_GROUPS / 16)
static uint64_t hotness[HOTNESS_WORDS];

void hotness_init(void) {
    for (int i = 0; i < HOTNESS_WORDS; i++) {
        hotness[i] = 0;
    }
}

/* W6.5: 以下两个函数当前未被内核调用（boomerang.c 只读 hotness_get），
 * 用条件编译包裹以便后续启用，避免死代码进入二进制。 */
#if 0
void hotness_update(uint32_t group_idx) {
    int word = group_idx / 16;
    int shift = (group_idx % 16) * 4;
    uint64_t val = (hotness[word] >> shift) & 0xF;
    if (val < 15) val++;
    hotness[word] = (hotness[word] & ~(0xFULL << shift)) | (val << shift);
}
#endif

uint8_t hotness_get(uint32_t group_idx) {
    int word = group_idx / 16;
    int shift = (group_idx % 16) * 4;
    return (uint8_t)((hotness[word] >> shift) & 0xF);
}

#if 0
void hotness_reset(uint32_t group_idx) {
    int word = group_idx / 16;
    int shift = (group_idx % 16) * 4;
    hotness[word] &= ~(0xFULL << shift);
}
#endif

void hotness_age(void) {
    for (int i = 0; i < HOTNESS_WORDS; i++) {
        hotness[i] = (hotness[i] >> 1) & 0x7777777777777777ULL;
    }
}