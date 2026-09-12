#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "axion.h"

void kbd_init(void);
bool kbd_has_key(void);
char kbd_getchar(void);
int  kbd_try_get(char* c);
int  kbd_readline(char* buf, int max_len);

#endif