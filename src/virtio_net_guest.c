#include "virtio_net.h"

#include <stddef.h>

/*
 * The guest has no allocator, so the driver uses a small, fixed layout in
 * guest RAM.  These addresses are part of this educational device ABI.
 */
#define NET_QUEUE_SIZE          8
#define NET_BUFFER_SIZE         2048
#define NET_VIRTIO_HEADER_SIZE  10

#define NET_RX_QUEUE_GPA        UINT64_C(0x20000)
#define NET_TX_QUEUE_GPA        UINT64_C(0x21000)
#define NET_RX_BUFFER_GPA       UINT64_C(0x22000)
#define NET_TX_BUFFER_GPA       UINT64_C(0x26000)

#define NET_DESC_OFFSET         UINT64_C(0x000)
#define NET_AVAIL_OFFSET        UINT64_C(0x100)
#define NET_USED_OFFSET         UINT64_C(0x200)
#define NET_QUEUE_REGION_SIZE   UINT64_C(0x1000)

#define NET_ETHERNET_HEADER_SIZE 14
#define NET_IPV4_HEADER_SIZE     20
#define NET_UDP_HEADER_SIZE        8
#define NET_ETHERNET_IPV4       UINT16_C(0x0800)
#define NET_ETHERNET_ARP        UINT16_C(0x0806)
#define NET_IP_PROTOCOL_UDP     UINT8_C(17)
#define NET_ARP_REQUEST         UINT16_C(1)
#define NET_ARP_REPLY           UINT16_C(2)

struct virtq_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct virtq_available {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[NET_QUEUE_SIZE];
};

struct virtq_used_element {
    uint32_t id;
    uint32_t length;
};

struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_element ring[NET_QUEUE_SIZE];
};

struct net_state {
    uint8_t initialized;
    uint8_t mac[6];
    uint32_t local_ip;
    uint32_t gateway_ip;
    uint8_t gateway_mac[6];
    uint8_t gateway_known;
    uint8_t arp_request_sent;
    uint8_t tx_busy;
    uint16_t arp_poll_count;
    uint16_t rx_used_index;
    uint16_t tx_used_index;
    uint16_t packet_id;
};

static struct net_state state;

static void memory_zero(void* address, size_t length) {
    uint8_t* bytes = (uint8_t*)address;
    while (length-- != 0) *bytes++ = 0;
}

static void memory_copy(void* destination, const void* source, size_t length) {
    uint8_t* dst = (uint8_t*)destination;
    const uint8_t* src = (const uint8_t*)source;
    while (length-- != 0) *dst++ = *src++;
}

static void memory_barrier(void) {
    asm volatile("" ::: "memory");
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

static uint16_t read_big_endian16(const uint8_t* bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_big_endian16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static uint32_t read_ipv4(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void write_ipv4(uint8_t* bytes, uint32_t address) {
    bytes[0] = (uint8_t)(address >> 24);
    bytes[1] = (uint8_t)(address >> 16);
    bytes[2] = (uint8_t)(address >> 8);
    bytes[3] = (uint8_t)address;
}

static uint16_t internet_checksum(const uint8_t* bytes, size_t length) {
    uint32_t sum = 0;

    while (length >= 2) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length != 0) sum += (uint16_t)bytes[0] << 8;

    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static struct virtq_descriptor* rx_descriptors(void) {
    return (struct virtq_descriptor*)(uintptr_t)(NET_RX_QUEUE_GPA + NET_DESC_OFFSET);
}

static struct virtq_available* rx_available(void) {
    return (struct virtq_available*)(uintptr_t)(NET_RX_QUEUE_GPA + NET_AVAIL_OFFSET);
}

static struct virtq_used* rx_used(void) {
    return (struct virtq_used*)(uintptr_t)(NET_RX_QUEUE_GPA + NET_USED_OFFSET);
}

static struct virtq_descriptor* tx_descriptors(void) {
    return (struct virtq_descriptor*)(uintptr_t)(NET_TX_QUEUE_GPA + NET_DESC_OFFSET);
}

static struct virtq_available* tx_available(void) {
    return (struct virtq_available*)(uintptr_t)(NET_TX_QUEUE_GPA + NET_AVAIL_OFFSET);
}

static struct virtq_used* tx_used(void) {
    return (struct virtq_used*)(uintptr_t)(NET_TX_QUEUE_GPA + NET_USED_OFFSET);
}

static uint8_t* rx_buffer(uint16_t index) {
    return (uint8_t*)(uintptr_t)(NET_RX_BUFFER_GPA +
                                 (uint64_t)index * NET_BUFFER_SIZE);
}

static uint8_t* tx_buffer(void) {
    return (uint8_t*)(uintptr_t)NET_TX_BUFFER_GPA;
}

static void write_queue_address(uint32_t low_offset,
                                uint32_t high_offset,
                                uint64_t address) {
    mmio_write32(low_offset, (uint32_t)address);
    mmio_write32(high_offset, (uint32_t)(address >> 32));
}

static int configure_queue(uint32_t queue_index,
                           uint64_t descriptor_address,
                           uint64_t driver_address,
                           uint64_t device_address) {
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_SEL, queue_index);
    if (mmio_read32(VIRTIO_MMIO_REG_QUEUE_NUM_MAX) < NET_QUEUE_SIZE) {
        return NET_BAD_ARGUMENT;
    }

    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NUM, NET_QUEUE_SIZE);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DESC_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DESC_HIGH,
                        descriptor_address);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH,
                        driver_address);
    write_queue_address(VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW,
                        VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH,
                        device_address);
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_READY, 1);

    return mmio_read32(VIRTIO_MMIO_REG_QUEUE_READY) == 1
        ? NET_OK : NET_BAD_ARGUMENT;
}

static void initialize_receive_ring(void) {
    memory_zero((void*)(uintptr_t)NET_RX_QUEUE_GPA, NET_QUEUE_REGION_SIZE);

    struct virtq_descriptor* descriptors = rx_descriptors();
    struct virtq_available* available = rx_available();
    for (uint16_t index = 0; index < NET_QUEUE_SIZE; ++index) {
        descriptors[index].address = NET_RX_BUFFER_GPA +
                                     (uint64_t)index * NET_BUFFER_SIZE;
        descriptors[index].length = NET_BUFFER_SIZE;
        descriptors[index].flags = VIRTQ_DESC_F_WRITE;
        descriptors[index].next = 0;
        available->ring[index] = index;
    }

    memory_barrier();
    available->index = NET_QUEUE_SIZE;
}

static void initialize_transmit_ring(void) {
    memory_zero((void*)(uintptr_t)NET_TX_QUEUE_GPA, NET_QUEUE_REGION_SIZE);

    struct virtq_descriptor* descriptor = tx_descriptors();
    descriptor[0].address = NET_TX_BUFFER_GPA;
    descriptor[0].length = NET_BUFFER_SIZE;
    descriptor[0].flags = 0;
    descriptor[0].next = 0;
}

static void reclaim_transmit_buffer(void) {
    struct virtq_used* used = tx_used();
    const uint16_t completed = used->index;
    if (state.tx_busy && completed != state.tx_used_index) {
        state.tx_used_index = completed;
        state.tx_busy = 0;
    }
}

static int submit_frame(const uint8_t* frame, uint16_t frame_length) {
    if (frame_length > NET_BUFFER_SIZE - NET_VIRTIO_HEADER_SIZE) {
        return NET_BAD_ARGUMENT;
    }

    reclaim_transmit_buffer();
    if (state.tx_busy) return NET_WOULD_BLOCK;

    uint8_t* buffer = tx_buffer();
    memory_zero(buffer, NET_VIRTIO_HEADER_SIZE);
    memory_copy(buffer + NET_VIRTIO_HEADER_SIZE, frame, frame_length);

    struct virtq_descriptor* descriptor = tx_descriptors();
    descriptor[0].address = NET_TX_BUFFER_GPA;
    descriptor[0].length = NET_VIRTIO_HEADER_SIZE + frame_length;
    descriptor[0].flags = 0;

    struct virtq_available* available = tx_available();
    available->ring[available->index % NET_QUEUE_SIZE] = 0;
    memory_barrier();
    ++available->index;
    state.tx_busy = 1;
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 1);
    memory_barrier();
    return NET_OK;
}

static int send_arp_request(void) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + 28];
    uint8_t* ethernet = frame;
    uint8_t* arp = frame + NET_ETHERNET_HEADER_SIZE;

    for (int i = 0; i < 6; ++i) ethernet[i] = 0xff;
    memory_copy(ethernet + 6, state.mac, 6);
    write_big_endian16(ethernet + 12, NET_ETHERNET_ARP);

    write_big_endian16(arp + 0, 1);              /* Ethernet */
    write_big_endian16(arp + 2, NET_ETHERNET_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_big_endian16(arp + 6, NET_ARP_REQUEST);
    memory_copy(arp + 8, state.mac, 6);
    write_ipv4(arp + 14, state.local_ip);
    memory_zero(arp + 18, 6);
    write_ipv4(arp + 24, state.gateway_ip);

    return submit_frame(frame, sizeof(frame));
}

static void retry_arp_if_needed(void) {
    if (state.gateway_known || !state.arp_request_sent) return;

    ++state.arp_poll_count;
    if (state.arp_poll_count < 32) return;

    if (send_arp_request() == NET_OK) state.arp_poll_count = 0;
}

static int send_arp_reply(const uint8_t target_mac[6], uint32_t target_ip) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + 28];
    uint8_t* ethernet = frame;
    uint8_t* arp = frame + NET_ETHERNET_HEADER_SIZE;

    memory_copy(ethernet, target_mac, 6);
    memory_copy(ethernet + 6, state.mac, 6);
    write_big_endian16(ethernet + 12, NET_ETHERNET_ARP);

    write_big_endian16(arp + 0, 1);
    write_big_endian16(arp + 2, NET_ETHERNET_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_big_endian16(arp + 6, NET_ARP_REPLY);
    memory_copy(arp + 8, state.mac, 6);
    write_ipv4(arp + 14, state.local_ip);
    memory_copy(arp + 18, target_mac, 6);
    write_ipv4(arp + 24, target_ip);

    return submit_frame(frame, sizeof(frame));
}

static void learn_gateway(const uint8_t* sender_mac, uint32_t sender_ip) {
    if (sender_ip != state.gateway_ip) return;
    memory_copy(state.gateway_mac, sender_mac, 6);
    state.gateway_known = 1;
    state.arp_request_sent = 0;
    state.arp_poll_count = 0;
}

static int handle_arp(const uint8_t* frame, size_t frame_length) {
    if (frame_length < NET_ETHERNET_HEADER_SIZE + 28) return NET_WOULD_BLOCK;

    const uint8_t* arp = frame + NET_ETHERNET_HEADER_SIZE;
    if (read_big_endian16(arp + 0) != 1 ||
        read_big_endian16(arp + 2) != NET_ETHERNET_IPV4 ||
        arp[4] != 6 || arp[5] != 4) {
        return NET_WOULD_BLOCK;
    }

    const uint16_t operation = read_big_endian16(arp + 6);
    const uint8_t* sender_mac = arp + 8;
    const uint32_t sender_ip = read_ipv4(arp + 14);
    const uint32_t target_ip = read_ipv4(arp + 24);
    learn_gateway(sender_mac, sender_ip);

    if (operation == NET_ARP_REQUEST && target_ip == state.local_ip) {
        send_arp_reply(sender_mac, sender_ip);
    }
    return NET_WOULD_BLOCK;
}

static int handle_ipv4_udp(const uint8_t* frame,
                           size_t frame_length,
                           uint32_t* source_ip,
                           uint16_t* source_port,
                           void* payload,
                           uint16_t payload_capacity) {
    if (frame_length < NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE) {
        return NET_WOULD_BLOCK;
    }

    const uint8_t* ip = frame + NET_ETHERNET_HEADER_SIZE;
    const uint8_t version_and_length = ip[0];
    const uint8_t version = version_and_length >> 4;
    const uint8_t header_words = version_and_length & 0x0f;
    const size_t header_length = (size_t)header_words * 4;
    if (version != 4 || header_words < 5 ||
        frame_length < NET_ETHERNET_HEADER_SIZE + header_length ||
        ip[9] != NET_IP_PROTOCOL_UDP || read_ipv4(ip + 16) != state.local_ip) {
        return NET_WOULD_BLOCK;
    }

    const uint16_t total_length = read_big_endian16(ip + 2);
    if (total_length < header_length + NET_UDP_HEADER_SIZE ||
        total_length > frame_length - NET_ETHERNET_HEADER_SIZE) {
        return NET_WOULD_BLOCK;
    }

    const uint8_t* udp = ip + header_length;
    const uint16_t udp_length = read_big_endian16(udp + 4);
    if (udp_length < NET_UDP_HEADER_SIZE ||
        udp_length > total_length - header_length) {
        return NET_WOULD_BLOCK;
    }

    const uint16_t payload_length = udp_length - NET_UDP_HEADER_SIZE;
    if (payload_length > payload_capacity) return NET_WOULD_BLOCK;

    if (source_ip != NULL) *source_ip = read_ipv4(ip + 12);
    if (source_port != NULL) *source_port = read_big_endian16(udp + 0);
    if (payload_length != 0 && payload != NULL) {
        memory_copy(payload, udp + NET_UDP_HEADER_SIZE, payload_length);
    }
    return payload_length;
}

static int poll_one_packet(uint32_t* source_ip,
                           uint16_t* source_port,
                           void* payload,
                           uint16_t payload_capacity) {
    struct virtq_used* used = rx_used();
    const uint16_t completed = used->index;

    while (state.rx_used_index != completed) {
        const uint16_t slot = state.rx_used_index % NET_QUEUE_SIZE;
        const uint32_t descriptor_id = used->ring[slot].id;
        const uint32_t received_length = used->ring[slot].length;
        ++state.rx_used_index;

        if (descriptor_id < NET_QUEUE_SIZE &&
            received_length >= NET_VIRTIO_HEADER_SIZE + NET_ETHERNET_HEADER_SIZE) {
            const uint16_t frame_length =
                (uint16_t)(received_length - NET_VIRTIO_HEADER_SIZE);
            const uint8_t* frame = rx_buffer((uint16_t)descriptor_id) +
                                   NET_VIRTIO_HEADER_SIZE;
            const uint16_t ethernet_type = read_big_endian16(frame + 12);
            int result = NET_WOULD_BLOCK;

            if (ethernet_type == NET_ETHERNET_ARP) {
                result = handle_arp(frame, frame_length);
            } else if (ethernet_type == NET_ETHERNET_IPV4) {
                result = handle_ipv4_udp(frame,
                                         frame_length,
                                         source_ip,
                                         source_port,
                                         payload,
                                         payload_capacity);
            }

            /* Return the buffer to the device after inspecting its contents. */
            struct virtq_available* available = rx_available();
            available->ring[available->index % NET_QUEUE_SIZE] =
                (uint16_t)descriptor_id;
            memory_barrier();
            ++available->index;

            if (result >= 0) {
                mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 0);
                return result;
            }
        }
    }

    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 0);
    return NET_WOULD_BLOCK;
}

int net_init(void) {
    memory_zero(&state, sizeof(state));

    mmio_write32(VIRTIO_MMIO_REG_STATUS, 0);
    if (mmio_read32(VIRTIO_MMIO_REG_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC_VALUE ||
        mmio_read32(VIRTIO_MMIO_REG_VERSION) != VIRTIO_MMIO_VERSION ||
        mmio_read32(VIRTIO_MMIO_REG_DEVICE_ID) != VIRTIO_MMIO_DEVICE_NET) {
        return NET_BAD_ARGUMENT;
    }

    mmio_write32(VIRTIO_MMIO_REG_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(VIRTIO_MMIO_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    mmio_write32(VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL, 0);
    const uint32_t device_features_low =
        mmio_read32(VIRTIO_MMIO_REG_DEVICE_FEATURES);
    mmio_write32(VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL, 1);
    const uint32_t device_features_high =
        mmio_read32(VIRTIO_MMIO_REG_DEVICE_FEATURES);

    if ((device_features_high & (UINT32_C(1) << (VIRTIO_F_VERSION_1 - 32))) == 0) {
        return NET_BAD_ARGUMENT;
    }

    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL, 0);
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES,
                 device_features_low & (UINT32_C(1) << VIRTIO_NET_F_MAC));
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL, 1);
    mmio_write32(VIRTIO_MMIO_REG_DRIVER_FEATURES,
                 UINT32_C(1) << (VIRTIO_F_VERSION_1 - 32));

    mmio_write32(VIRTIO_MMIO_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK);
    if ((mmio_read32(VIRTIO_MMIO_REG_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        return NET_BAD_ARGUMENT;
    }

    for (int index = 0; index < 6; ++index) {
        state.mac[index] = *mmio8(VIRTIO_MMIO_REG_CONFIG_SPACE + (uint32_t)index);
    }
    state.local_ip = NET_IPV4(10, 200, state.mac[5], 2);
    state.gateway_ip = NET_IPV4(10, 200, state.mac[5], 1);

    initialize_receive_ring();
    initialize_transmit_ring();
    if (configure_queue(0,
                        NET_RX_QUEUE_GPA + NET_DESC_OFFSET,
                        NET_RX_QUEUE_GPA + NET_AVAIL_OFFSET,
                        NET_RX_QUEUE_GPA + NET_USED_OFFSET) != NET_OK ||
        configure_queue(1,
                        NET_TX_QUEUE_GPA + NET_DESC_OFFSET,
                        NET_TX_QUEUE_GPA + NET_AVAIL_OFFSET,
                        NET_TX_QUEUE_GPA + NET_USED_OFFSET) != NET_OK) {
        return NET_BAD_ARGUMENT;
    }

    mmio_write32(VIRTIO_MMIO_REG_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    state.initialized = 1;
    return NET_OK;
}

uint32_t net_local_ip(void) {
    return state.local_ip;
}

uint32_t net_gateway_ip(void) {
    return state.gateway_ip;
}

int net_udp_send(uint32_t destination_ip,
                 uint16_t source_port,
                 uint16_t destination_port,
                 const void* payload,
                 uint16_t payload_length) {
    if (!state.initialized) return NET_NOT_INITIALIZED;
    if (payload_length > 1472 || (payload_length != 0 && payload == NULL)) {
        return NET_BAD_ARGUMENT;
    }

    if (!state.gateway_known) {
        if (!state.arp_request_sent && send_arp_request() == NET_OK) {
            state.arp_request_sent = 1;
        }
        return NET_WOULD_BLOCK;
    }

    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_UDP_HEADER_SIZE + 1472];
    uint8_t* ethernet = frame;
    uint8_t* ip = frame + NET_ETHERNET_HEADER_SIZE;
    uint8_t* udp = ip + NET_IPV4_HEADER_SIZE;
    const uint16_t total_length =
        NET_IPV4_HEADER_SIZE + NET_UDP_HEADER_SIZE + payload_length;

    memory_copy(ethernet, state.gateway_mac, 6);
    memory_copy(ethernet + 6, state.mac, 6);
    write_big_endian16(ethernet + 12, NET_ETHERNET_IPV4);

    ip[0] = 0x45;
    ip[1] = 0;
    write_big_endian16(ip + 2, total_length);
    write_big_endian16(ip + 4, state.packet_id++);
    write_big_endian16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = NET_IP_PROTOCOL_UDP;
    write_big_endian16(ip + 10, 0);
    write_ipv4(ip + 12, state.local_ip);
    write_ipv4(ip + 16, destination_ip);
    write_big_endian16(ip + 10, internet_checksum(ip, NET_IPV4_HEADER_SIZE));

    write_big_endian16(udp + 0, source_port);
    write_big_endian16(udp + 2, destination_port);
    write_big_endian16(udp + 4, NET_UDP_HEADER_SIZE + payload_length);
    write_big_endian16(udp + 6, 0); /* UDP checksum is optional for IPv4. */
    if (payload_length != 0) {
        memory_copy(udp + NET_UDP_HEADER_SIZE, payload, payload_length);
    }

    return submit_frame(frame, NET_ETHERNET_HEADER_SIZE + total_length);
}

int net_udp_receive(uint32_t* source_ip,
                    uint16_t* source_port,
                    void* payload,
                    uint16_t payload_capacity) {
    if (!state.initialized) return NET_NOT_INITIALIZED;
    if (payload == NULL && payload_capacity != 0) return NET_BAD_ARGUMENT;

    reclaim_transmit_buffer();
    retry_arp_if_needed();
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 0);
    memory_barrier();
    return poll_one_packet(source_ip,
                           source_port,
                           payload,
                           payload_capacity);
}

void net_poll(void) {
    if (!state.initialized) return;
    reclaim_transmit_buffer();
    retry_arp_if_needed();
    mmio_write32(VIRTIO_MMIO_REG_QUEUE_NOTIFY, 0);
    memory_barrier();
    poll_one_packet(NULL, NULL, NULL, 0);
}
