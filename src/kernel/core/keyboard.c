#include "keyboard.h"
#include "port.h"

#define KBD_BUF_SIZE    256
#define KBD_ENTER       0x1C
#define KBD_BACKSPACE   0x0E
#define KBD_LSHIFT      0x2A
#define KBD_RSHIFT      0x36
#define KBD_LSHIFT_REL  0xAA
#define KBD_RSHIFT_REL  0xB6
#define KBD_CAPS_LOCK   0x3A

static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile int  kbd_head = 0;
static volatile int  kbd_tail = 0;
static bool shift_pressed = false;
static bool caps_lock = false;

static const char scan_to_ascii_lower[] = {
    0,   0,   '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    0,   'q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,   ' ',0
};

static const char scan_to_ascii_upper[] = {
    0,   0,   '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    0,   'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,   ' ',0
};

static int buf_put(char c) {
    int next = (kbd_tail + 1) % KBD_BUF_SIZE;
    if (next == kbd_head) return 0;
    kbd_buf[kbd_tail] = c;
    kbd_tail = next;
    return 1;
}

static int buf_get(char* c) {
    if (kbd_head == kbd_tail) return 0;
    *c = kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_BUF_SIZE;
    return 1;
}

static int buf_is_empty(void) {
    return kbd_head == kbd_tail;
}

void kbd_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    shift_pressed = false;
    caps_lock = false;

    while (inb(PS2_STATUS_PORT) & 0x01) inb(PS2_DATA_PORT);

    outb(PS2_CMD_PORT, 0x20);
    while (!(inb(PS2_STATUS_PORT) & 0x01)) {}
    uint8_t config = inb(PS2_DATA_PORT);
    config |= 0x01;
    config &= ~0x10;
    outb(PS2_CMD_PORT, 0x60);
    while (inb(PS2_STATUS_PORT) & 0x02) {}
    outb(PS2_DATA_PORT, config);
}

static void kbd_poll(void) {
    /* 串口输入: QEMU -nographic 下终端 stdin 经由 COM1 注入 */
    while (inb(SERIAL_PORT + 5) & 0x01) {
        char c = (char)inb(SERIAL_PORT);
        if (c == '\r')      c = '\n';   /* Enter 归一化为换行 */
        else if (c == 0x7F) c = '\b';   /* Backspace 归一化为退格 */
        buf_put(c);
    }

    /* PS/2 键盘 — run-gui / 真机下使用 */
    if (!(inb(PS2_STATUS_PORT) & 0x01)) return;
    uint8_t sc = inb(PS2_DATA_PORT);

    if (sc == KBD_LSHIFT_REL || sc == KBD_RSHIFT_REL) { shift_pressed = false; return; }
    if (sc & 0x80) return;
    if (sc == KBD_LSHIFT || sc == KBD_RSHIFT) { shift_pressed = true; return; }
    if (sc == KBD_CAPS_LOCK) { caps_lock = !caps_lock; return; }

    if (sc < sizeof(scan_to_ascii_lower)) {
        char c = (shift_pressed || caps_lock) ? scan_to_ascii_upper[sc] : scan_to_ascii_lower[sc];
        if (c != 0) buf_put(c);
    }
}

bool kbd_has_key(void) {
    kbd_poll();
    return !buf_is_empty();
}

char kbd_getchar(void) {
    char c;
    while (1) {
        kbd_poll();
        if (buf_get(&c)) return c;
        __asm__ volatile("pause");
    }
}

/* W7: 非阻塞取键。有键返回 1 并写 *c，无键返回 0（不轮询硬件）。 */
int kbd_try_get(char* c) {
    return buf_get(c) ? 1 : 0;
}

int kbd_readline(char* buf, int max_len) {
    int pos = 0;
    while (1) {
        char c = kbd_getchar();
        if (c == '\n' || c == '\r') { buf[pos] = '\0'; return pos; }
        if (c == '\b' || c == 0x7F) { if (pos > 0) pos--; }
        else if (c >= ' ' && c <= '~') { if (pos < max_len - 1) buf[pos++] = c; }
    }
}