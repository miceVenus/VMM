#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The freestanding Guest cannot rely on libc. These tiny helpers are shared
 * by the Virtio transport and the protocol stack.
 */
static inline void guest_memory_zero(void* address, size_t length) {
    uint8_t* bytes = (uint8_t*)address;
    while (length-- != 0) *bytes++ = 0;
}

static inline void guest_memory_copy(void* destination,
                                     const void* source,
                                     size_t length) {
    uint8_t* dst = (uint8_t*)destination;
    const uint8_t* src = (const uint8_t*)source;
    while (length-- != 0) *dst++ = *src++;
}

static inline void guest_memory_barrier(void) {
    asm volatile("" ::: "memory");
}
