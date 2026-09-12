#ifndef HOTNESS_H
#define HOTNESS_H

#include "axion.h"

void hotness_init(void);
void hotness_update(uint32_t group_idx);   /* group = page_idx / 64 */
void hotness_age(void);
uint8_t hotness_get(uint32_t group_idx);
void    hotness_reset(uint32_t group_idx);

#endif