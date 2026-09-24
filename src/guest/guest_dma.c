#include "guest/guest_dma.h"

#include "guest/guest_memory.h"

#include <stdint.h>

#define GUEST_DMA_BASE       UINT32_C(0x00020000)
#define GUEST_DMA_SIZE       UINT32_C(0x00020000)
#define GUEST_DMA_PAGE_SIZE  UINT32_C(0x00001000)
#define GUEST_DMA_PAGE_COUNT (GUEST_DMA_SIZE / GUEST_DMA_PAGE_SIZE)

/* Positive entries mark allocation heads; -1 marks continuation pages. */
static int8_t page_allocations[GUEST_DMA_PAGE_COUNT];

void* guest_dma_alloc(size_t size, size_t alignment) {
    if (size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 ||
        alignment > GUEST_DMA_PAGE_SIZE ||
        size > GUEST_DMA_SIZE) {
        return NULL;
    }

    const size_t page_count =
        (size + GUEST_DMA_PAGE_SIZE - 1) / GUEST_DMA_PAGE_SIZE;
    if (page_count == 0 || page_count > GUEST_DMA_PAGE_COUNT) return NULL;

    for (size_t first = 0; first + page_count <= GUEST_DMA_PAGE_COUNT; ++first) {
        const uintptr_t address =
            GUEST_DMA_BASE + first * GUEST_DMA_PAGE_SIZE;
        if ((address & (alignment - 1)) != 0) continue;

        size_t page = 0;
        while (page < page_count && page_allocations[first + page] == 0) {
            ++page;
        }
        if (page != page_count) {
            continue;
        }

        page_allocations[first] = (int8_t)page_count;
        for (page = 1; page < page_count; ++page) {
            page_allocations[first + page] = -1;
        }

        void* result = (void*)address;
        guest_memory_zero(result, page_count * GUEST_DMA_PAGE_SIZE);
        return result;
    }

    return NULL;
}

void guest_dma_free(void* address) {
    const uintptr_t value = (uintptr_t)address;
    if (value < GUEST_DMA_BASE || value >= GUEST_DMA_BASE + GUEST_DMA_SIZE ||
        ((value - GUEST_DMA_BASE) % GUEST_DMA_PAGE_SIZE) != 0) {
        return;
    }

    const size_t first = (value - GUEST_DMA_BASE) / GUEST_DMA_PAGE_SIZE;
    const int8_t allocation_size = page_allocations[first];
    if (allocation_size <= 0) return;

    const size_t page_count = (size_t)allocation_size;
    guest_memory_zero(address, page_count * GUEST_DMA_PAGE_SIZE);
    for (size_t page = 0; page < page_count; ++page) {
        page_allocations[first + page] = 0;
    }
}
