#include "vga.h"
#include "port.h"

static uint16_t* const vga_buffer = (uint16_t*)VGA_BASE;
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t vga_color = VGA_COLOR;

static void serial_putc(char c) {
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0) {}
    outb(SERIAL_PORT, c);
}

static void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x03);
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
    outb(SERIAL_PORT + 4, 0x0B);
}

void vga_init(void) {
    serial_init();
    vga_clear();
}

void vga_clear(void) {
    uint16_t blank = (uint16_t)' ' | ((uint16_t)vga_color << 8);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = blank;
    }
    cursor_x = 0;
    cursor_y = 0;
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = (bg << 4) | (fg & 0x0F);
}

static void vga_scroll(void) {
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    uint16_t blank = (uint16_t)' ' | ((uint16_t)vga_color << 8);
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;
    }
    cursor_y = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
    serial_putc(c);

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= VGA_HEIGHT) vga_scroll();
        return;
    }
    if (c == '\r') { cursor_x = 0; return; }
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga_buffer[cursor_y * VGA_WIDTH + cursor_x] =
                (uint16_t)' ' | ((uint16_t)vga_color << 8);
        }
        return;
    }
    if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7;
        if (cursor_x >= VGA_WIDTH) { cursor_x = 0; cursor_y++; }
        if (cursor_y >= VGA_HEIGHT) vga_scroll();
        return;
    }

    vga_buffer[cursor_y * VGA_WIDTH + cursor_x] =
        (uint16_t)c | ((uint16_t)vga_color << 8);
    cursor_x++;
    if (cursor_x >= VGA_WIDTH) { cursor_x = 0; cursor_y++; }
    if (cursor_y >= VGA_HEIGHT) vga_scroll();
}

void vga_puts(const char* s) {
    while (*s) vga_putc(*s++);
}