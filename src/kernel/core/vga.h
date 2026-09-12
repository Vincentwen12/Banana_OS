#ifndef VGA_H
#define VGA_H

#include "axion.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_COLOR   0x0F

void vga_init(void);
void vga_putc(char c);
void vga_puts(const char* s);
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);

#endif