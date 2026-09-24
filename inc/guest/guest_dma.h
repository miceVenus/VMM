#pragma once

#include <stddef.h>

/*
 * Small identity-mapped DMA arena for the current freestanding Guest.
 * Returned pointers are also the device-visible Guest physical addresses.
 */
void* guest_dma_alloc(size_t size, size_t alignment);
void guest_dma_free(void* address);

