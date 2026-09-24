#include "guest/network_stack.h"

#include "guest/guest_memory.h"
#include "guest/virtio_net.h"

/* Ethernet II header size; the frame excludes the physical-layer FCS. */
#define ETHERNET_HEADER_SIZE 14U
/* Maximum IPv4 header size including options. */
#define IPV4_HEADER_MAX_SIZE 60U
/* Minimum IPv4 header size without options. */
#define IPV4_HEADER_MIN_SIZE 20U
/* UDP header size from RFC 768. */
#define UDP_HEADER_SIZE 8U
/* RFC 826 operation codes for ARP request and reply. */
#define ARP_REQUEST 1U
#define ARP_REPLY 2U
/* EtherType values for ARP and IPv4. */
#define ETHERTYPE_ARP UINT16_C(0x0806)
#define ETHERTYPE_IPV4 UINT16_C(0x0800)
/* IPv4 protocol number assigned to UDP by IANA. */
#define IPV4_PROTOCOL_UDP UINT8_C(17)
/* Ethernet + IPv4 + UDP header overhead at a 1500-byte IPv4 MTU. */
#define UDP_PAYLOAD_MAX (1500U - IPV4_HEADER_MIN_SIZE - UDP_HEADER_SIZE)
/* Fixed receive scratch space; larger frames are rejected by the driver. */
#define RX_FRAME_BUFFER_SIZE 2048U
/* Retry an unanswered gateway ARP after this many receive-poll calls. */
#define ARP_RETRY_POLLS UINT32_C(100000000)

struct pending_datagram {
    uint8_t ready;
    uint16_t length;
    uint16_t source_port;
    uint32_t source_ip;
    uint8_t payload[UDP_PAYLOAD_MAX];
};

struct network_state {
    uint8_t initialized;
    uint8_t mac[6];
    uint8_t gateway_mac[6];
    uint8_t gateway_known;
    uint8_t arp_request_sent;
    uint32_t local_ip;
    uint32_t gateway_ip;
    uint32_t arp_retry_counter;
    uint16_t packet_id;
    struct pending_datagram datagram;
};

static struct network_state state;

static uint16_t read_be16(const uint8_t* bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t* bytes, uint16_t value) {
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

static uint32_t checksum_add(uint32_t sum,
                             const uint8_t* bytes,
                             size_t length) {
    while (length >= 2) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length != 0) sum += (uint16_t)bytes[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t internet_checksum(const uint8_t* bytes, size_t length) {
    return checksum_finish(checksum_add(0, bytes, length));
}

static int udp_checksum_valid(const uint8_t* ip,
                              const uint8_t* udp,
                              uint16_t udp_length) {
    if (read_be16(udp + 6) == 0) return 1;

    uint32_t sum = checksum_add(0, ip + 12, 8);
    sum += IPV4_PROTOCOL_UDP;
    sum += udp_length;
    sum = checksum_add(sum, udp, udp_length);
    return checksum_finish(sum) == 0;
}

static int submit_frame(const uint8_t* frame, uint16_t length) {
    const int status = virtio_net_send_frame(frame, length);
    if (status == VIRTIO_NET_OK) return NET_OK;
    if (status == VIRTIO_NET_WOULD_BLOCK) return NET_WOULD_BLOCK;
    if (status == VIRTIO_NET_NOT_INITIALIZED) return NET_NOT_INITIALIZED;
    return NET_BAD_ARGUMENT;
}

static int send_arp_request(void) {
    uint8_t frame[ETHERNET_HEADER_SIZE + 28U];
    uint8_t* arp = frame + ETHERNET_HEADER_SIZE;
    for (unsigned i = 0; i < 6; ++i) frame[i] = UINT8_C(0xff);
    guest_memory_copy(frame + 6, state.mac, 6);
    write_be16(frame + 12, ETHERTYPE_ARP);

    write_be16(arp + 0, 1);
    write_be16(arp + 2, ETHERTYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, ARP_REQUEST);
    guest_memory_copy(arp + 8, state.mac, 6);
    write_ipv4(arp + 14, state.local_ip);
    guest_memory_zero(arp + 18, 6);
    write_ipv4(arp + 24, state.gateway_ip);

    return submit_frame(frame, sizeof(frame));
}

static void learn_gateway(const uint8_t* sender_mac, uint32_t sender_ip) {
    if (sender_ip != state.gateway_ip) return;
    guest_memory_copy(state.gateway_mac, sender_mac, sizeof(state.gateway_mac));
    state.gateway_known = 1;
    state.arp_request_sent = 0;
    state.arp_retry_counter = 0;
}

static int send_arp_reply(const uint8_t* target_mac, uint32_t target_ip) {
    uint8_t frame[ETHERNET_HEADER_SIZE + 28U];
    uint8_t* arp = frame + ETHERNET_HEADER_SIZE;
    guest_memory_copy(frame, target_mac, 6);
    guest_memory_copy(frame + 6, state.mac, 6);
    write_be16(frame + 12, ETHERTYPE_ARP);

    write_be16(arp + 0, 1);
    write_be16(arp + 2, ETHERTYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, ARP_REPLY);
    guest_memory_copy(arp + 8, state.mac, 6);
    write_ipv4(arp + 14, state.local_ip);
    guest_memory_copy(arp + 18, target_mac, 6);
    write_ipv4(arp + 24, target_ip);
    return submit_frame(frame, sizeof(frame));
}

static int handle_arp(const uint8_t* frame, size_t length) {
    if (length < ETHERNET_HEADER_SIZE + 28U) return NET_WOULD_BLOCK;

    const uint8_t* arp = frame + ETHERNET_HEADER_SIZE;
    if (read_be16(arp + 0) != 1 || read_be16(arp + 2) != ETHERTYPE_IPV4 ||
        arp[4] != 6 || arp[5] != 4) {
        return NET_WOULD_BLOCK;
    }

    const uint16_t operation = read_be16(arp + 6);
    if (operation != ARP_REQUEST && operation != ARP_REPLY) {
        return NET_WOULD_BLOCK;
    }

    const uint8_t* sender_mac = arp + 8;
    const uint32_t sender_ip = read_ipv4(arp + 14);
    const uint32_t target_ip = read_ipv4(arp + 24);
    learn_gateway(sender_mac, sender_ip);

    if (operation == ARP_REQUEST && target_ip == state.local_ip) {
        (void)send_arp_reply(sender_mac, sender_ip);
    }
    return NET_WOULD_BLOCK;
}

static int save_udp_datagram(const uint8_t* ip, size_t ip_available) {
    if (ip_available < IPV4_HEADER_MIN_SIZE) return NET_WOULD_BLOCK;

    const uint8_t version = ip[0] >> 4;
    const uint8_t header_words = ip[0] & 0x0f;
    const size_t header_length = (size_t)header_words * 4;
    if (version != 4 || header_length < IPV4_HEADER_MIN_SIZE ||
        header_length > IPV4_HEADER_MAX_SIZE || header_length > ip_available ||
        ip[9] != IPV4_PROTOCOL_UDP || read_ipv4(ip + 16) != state.local_ip ||
        internet_checksum(ip, header_length) != 0) {
        return NET_WOULD_BLOCK;
    }

    const uint16_t fragment = read_be16(ip + 6);
    if ((fragment & UINT16_C(0x3fff)) != 0) return NET_WOULD_BLOCK;

    const uint16_t total_length = read_be16(ip + 2);
    if (total_length < header_length + UDP_HEADER_SIZE ||
        total_length > ip_available) {
        return NET_WOULD_BLOCK;
    }

    const uint8_t* udp = ip + header_length;
    const uint16_t udp_length = read_be16(udp + 4);
    if (udp_length < UDP_HEADER_SIZE ||
        udp_length > total_length - header_length ||
        !udp_checksum_valid(ip, udp, udp_length)) {
        return NET_WOULD_BLOCK;
    }

    const uint16_t payload_length = udp_length - UDP_HEADER_SIZE;
    if (payload_length > UDP_PAYLOAD_MAX || state.datagram.ready) {
        return NET_WOULD_BLOCK;
    }

    state.datagram.source_ip = read_ipv4(ip + 12);
    state.datagram.source_port = read_be16(udp + 0);
    state.datagram.length = payload_length;
    if (payload_length != 0) {
        guest_memory_copy(state.datagram.payload,
                          udp + UDP_HEADER_SIZE,
                          payload_length);
    }
    state.datagram.ready = 1;
    return payload_length;
}

static int process_frame(const uint8_t* frame, size_t length) {
    if (length < ETHERNET_HEADER_SIZE) return NET_WOULD_BLOCK;

    const uint16_t ether_type = read_be16(frame + 12);
    if (ether_type == ETHERTYPE_ARP) return handle_arp(frame, length);
    uint8_t is_broadcast = 1;
    for (unsigned i = 0; i < 6; ++i) {
        if (frame[i] != UINT8_C(0xff)) is_broadcast = 0;
    }
    uint8_t is_local = 1;
    for (unsigned i = 0; i < 6; ++i) {
        if (frame[i] != state.mac[i]) is_local = 0;
    }
    if (ether_type != ETHERTYPE_IPV4 || (!is_broadcast && !is_local)) {
        return NET_WOULD_BLOCK;
    }

    return save_udp_datagram(frame + ETHERNET_HEADER_SIZE,
                             length - ETHERNET_HEADER_SIZE);
}

static void retry_arp(void) {
    if (state.gateway_known || !state.arp_request_sent) return;
    if (++state.arp_retry_counter < ARP_RETRY_POLLS) return;
    if (send_arp_request() == NET_OK) state.arp_retry_counter = 0;
}

static void drain_receive_queue(void) {
    uint8_t frame[RX_FRAME_BUFFER_SIZE];
    for (;;) {
        const int length = virtio_net_receive_frame(frame, sizeof(frame));
        if (length == VIRTIO_NET_WOULD_BLOCK) break;
        if (length < 0) continue;
        (void)process_frame(frame, (size_t)length);
        if (state.datagram.ready) break;
    }
}

int net_init(void) {
    guest_memory_zero(&state, sizeof(state));
    if (virtio_net_init(state.mac) != VIRTIO_NET_OK) return NET_BAD_ARGUMENT;

    const uint8_t vm_octet = state.mac[5];
    state.local_ip = NET_IPV4(10, 200, vm_octet, 2);
    state.gateway_ip = NET_IPV4(10, 200, vm_octet, 1);
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
    if ((payload == NULL && payload_length != 0) ||
        payload_length > UDP_PAYLOAD_MAX) {
        return NET_BAD_ARGUMENT;
    }

    if (!state.gateway_known) {
        if (!state.arp_request_sent && send_arp_request() == NET_OK) {
            state.arp_request_sent = 1;
            state.arp_retry_counter = 0;
        }
        return NET_WOULD_BLOCK;
    }

    uint8_t frame[ETHERNET_HEADER_SIZE + IPV4_HEADER_MIN_SIZE +
                  UDP_HEADER_SIZE + UDP_PAYLOAD_MAX];
    uint8_t* ip = frame + ETHERNET_HEADER_SIZE;
    uint8_t* udp = ip + IPV4_HEADER_MIN_SIZE;
    const uint16_t ip_length = IPV4_HEADER_MIN_SIZE + UDP_HEADER_SIZE +
                               payload_length;

    guest_memory_copy(frame, state.gateway_mac, 6);
    guest_memory_copy(frame + 6, state.mac, 6);
    write_be16(frame + 12, ETHERTYPE_IPV4);

    ip[0] = UINT8_C(0x45);
    ip[1] = 0;
    write_be16(ip + 2, ip_length);
    write_be16(ip + 4, state.packet_id++);
    write_be16(ip + 6, UINT16_C(0x4000));
    ip[8] = 64;
    ip[9] = IPV4_PROTOCOL_UDP;
    write_be16(ip + 10, 0);
    write_ipv4(ip + 12, state.local_ip);
    write_ipv4(ip + 16, destination_ip);
    write_be16(ip + 10, internet_checksum(ip, IPV4_HEADER_MIN_SIZE));

    write_be16(udp + 0, source_port);
    write_be16(udp + 2, destination_port);
    write_be16(udp + 4, UDP_HEADER_SIZE + payload_length);
    write_be16(udp + 6, 0);
    if (payload_length != 0) {
        guest_memory_copy(udp + UDP_HEADER_SIZE, payload, payload_length);
    }

    return submit_frame(frame,
                        ETHERNET_HEADER_SIZE + ip_length);
}

int net_udp_receive(uint32_t* source_ip,
                    uint16_t* source_port,
                    void* payload,
                    uint16_t payload_capacity) {
    if (!state.initialized) return NET_NOT_INITIALIZED;
    if (payload == NULL && payload_capacity != 0) return NET_BAD_ARGUMENT;

    virtio_net_poll();
    retry_arp();
    if (!state.datagram.ready) drain_receive_queue();
    if (!state.datagram.ready) return NET_WOULD_BLOCK;
    if (state.datagram.length > payload_capacity) {
        state.datagram.ready = 0;
        return NET_BAD_ARGUMENT;
    }

    if (source_ip != NULL) *source_ip = state.datagram.source_ip;
    if (source_port != NULL) *source_port = state.datagram.source_port;
    if (state.datagram.length != 0) {
        guest_memory_copy(payload,
                          state.datagram.payload,
                          state.datagram.length);
    }
    const int result = state.datagram.length;
    state.datagram.ready = 0;
    return result;
}

void net_poll(void) {
    if (!state.initialized) return;
    virtio_net_poll();
    retry_arp();
    if (!state.datagram.ready) drain_receive_queue();
}
