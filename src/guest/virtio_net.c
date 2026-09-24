#include "guest/virtio_net.h"

#include "guest/guest_dma.h"
#include "guest/guest_memory.h"
#include "virtio_defs.h"
#include "virtio_mmio.h"

/* Virtio-net split queue 0 receives Ethernet frames. */
#define RX_QUEUE_INDEX 0U
/* Virtio-net split queue 1 transmits Ethernet frames. */
#define TX_QUEUE_INDEX 1U
/* Host device model advertises eight descriptors per queue. */
#define VIRTIO_NET_QUEUE_LIMIT 8U
/* Buffer capacity includes enough room for the Virtio header and an MTU frame. */
#define VIRTIO_NET_BUFFER_SIZE 2048U
/* Split-ring available flag: suppress device-to-driver completion interrupts. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT UINT16_C(1)
/* Ethernet frames shorter than 60 bytes are zero-padded before transmission. */
#define ETHERNET_MIN_FRAME_SIZE 60U

struct virtq_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct virtq_available {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTIO_NET_QUEUE_LIMIT];
};

struct virtq_used_element {
    uint32_t id;
    uint32_t length;
};

struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_element ring[VIRTIO_NET_QUEUE_LIMIT];
};

struct virtqueue {
    struct virtq_descriptor* descriptors;
    struct virtq_available* available;
    struct virtq_used* used;
    uint8_t* buffers;
    uint8_t busy[VIRTIO_NET_QUEUE_LIMIT];
    uint16_t size;
    uint16_t available_index;
    uint16_t last_used_index;
};

struct virtio_net_state {
    struct virtqueue rx;
    struct virtqueue tx;
    uint8_t mac[6];
    uint8_t initialized;
};

static struct virtio_net_state state;

static volatile uint32_t* mmio_register(uint32_t offset) {
    return (volatile uint32_t*)(uintptr_t)(VIRTIO_MMIO_BASE_GPA + offset);
}

static uint32_t mmio_read(uint32_t offset) {
    return *mmio_register(offset);
}

static void mmio_write(uint32_t offset, uint32_t value) {
    *mmio_register(offset) = value;
}

static void write_queue_address(uint32_t low_offset,
                                uint32_t high_offset,
                                uint64_t address) {
    mmio_write(low_offset, (uint32_t)address);
    mmio_write(high_offset, (uint32_t)(address >> 32));
}

static uint16_t select_queue_size(uint32_t maximum) {
    if (maximum > VIRTIO_NET_QUEUE_LIMIT) maximum = VIRTIO_NET_QUEUE_LIMIT;
    uint16_t size = 1;
    while ((uint32_t)size * 2 <= maximum) size = (uint16_t)(size * 2);
    return size <= maximum ? size : 0;
}

static uint64_t offered_features(void) {
    mmio_write(VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL, 0);
    const uint64_t low = mmio_read(VIRTIO_MMIO_REG_DEVICE_FEATURES);
    mmio_write(VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL, 1);
    const uint64_t high = mmio_read(VIRTIO_MMIO_REG_DEVICE_FEATURES);
    return low | (high << 32);
}

static void release_queue(struct virtqueue* queue) {
    guest_dma_free(queue->descriptors);
    guest_dma_free(queue->available);
    guest_dma_free(queue->used);
    guest_dma_free(queue->buffers);
    guest_memory_zero(queue, sizeof(*queue));
}

static void release_state(void) {
    release_queue(&state.rx);
    release_queue(&state.tx);
    guest_memory_zero(state.mac, sizeof(state.mac));
    state.initialized = 0;
}

static int fail_initialization(void) {
    mmio_write(VIRTIO_MMIO_REG_STATUS, 0);
    mmio_write(VIRTIO_MMIO_REG_STATUS, VIRTIO_STATUS_FAILED);
    release_state();
    return VIRTIO_NET_BAD_ARGUMENT;
}

static int allocate_queue(struct virtqueue* queue,
                          uint16_t index,
                          uint16_t size,
                          uint8_t receive_queue) {
    queue->size = size;
    queue->descriptors = guest_dma_alloc(
        (size_t)size * sizeof(*queue->descriptors), 16);
    queue->available = guest_dma_alloc(
        sizeof(uint16_t) * (2U + size), 2);
    queue->used = guest_dma_alloc(
        sizeof(uint16_t) * 2U +
            (size_t)size * sizeof(struct virtq_used_element),
        4);
    queue->buffers = guest_dma_alloc((size_t)size * VIRTIO_NET_BUFFER_SIZE, 16);
    if (queue->descriptors == NULL || queue->available == NULL ||
        queue->used == NULL || queue->buffers == NULL) {
        return VIRTIO_NET_BAD_ARGUMENT;
    }

    guest_memory_zero(queue->descriptors,
                      (size_t)size * sizeof(*queue->descriptors));
    guest_memory_zero(queue->available,
                      sizeof(uint16_t) * (2U + size));
    guest_memory_zero(queue->used,
                      sizeof(uint16_t) * 2U +
                          (size_t)size * sizeof(struct virtq_used_element));
    guest_memory_zero(queue->buffers,
                      (size_t)size * VIRTIO_NET_BUFFER_SIZE);
    queue->available->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    for (uint16_t slot = 0; slot < size; ++slot) {
        queue->descriptors[slot].address =
            (uint64_t)(uintptr_t)(queue->buffers +
                                  (size_t)slot * VIRTIO_NET_BUFFER_SIZE);
        queue->descriptors[slot].length = VIRTIO_NET_BUFFER_SIZE;
        queue->descriptors[slot].flags = receive_queue ? VIRTQ_DESC_F_WRITE : 0;
        queue->descriptors[slot].next = 0;
        if (receive_queue) queue->available->ring[slot] = slot;
    }

    if (receive_queue) {
        guest_memory_barrier();
        queue->available_index = size;
        queue->available->index = size;
    }

    mmio_write(VIRTIO_MMIO_REG_QUEUE_SEL, index);
    mmio_write(VIRTIO_MMIO_REG_QUEUE_NUM, size);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DESC_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DESC_HIGH,
                        (uint64_t)(uintptr_t)queue->descriptors);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH,
                        (uint64_t)(uintptr_t)queue->available);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH,
                        (uint64_t)(uintptr_t)queue->used);
    mmio_write(VIRTIO_MMIO_REG_QUEUE_READY, 1);
    return mmio_read(VIRTIO_MMIO_REG_QUEUE_READY) == 1
        ? VIRTIO_NET_OK : VIRTIO_NET_BAD_ARGUMENT;
}

static void reclaim_transmit_buffers(void) {
    struct virtqueue* queue = &state.tx;
    const uint16_t completed =
        __atomic_load_n(&queue->used->index, __ATOMIC_ACQUIRE);

    while (queue->last_used_index != completed) {
        const uint16_t slot = queue->last_used_index % queue->size;
        const uint32_t descriptor = queue->used->ring[slot].id;
        if (descriptor < queue->size) queue->busy[descriptor] = 0;
        ++queue->last_used_index;
    }
}

int virtio_net_init(uint8_t mac[6]) {
    if (mac == NULL) return VIRTIO_NET_BAD_ARGUMENT;
    if (state.initialized) {
        guest_memory_copy(mac, state.mac, sizeof(state.mac));
        return VIRTIO_NET_OK;
    }

    if (mmio_read(VIRTIO_MMIO_REG_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC_VALUE ||
        mmio_read(VIRTIO_MMIO_REG_VERSION) != VIRTIO_MMIO_VERSION ||
        mmio_read(VIRTIO_MMIO_REG_DEVICE_ID) != VIRTIO_DEVICE_ID_NET) {
        return VIRTIO_NET_BAD_ARGUMENT;
    }

    mmio_write(VIRTIO_MMIO_REG_STATUS, 0);
    mmio_write(VIRTIO_MMIO_REG_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write(VIRTIO_MMIO_REG_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    const uint64_t required =
        (UINT64_C(1) << VIRTIO_F_VERSION_1) |
        (UINT64_C(1) << VIRTIO_NET_F_MAC);
    const uint64_t offered = offered_features();
    if ((offered & required) != required) return fail_initialization();

    mmio_write(VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL, 0);
    mmio_write(VIRTIO_MMIO_REG_DRIVER_FEATURES, (uint32_t)required);
    mmio_write(VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL, 1);
    mmio_write(VIRTIO_MMIO_REG_DRIVER_FEATURES,
               (uint32_t)(required >> 32));

    const uint8_t features_ok = VIRTIO_STATUS_ACKNOWLEDGE |
                                VIRTIO_STATUS_DRIVER |
                                VIRTIO_STATUS_FEATURES_OK;
    mmio_write(VIRTIO_MMIO_REG_STATUS, features_ok);
    const uint32_t status = mmio_read(VIRTIO_MMIO_REG_STATUS);
    if ((status & VIRTIO_STATUS_FEATURES_OK) == 0 ||
        (status & VIRTIO_STATUS_FAILED) != 0) {
        return fail_initialization();
    }

    volatile uint8_t* config = (volatile uint8_t*)(uintptr_t)(
        VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_REG_CONFIG_SPACE);
    for (unsigned i = 0; i < sizeof(state.mac); ++i) state.mac[i] = config[i];

    mmio_write(VIRTIO_MMIO_REG_QUEUE_SEL, RX_QUEUE_INDEX);
    const uint16_t rx_size = select_queue_size(
        mmio_read(VIRTIO_MMIO_REG_QUEUE_NUM_MAX));
    mmio_write(VIRTIO_MMIO_REG_QUEUE_SEL, TX_QUEUE_INDEX);
    const uint16_t tx_size = select_queue_size(
        mmio_read(VIRTIO_MMIO_REG_QUEUE_NUM_MAX));
    if (rx_size == 0 || tx_size == 0 ||
        allocate_queue(&state.rx, RX_QUEUE_INDEX, rx_size, 1) != VIRTIO_NET_OK ||
        allocate_queue(&state.tx, TX_QUEUE_INDEX, tx_size, 0) != VIRTIO_NET_OK) {
        return fail_initialization();
    }

    mmio_write(VIRTIO_MMIO_REG_STATUS,
               features_ok | VIRTIO_STATUS_DRIVER_OK);
    const uint32_t final_status = mmio_read(VIRTIO_MMIO_REG_STATUS);
    if ((final_status & VIRTIO_STATUS_DRIVER_OK) == 0 ||
        (final_status & VIRTIO_STATUS_FAILED) != 0) {
        return fail_initialization();
    }

    state.initialized = 1;
    guest_memory_copy(mac, state.mac, sizeof(state.mac));
    return VIRTIO_NET_OK;
}

int virtio_net_send_frame(const uint8_t* frame, uint16_t length) {
    if (!state.initialized) return VIRTIO_NET_NOT_INITIALIZED;
    if ((frame == NULL && length != 0) || length > VIRTIO_NET_FRAME_CAPACITY) {
        return VIRTIO_NET_BAD_ARGUMENT;
    }

    reclaim_transmit_buffers();
    struct virtqueue* queue = &state.tx;
    uint16_t descriptor = 0;
    while (descriptor < queue->size && queue->busy[descriptor]) ++descriptor;
    if (descriptor == queue->size) return VIRTIO_NET_WOULD_BLOCK;

    uint16_t frame_size = length;
    if (frame_size < ETHERNET_MIN_FRAME_SIZE) frame_size = ETHERNET_MIN_FRAME_SIZE;
    uint8_t* buffer = queue->buffers +
                      (size_t)descriptor * VIRTIO_NET_BUFFER_SIZE;
    guest_memory_zero(buffer, VIRTIO_NET_HEADER_SIZE + frame_size);
    if (length != 0) {
        guest_memory_copy(buffer + VIRTIO_NET_HEADER_SIZE, frame, length);
    }

    queue->descriptors[descriptor].length =
        VIRTIO_NET_HEADER_SIZE + frame_size;
    queue->available->ring[queue->available_index % queue->size] = descriptor;
    queue->busy[descriptor] = 1;
    guest_memory_barrier();
    ++queue->available_index;
    queue->available->index = queue->available_index;
    guest_memory_barrier();
    mmio_write(VIRTIO_MMIO_REG_QUEUE_NOTIFY, TX_QUEUE_INDEX);
    return VIRTIO_NET_OK;
}

int virtio_net_receive_frame(uint8_t* frame, size_t capacity) {
    if (!state.initialized) return VIRTIO_NET_NOT_INITIALIZED;
    if (frame == NULL && capacity != 0) return VIRTIO_NET_BAD_ARGUMENT;

    struct virtqueue* queue = &state.rx;
    const uint16_t completed =
        __atomic_load_n(&queue->used->index, __ATOMIC_ACQUIRE);
    if (queue->last_used_index == completed) return VIRTIO_NET_WOULD_BLOCK;

    const uint16_t used_slot = queue->last_used_index % queue->size;
    const uint32_t descriptor = queue->used->ring[used_slot].id;
    const uint32_t length = queue->used->ring[used_slot].length;
    ++queue->last_used_index;

    int result = VIRTIO_NET_BAD_ARGUMENT;
    if (descriptor < queue->size && length >= VIRTIO_NET_HEADER_SIZE &&
        length <= VIRTIO_NET_BUFFER_SIZE) {
        const size_t frame_length = length - VIRTIO_NET_HEADER_SIZE;
        if (frame_length > capacity) {
            result = VIRTIO_NET_FRAME_TOO_LARGE;
        } else {
            guest_memory_copy(frame,
                              queue->buffers +
                                  (size_t)descriptor * VIRTIO_NET_BUFFER_SIZE +
                                  VIRTIO_NET_HEADER_SIZE,
                              frame_length);
            result = (int)frame_length;
        }

        queue->available->ring[queue->available_index % queue->size] =
            (uint16_t)descriptor;
        guest_memory_barrier();
        ++queue->available_index;
        queue->available->index = queue->available_index;
        guest_memory_barrier();
        mmio_write(VIRTIO_MMIO_REG_QUEUE_NOTIFY, RX_QUEUE_INDEX);
    }
    return result;
}

void virtio_net_poll(void) {
    if (state.initialized) reclaim_transmit_buffers();
}
