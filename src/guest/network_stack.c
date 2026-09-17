#include "guest/virtio_net.h"

#include "guest/guest_memory.h"
#include "guest/virtio_net_driver.h"


/* Maximum Ethernet-frame size supplied by the lower-level Virtio driver;
 * the 10-byte Virtio-net transport header is not included here. */
#define NET_FRAME_BUFFER_SIZE \
    VIRTIO_NET_DRIVER_MAX_FRAME_SIZE

/* IEEE 802 Ethernet II header without the optional FCS: 6 + 6 + 2 bytes. */
#define NET_ETHERNET_HEADER_SIZE 14
/* RFC 791 minimum IPv4 header size, with no IP options. */
#define NET_IPV4_HEADER_SIZE     20
/* RFC 768 UDP header size. */
#define NET_UDP_HEADER_SIZE       8
/* IEEE 802 Ethernet II EtherType 0x0800: the payload is IPv4. */
#define NET_ETHERNET_IPV4       UINT16_C(0x0800)
/* IEEE 802 Ethernet II EtherType 0x0806: the payload is ARP. */
#define NET_ETHERNET_ARP        UINT16_C(0x0806)
/* IANA IP protocol number 17, assigned to UDP by RFC 768. */
#define NET_IP_PROTOCOL_UDP     UINT8_C(17)
/* RFC 826 ARP operation code 1: request an IP-to-MAC mapping. */
#define NET_ARP_REQUEST         UINT16_C(1)
/* RFC 826 ARP operation code 2: reply with an IP-to-MAC mapping. */
#define NET_ARP_REPLY           UINT16_C(2)
/* RFC 826 fixed ARP payload size for Ethernet + IPv4: 28 bytes. */
#define NET_ARP_PAYLOAD_SIZE    28
/* Project limit based on a 1500-byte IPv4 MTU: 1500 - 20 - 8 = 1472. */
#define NET_MAX_UDP_PAYLOAD     1472

struct network_state {
    uint8_t initialized;
    uint8_t mac[6];
    uint32_t local_ip;
    uint32_t gateway_ip;
    uint8_t gateway_mac[6];
    uint8_t gateway_known;
    uint8_t arp_request_sent;
    uint16_t arp_poll_count;
    uint16_t packet_id;
};

static struct network_state state;

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

static int submit_frame(const uint8_t* frame, uint16_t frame_length) {
    const int status =
        virtio_net_driver_send_frame(frame, frame_length);
    if (status == VIRTIO_NET_DRIVER_OK) return NET_OK;
    if (status == VIRTIO_NET_DRIVER_WOULD_BLOCK) return NET_WOULD_BLOCK;
    if (status == VIRTIO_NET_DRIVER_NOT_INITIALIZED) {
        return NET_NOT_INITIALIZED;
    }
    return NET_BAD_ARGUMENT;
}

static int send_arp_request(void) {
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_ARP_PAYLOAD_SIZE];
    uint8_t* ethernet = frame;
    uint8_t* arp = frame + NET_ETHERNET_HEADER_SIZE;

    for (int i = 0; i < 6; ++i) ethernet[i] = 0xff;
    guest_memory_copy(ethernet + 6, state.mac, 6);
    write_big_endian16(ethernet + 12, NET_ETHERNET_ARP);

    write_big_endian16(arp + 0, 1);
    write_big_endian16(arp + 2, NET_ETHERNET_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_big_endian16(arp + 6, NET_ARP_REQUEST);
    guest_memory_copy(arp + 8, state.mac, 6);
    write_ipv4(arp + 14, state.local_ip);
    guest_memory_zero(arp + 18, 6);
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
    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_ARP_PAYLOAD_SIZE];
    uint8_t* ethernet = frame;
    uint8_t* arp = frame + NET_ETHERNET_HEADER_SIZE;

    guest_memory_copy(ethernet, target_mac, 6);
    guest_memory_copy(ethernet + 6, state.mac, 6);
    write_big_endian16(ethernet + 12, NET_ETHERNET_ARP);

    write_big_endian16(arp + 0, 1);
    write_big_endian16(arp + 2, NET_ETHERNET_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_big_endian16(arp + 6, NET_ARP_REPLY);
    guest_memory_copy(arp + 8, state.mac, 6);
    write_ipv4(arp + 14, state.local_ip);
    guest_memory_copy(arp + 18, target_mac, 6);
    write_ipv4(arp + 24, target_ip);

    return submit_frame(frame, sizeof(frame));
}

static void learn_gateway(const uint8_t* sender_mac, uint32_t sender_ip) {
    if (sender_ip != state.gateway_ip) return;

    guest_memory_copy(state.gateway_mac, sender_mac, 6);
    state.gateway_known = 1;
    state.arp_request_sent = 0;
    state.arp_poll_count = 0;
}

static int handle_arp(const uint8_t* frame, size_t frame_length) {
    if (frame_length < NET_ETHERNET_HEADER_SIZE + NET_ARP_PAYLOAD_SIZE) {
        return NET_WOULD_BLOCK;
    }

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
        (void)send_arp_reply(sender_mac, sender_ip);
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
        ip[9] != NET_IP_PROTOCOL_UDP ||
        read_ipv4(ip + 16) != state.local_ip) {
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
        guest_memory_copy(payload,
                          udp + NET_UDP_HEADER_SIZE,
                          payload_length);
    }
    return payload_length;
}

static int poll_one_packet(uint32_t* source_ip,
                           uint16_t* source_port,
                           void* payload,
                           uint16_t payload_capacity) {
    uint8_t frame[NET_FRAME_BUFFER_SIZE];

    for (;;) {
        const int received_length =
            virtio_net_driver_receive_frame(frame, sizeof(frame));
        if (received_length == VIRTIO_NET_DRIVER_WOULD_BLOCK) {
            return NET_WOULD_BLOCK;
        }
        if (received_length < 0) continue;

        const size_t frame_length = (size_t)received_length;
        if (frame_length < NET_ETHERNET_HEADER_SIZE) continue;

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

        if (result >= 0) return result;
    }
}

int net_init(void) {
    uint8_t mac[6];
    guest_memory_zero(&state, sizeof(state));

    if (virtio_net_driver_init(mac) != VIRTIO_NET_DRIVER_OK) {
        return NET_BAD_ARGUMENT;
    }

    guest_memory_copy(state.mac, mac, sizeof(state.mac));
    state.local_ip = NET_IPV4(10, 200, state.mac[5], 2);
    state.gateway_ip = NET_IPV4(10, 200, state.mac[5], 1);
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
    if (payload_length > NET_MAX_UDP_PAYLOAD ||
        (payload_length != 0 && payload == NULL)) {
        return NET_BAD_ARGUMENT;
    }

    if (!state.gateway_known) {
        if (!state.arp_request_sent && send_arp_request() == NET_OK) {
            state.arp_request_sent = 1;
        }
        return NET_WOULD_BLOCK;
    }

    uint8_t frame[NET_ETHERNET_HEADER_SIZE + NET_IPV4_HEADER_SIZE +
                  NET_UDP_HEADER_SIZE + NET_MAX_UDP_PAYLOAD];
    uint8_t* ethernet = frame;
    uint8_t* ip = frame + NET_ETHERNET_HEADER_SIZE;
    uint8_t* udp = ip + NET_IPV4_HEADER_SIZE;
    const uint16_t total_length =
        NET_IPV4_HEADER_SIZE + NET_UDP_HEADER_SIZE + payload_length;

    guest_memory_copy(ethernet, state.gateway_mac, 6);
    guest_memory_copy(ethernet + 6, state.mac, 6);
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
    write_big_endian16(ip + 10,
                       internet_checksum(ip, NET_IPV4_HEADER_SIZE));

    write_big_endian16(udp + 0, source_port);
    write_big_endian16(udp + 2, destination_port);
    write_big_endian16(udp + 4, NET_UDP_HEADER_SIZE + payload_length);
    write_big_endian16(udp + 6, 0);
    if (payload_length != 0) {
        guest_memory_copy(udp + NET_UDP_HEADER_SIZE,
                          payload,
                          payload_length);
    }

    return submit_frame(frame, NET_ETHERNET_HEADER_SIZE + total_length);
}

int net_udp_receive(uint32_t* source_ip,
                    uint16_t* source_port,
                    void* payload,
                    uint16_t payload_capacity) {
    if (!state.initialized) return NET_NOT_INITIALIZED;
    if (payload == NULL && payload_capacity != 0) return NET_BAD_ARGUMENT;

    virtio_net_driver_poll();
    retry_arp_if_needed();
    return poll_one_packet(source_ip,
                           source_port,
                           payload,
                           payload_capacity);
}

void net_poll(void) {
    if (!state.initialized) return;

    virtio_net_driver_poll();
    retry_arp_if_needed();
    (void)poll_one_packet(NULL, NULL, NULL, 0);
}

void net_wait(void) {
    if (!state.initialized) return;
    virtio_net_driver_wait();
}
