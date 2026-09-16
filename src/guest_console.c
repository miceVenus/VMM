#include "guest_console.h"

#include <stdint.h>

static const uint16_t console_port = 0xE9;

static void outb(uint16_t port, uint8_t value) {
    asm("outb %0,%1" : : "a" (value), "Nd" (port) : "memory");
}

void putc(int c) {
    outb(console_port, (uint8_t)c);
}

void puts(const char* s) {
    for (const char* p = s; *p; ++p) {
        putc(*p);
    }
}
