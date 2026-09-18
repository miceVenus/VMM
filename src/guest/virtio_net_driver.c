#include "guest/virtio_net_driver.h"

#include "guest/guest_memory.h"
#include "virtio_mmio.h"

extern void virtio_net_irq_stub(void);

/*
 * The Guest has no allocator, so the transport uses a fixed layout in Guest
 * RAM. These addresses are part of the educational device ABI.
 */
/*
 * Specification reference: OASIS Virtio 1.2 §2.7 (split virtqueue) requires
 * the queue size to be a power of two; 8 is this project's selected RX/TX
 * queue size, not the only size allowed by the specification.
 */
/* Project convention: number of descriptors/ring elements in each RX/TX virtqueue. */
#define DRIVER_QUEUE_SIZE          8
/* Project-internal alias: total capacity of one Guest frame buffer. */
#define DRIVER_BUFFER_SIZE         VIRTIO_NET_DRIVER_BUFFER_SIZE
/* Project-internal alias: Virtio-net metadata space reserved before each frame. */
#define DRIVER_VIRTIO_HEADER_SIZE  VIRTIO_NET_DRIVER_HEADER_SIZE

/* Project convention: start of the RX split-virtqueue region in Guest physical memory. */
#define DRIVER_RX_QUEUE_GPA        UINT64_C(0x20000)
/* Project convention: start of the TX split-virtqueue region in Guest physical memory. */
#define DRIVER_TX_QUEUE_GPA        UINT64_C(0x21000)
/* Project convention: start of the frame-buffer array referenced by RX descriptors. */
#define DRIVER_RX_BUFFER_GPA       UINT64_C(0x22000)
/* Project convention: start of the TX frame buffer, after the RX buffer array. */
#define DRIVER_TX_BUFFER_GPA       UINT64_C(0x26000)

/* Project convention: Descriptor Table offset within one reserved queue region. */
#define DRIVER_DESC_OFFSET         UINT64_C(0x000)
/* Project convention: Available Ring (Virtio Driver Area) offset. */
#define DRIVER_AVAIL_OFFSET        UINT64_C(0x100)
/* Project convention: Used Ring (Virtio Device Area) offset. */
#define DRIVER_USED_OFFSET         UINT64_C(0x200)
/* Project convention: Guest RAM region reserved and cleared for each queue. */
#define DRIVER_QUEUE_REGION_SIZE   UINT64_C(0x1000)

struct driver_virtq_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct driver_virtq_available {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[DRIVER_QUEUE_SIZE];
};

struct driver_virtq_used_element {
    uint32_t id;
    uint32_t length;
};

struct driver_virtq_used {
    uint16_t flags;
    uint16_t index;
    struct driver_virtq_used_element ring[DRIVER_QUEUE_SIZE];
};

struct driver_state {
    uint8_t initialized;
    uint8_t tx_busy;
    uint16_t rx_used_index;
    uint16_t tx_used_index;
};

struct driver_idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct driver_descriptor_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct driver_idt_entry driver_idt[256] __attribute__((aligned(16)));
static uint64_t driver_gdt[3] __attribute__((aligned(8)));
static struct driver_state state;

/* Set by the assembly ISR and consumed by virtio_net_driver_wait/poll. */
volatile uint8_t virtio_net_interrupt_pending = 0;

static void install_guest_gdt(void) {
    driver_gdt[0] = 0;
    driver_gdt[1] = UINT64_C(0x00af9b000000ffff);
    driver_gdt[2] = UINT64_C(0x00cf93000000ffff);

    const struct driver_descriptor_pointer pointer = {
        (uint16_t)(sizeof(driver_gdt) - 1),
        (uint64_t)(uintptr_t)driver_gdt,
    };
    asm volatile("lgdt %0" : : "m"(pointer) : "memory");
}

static void install_guest_idt(void) {
    guest_memory_zero(driver_idt, sizeof(driver_idt));

    const uint64_t handler = (uint64_t)(uintptr_t)&virtio_net_irq_stub;
    struct driver_idt_entry* entry =
        &driver_idt[VIRTIO_NET_INTERRUPT_VECTOR];
    entry->offset_low = (uint16_t)handler;
    entry->selector = UINT16_C(0x0008);
    entry->ist = 0;
    entry->type_attributes = UINT8_C(0x8e);
    entry->offset_middle = (uint16_t)(handler >> 16);
    entry->offset_high = (uint32_t)(handler >> 32);
    entry->reserved = 0;

    const struct driver_descriptor_pointer pointer = {
        (uint16_t)(sizeof(driver_idt) - 1),
        (uint64_t)(uintptr_t)driver_idt,
    };
    asm volatile("lidt %0" : : "m"(pointer) : "memory");
}

static void clear_interrupt_pending(void) {
    asm volatile("cli" ::: "memory");
    virtio_net_interrupt_pending = 0;
    asm volatile("sti" ::: "memory");
}

static volatile uint32_t* mmio32(uint32_t offset) {
    return (volatile uint32_t*)(uintptr_t)(VIRTIO_MMIO_BASE_GPA + offset);
}

static volatile uint8_t* mmio8(uint32_t offset) {
    return (volatile uint8_t*)(uintptr_t)(VIRTIO_MMIO_BASE_GPA + offset);
}

static uint32_t mmio_read32(uint32_t offset) {
    return *mmio32(offset);
}

static void mmio_write32(uint32_t offset, uint32_t value) {
    *mmio32(offset) = value;
}

static void acknowledge_device_interrupt(void) {
    const uint32_t status = mmio_read32(VIRTIO_MMIO_REG_INTERRUPT_STATUS);
    if (status != 0) {
        mmio_write32(VIRTIO_MMIO_REG_INTERRUPT_ACK, status);
    }
}

static struct driver_virtq_descriptor* rx_descriptors(void) {
    return (struct driver_virtq_descriptor*)(uintptr_t)
        (DRIVER_RX_QUEUE_GPA + DRIVER_DESC_OFFSET);
}

static struct driver_virtq_available* rx_available(void) {
    return (struct driver_virtq_available*)(uintptr_t)
        (DRIVER_RX_QUEUE_GPA + DRIVER_AVAIL_OFFSET);
}

static struct driver_virtq_used* rx_used(void) {
    return (struct driver_virtq_used*)(uintptr_t)
        (DRIVER_RX_QUEUE_GPA + DRIVER_USED_OFFSET);
}

static struct driver_virtq_descriptor* tx_descriptors(void) {
    return (struct driver_virtq_descriptor*)(uintptr_t)
        (DRIVER_TX_QUEUE_GPA + DRIVER_DESC_OFFSET);
}

static struct driver_virtq_available* tx_available(void) {
    return (struct driver_virtq_available*)(uintptr_t)
        (DRIVER_TX_QUEUE_GPA + DRIVER_AVAIL_OFFSET);
}

static struct driver_virtq_used* tx_used(void) {
    return (struct driver_virtq_used*)(uintptr_t)
        (DRIVER_TX_QUEUE_GPA + DRIVER_USED_OFFSET);
}

static uint8_t* rx_buffer(uint16_t index) {
    return (uint8_t*)(uintptr_t)(DRIVER_RX_BUFFER_GPA +
                                 (uint64_t)index * DRIVER_BUFFER_SIZE);
}

static uint8_t* tx_buffer(void) {
    return (uint8_t*)(uintptr_t)DRIVER_TX_BUFFER_GPA;
}

static void write_queue_address(uint32_t low_offset,
                                uint32_t high_offset,
                                uint64_t address) {
    mmio_write32(low_offset, (uint32_t)address);
    mmio_write32(high_offset, (uint32_t)(address >> 32));
}

static int configure_queue(uint32_t queue_index,
                           uint64_t descriptor_address,
                           uint64_t available_address,
                           uint64_t used_address) {
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_SEL, queue_index);
    if (mmio_read32(VIRTIO_MMIO_REG_QUEUE_NUM_MAX) < DRIVER_QUEUE_SIZE) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NUM, DRIVER_QUEUE_SIZE);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DESC_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DESC_HIGH,
                        descriptor_address);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH,
                        available_address);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH,
                        used_address);
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_READY, 1);

    return mmio_read32(VIRTIO_MMIO_REG_QUEUE_READY) == 1
        ? VIRTIO_NET_DRIVER_OK
        : VIRTIO_NET_DRIVER_BAD_ARGUMENT;
}

static void initialize_receive_ring(void) {
    guest_memory_zero((void*)(uintptr_t)DRIVER_RX_QUEUE_GPA,
                      DRIVER_QUEUE_REGION_SIZE);

    struct driver_virtq_descriptor* descriptors = rx_descriptors();
    struct driver_virtq_available* available = rx_available();
    for (uint16_t index = 0; index < DRIVER_QUEUE_SIZE; ++index) {
        descriptors[index].address = DRIVER_RX_BUFFER_GPA +
                                     (uint64_t)index * DRIVER_BUFFER_SIZE;
        descriptors[index].length = DRIVER_BUFFER_SIZE;
        descriptors[index].flags = VIRTQ_DESC_F_WRITE;
        descriptors[index].next = 0;
        available->ring[index] = index;
    }

    guest_memory_barrier();
    available->index = DRIVER_QUEUE_SIZE;
}

static void initialize_transmit_ring(void) {
    guest_memory_zero((void*)(uintptr_t)DRIVER_TX_QUEUE_GPA,
                      DRIVER_QUEUE_REGION_SIZE);

    struct driver_virtq_descriptor* descriptor = tx_descriptors();
    descriptor[0].address = DRIVER_TX_BUFFER_GPA;
    descriptor[0].length = DRIVER_BUFFER_SIZE;
    descriptor[0].flags = 0;
    descriptor[0].next = 0;
}

static void reclaim_transmit_buffer(void) {
    struct driver_virtq_used* used = tx_used();
    const uint16_t completed = used->index;
    if (state.tx_busy && completed != state.tx_used_index) {
        state.tx_used_index = completed;
        state.tx_busy = 0;
    }
}

int virtio_net_driver_init(uint8_t mac[6]) {
    if (mac == NULL) return VIRTIO_NET_DRIVER_BAD_ARGUMENT;

    /*
     * The driver may be initialized only from a clean software state. The
     * device itself is reset below by writing zero to its status register.
     */
    guest_memory_zero(&state, sizeof(state));
    virtio_net_interrupt_pending = 0;

    /*
     * Virtio initialization starts with a device reset. Afterwards, verify
     * that the MMIO region really exposes the Virtio-net device we expect.
     */
    mmio_write32(VIRTIO_MMIO_REG_STATUS, 0);
    if (mmio_read32(VIRTIO_MMIO_REG_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC_VALUE ||
        mmio_read32(VIRTIO_MMIO_REG_VERSION) != VIRTIO_MMIO_VERSION ||
        mmio_read32(VIRTIO_MMIO_REG_DEVICE_ID) != VIRTIO_MMIO_DEVICE_NET) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    /*
     * Status bits describe the initialization handshake:
     *   ACKNOWLEDGE: the Guest recognized the device;
     *   DRIVER:      a Guest driver is ready to configure it.
     * The second write repeats ACKNOWLEDGE because the status register is
     * written with the complete set of bits that should remain asserted.
     */
    mmio_write32(VIRTIO_MMIO_REG_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(VIRTIO_MMIO_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /*
     * Device features are exposed as two 32-bit words. VERSION_1 is bit 32,
     * while the optional MAC feature is bit 5 in the low word.
     */
    mmio_write32(VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL, 0);
    const uint32_t device_features_low =
        mmio_read32(VIRTIO_MMIO_REG_DEVICE_FEATURES);
    mmio_write32(VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL, 1);
    const uint32_t device_features_high =
        mmio_read32(VIRTIO_MMIO_REG_DEVICE_FEATURES);

    if ((device_features_high & (UINT32_C(1) << (VIRTIO_F_VERSION_1 - 32))) == 0) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    /* Accept the MAC feature when offered and require Virtio 1.x layout. */
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL, 0);
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES,
                 device_features_low &
                 (UINT32_C(1) << VIRTIO_NET_F_MAC));
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL, 1);
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES,
                 UINT32_C(1) << (VIRTIO_F_VERSION_1 - 32));

    /*
     * FEATURES_OK asks the device to validate the negotiated feature set.
     * Reading the bit back is mandatory: the device may reject it.
     */
    mmio_write32(VIRTIO_MMIO_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK);
    if ((mmio_read32(VIRTIO_MMIO_REG_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    /* The MAC address is exposed through the Virtio-net configuration space. */
    for (int index = 0; index < 6; ++index) {
        mac[index] = *mmio8(VIRTIO_MMIO_REG_CONFIG_SPACE +
                            (uint32_t)index);
    }

    /*
     * Prepare the fixed Guest-RAM regions and publish both split Virtqueues:
     * queue 0 receives frames from the device, queue 1 transmits frames.
     */
    initialize_receive_ring();
    initialize_transmit_ring();
    if (configure_queue(0,
                        DRIVER_RX_QUEUE_GPA + DRIVER_DESC_OFFSET,
                        DRIVER_RX_QUEUE_GPA + DRIVER_AVAIL_OFFSET,
                        DRIVER_RX_QUEUE_GPA + DRIVER_USED_OFFSET) != VIRTIO_NET_DRIVER_OK ||
        configure_queue(1,
                        DRIVER_TX_QUEUE_GPA + DRIVER_DESC_OFFSET,
                        DRIVER_TX_QUEUE_GPA + DRIVER_AVAIL_OFFSET,
                        DRIVER_TX_QUEUE_GPA + DRIVER_USED_OFFSET) != VIRTIO_NET_DRIVER_OK) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    /*
     * DRIVER_OK tells the device that feature negotiation and queue setup
     * have completed. Only after this point may normal frame I/O begin.
     */
    mmio_write32(VIRTIO_MMIO_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /*
     * The interrupt gate needs a valid GDT/IDT before interrupts are enabled.
     * KVM will later inject VIRTIO_NET_INTERRUPT_VECTOR into this Guest.
     */
    install_guest_gdt();
    install_guest_idt();
    state.initialized = 1;
    asm volatile("sti" ::: "memory");
    return VIRTIO_NET_DRIVER_OK;
}

int virtio_net_driver_send_frame(const uint8_t* frame,
                                 uint16_t frame_length) {
    if (!state.initialized) return VIRTIO_NET_DRIVER_NOT_INITIALIZED;
    if (frame == NULL && frame_length != 0) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }
    if (frame_length >
        DRIVER_BUFFER_SIZE - DRIVER_VIRTIO_HEADER_SIZE) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    reclaim_transmit_buffer();
    if (state.tx_busy) return VIRTIO_NET_DRIVER_WOULD_BLOCK;

    uint8_t* buffer = tx_buffer();
    guest_memory_zero(buffer, DRIVER_VIRTIO_HEADER_SIZE);
    guest_memory_copy(buffer + DRIVER_VIRTIO_HEADER_SIZE,
                      frame,
                      frame_length);

    struct driver_virtq_descriptor* descriptor = tx_descriptors();
    descriptor[0].address = DRIVER_TX_BUFFER_GPA;
    descriptor[0].length = DRIVER_VIRTIO_HEADER_SIZE + frame_length;
    descriptor[0].flags = 0;

    struct driver_virtq_available* available = tx_available();
    available->ring[available->index % DRIVER_QUEUE_SIZE] = 0;
    guest_memory_barrier();
    
    ++available->index;
    state.tx_busy = 1;
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 1);
    guest_memory_barrier();
    return VIRTIO_NET_DRIVER_OK;
}

int virtio_net_driver_receive_frame(uint8_t* frame, size_t capacity) {
    if (!state.initialized) return VIRTIO_NET_DRIVER_NOT_INITIALIZED;
    if (frame == NULL && capacity != 0) {
        return VIRTIO_NET_DRIVER_BAD_ARGUMENT;
    }

    struct driver_virtq_used* used = rx_used();
    const uint16_t completed = used->index;
    if (state.rx_used_index == completed) {
        acknowledge_device_interrupt();
        return VIRTIO_NET_DRIVER_WOULD_BLOCK;
    }

    const uint16_t slot = state.rx_used_index % DRIVER_QUEUE_SIZE;
    const uint32_t descriptor_id = used->ring[slot].id;
    const uint32_t received_length = used->ring[slot].length;
    ++state.rx_used_index;

    int result = VIRTIO_NET_DRIVER_WOULD_BLOCK;
    if (descriptor_id < DRIVER_QUEUE_SIZE) {
        if (received_length < DRIVER_VIRTIO_HEADER_SIZE) {
            result = VIRTIO_NET_DRIVER_BAD_ARGUMENT;
        } else {
            const size_t frame_length =
                received_length - DRIVER_VIRTIO_HEADER_SIZE;
            if (frame_length > capacity) {
                result = VIRTIO_NET_DRIVER_FRAME_TOO_LARGE;
            } else {
                guest_memory_copy(frame,
                                  rx_buffer((uint16_t)descriptor_id) +
                                      DRIVER_VIRTIO_HEADER_SIZE,
                                  frame_length);
                result = (int)frame_length;
            }
        }

        struct driver_virtq_available* available = rx_available();
        available->ring[available->index % DRIVER_QUEUE_SIZE] =
            (uint16_t)descriptor_id;
        guest_memory_barrier();
        ++available->index;
    }

    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 0);
    guest_memory_barrier();
    acknowledge_device_interrupt();
    return result;
}

void virtio_net_driver_poll(void) {
    if (!state.initialized) return;

    clear_interrupt_pending();
    reclaim_transmit_buffer();
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 0);
    guest_memory_barrier();
}

void virtio_net_driver_wait(void) {
    if (!state.initialized || virtio_net_interrupt_pending != 0) return;

    /* KVM returns KVM_EXIT_HLT; the VMM re-enters KVM_RUN after an IRQ. */
    asm volatile("sti\n\thlt\n\tcli" ::: "memory");
}
