#include "axion.h"

/* W7 (Task 4.8): ≥64B 走 `rep movsb`/`rep stosb`（QEMU TCG 对 rep 有块
 * 复制优化）；小块保留普通循环避免 rep 固定开销。 */
void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    if (n >= 64) {
        __asm__ __volatile__("rep movsb"
                             : "+D"(dst), "+S"(src), "+c"(n)
                             :
                             : "memory");
        return ret;
    }
    char* d = (char*)dst;
    const char* s = (const char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    if (n >= 64) {
        char v = (char)c;
        __asm__ __volatile__("rep stosb"
                             : "+D"(dst), "+c"(n)
                             : "a"(v)
                             : "memory");
        return ret;
    }
    char* d = (char*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (char)c;
    return dst;
}

/* MinGW __chkstk_ms stub: stack probing not needed in kernel */
void __chkstk_ms(void) {
    /* Intentionally empty: kernel has no stack probing requirement */
}

void ___chkstk_ms(void) {
    /* 64-bit MinGW variant */
}