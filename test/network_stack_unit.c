#include "guest/network_stack.h"
#include "guest/virtio_net.h"

#include <stdio.h>
#include <stdint.h>

/* Test-only maximum frame storage for mocked Virtio queues. */
#define TEST_FRAME_CAPACITY 2048U
/* Test-only number of captured TX frames. */
#define TEST_TX_CAPACITY 4U

static const uint8_t guest_mac[6] = {0x52, 0x54, 0, 0x12, 0, 1};
static const uint8_t gateway_mac[6] = {0x02, 0, 0, 0, 0, 1};
static uint8_t tx_frames[TEST_TX_CAPACITY][TEST_FRAME_CAPACITY];
static size_t tx_lengths[TEST_TX_CAPACITY];
static size_t tx_count;
static uint8_t rx_frames[2][TEST_FRAME_CAPACITY];
static size_t rx_lengths[2];
static size_t rx_count;
static size_t rx_index;

static uint16_t read_be16(const uint8_t* bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
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

static uint16_t make_udp_checksum(const uint8_t* ip,
                                  const uint8_t* udp,
                                  uint16_t udp_length) {
    uint32_t sum = checksum_add(0, ip + 12, 8);
    sum += 17;
    sum += udp_length;
    sum = checksum_add(sum, udp, udp_length);
    const uint16_t checksum = checksum_finish(sum);
    return checksum == 0 ? UINT16_C(0xffff) : checksum;
}

static int check(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "network_stack_unit: %s\n", message);
    return 0;
}

int virtio_net_init(uint8_t mac[6]) {
    for (unsigned i = 0; i < 6; ++i) mac[i] = guest_mac[i];
    return VIRTIO_NET_OK;
}

int virtio_net_send_frame(const uint8_t* frame, uint16_t length) {
    if (tx_count == TEST_TX_CAPACITY || length > TEST_FRAME_CAPACITY) {
        return VIRTIO_NET_WOULD_BLOCK;
    }
    for (uint16_t i = 0; i < length; ++i) tx_frames[tx_count][i] = frame[i];
    tx_lengths[tx_count] = length;
    ++tx_count;
    return VIRTIO_NET_OK;
}

int virtio_net_receive_frame(uint8_t* frame, size_t capacity) {
    if (rx_index == rx_count) return VIRTIO_NET_WOULD_BLOCK;
    if (rx_lengths[rx_index] > capacity) return VIRTIO_NET_FRAME_TOO_LARGE;
    const size_t length = rx_lengths[rx_index];
    for (size_t i = 0; i < length; ++i) frame[i] = rx_frames[rx_index][i];
    ++rx_index;
    return (int)length;
}

void virtio_net_poll(void) {}

static int queue_rx_frame(const uint8_t* frame, size_t length) {
    if (rx_count == 2 || length > TEST_FRAME_CAPACITY) return 0;
    for (size_t i = 0; i < length; ++i) rx_frames[rx_count][i] = frame[i];
    rx_lengths[rx_count] = length;
    ++rx_count;
    return 1;
}

static void make_arp_reply(uint8_t frame[42]) {
    for (unsigned i = 0; i < 6; ++i) {
        frame[i] = guest_mac[i];
        frame[6 + i] = gateway_mac[i];
    }
    write_be16(frame + 12, UINT16_C(0x0806));
    uint8_t* arp = frame + 14;
    write_be16(arp + 0, 1);
    write_be16(arp + 2, UINT16_C(0x0800));
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, 2);
    for (unsigned i = 0; i < 6; ++i) {
        arp[8 + i] = gateway_mac[i];
        arp[18 + i] = guest_mac[i];
    }
    write_ipv4(arp + 14, NET_IPV4(10, 200, 1, 1));
    write_ipv4(arp + 24, NET_IPV4(10, 200, 1, 2));
}

static size_t make_udp_echo_reply(uint8_t* frame,
                                  const uint8_t* payload,
                                  uint16_t payload_length) {
    static const uint8_t peer_mac[6] = {0x02, 0, 0, 0, 0, 9};
    const uint16_t ip_length = 20 + 8 + payload_length;
    const size_t frame_length = 14 + ip_length;

    for (unsigned i = 0; i < 6; ++i) {
        frame[i] = guest_mac[i];
        frame[6 + i] = peer_mac[i];
    }
    write_be16(frame + 12, UINT16_C(0x0800));
    uint8_t* ip = frame + 14;
    ip[0] = UINT8_C(0x45);
    ip[1] = 0;
    write_be16(ip + 2, ip_length);
    write_be16(ip + 4, 7);
    write_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = 17;
    write_be16(ip + 10, 0);
    write_ipv4(ip + 12, NET_IPV4(203, 0, 113, 9));
    write_ipv4(ip + 16, NET_IPV4(10, 200, 1, 2));
    write_be16(ip + 10, checksum_finish(checksum_add(0, ip, 20)));

    uint8_t* udp = ip + 20;
    write_be16(udp + 0, 9999);
    write_be16(udp + 2, 4000);
    write_be16(udp + 4, 8 + payload_length);
    write_be16(udp + 6, 0);
    for (uint16_t i = 0; i < payload_length; ++i) udp[8 + i] = payload[i];
    write_be16(udp + 6,
               make_udp_checksum(ip, udp, 8 + payload_length));
    return frame_length;
}

int main(void) {
    static const uint8_t message[] = "echo unit payload";
    uint8_t reply[64];
    uint32_t source_ip = 0;
    uint16_t source_port = 0;

    if (!check(net_init() == NET_OK, "Guest network initialization failed") ||
        !check(net_local_ip() == NET_IPV4(10, 200, 1, 2),
               "static Guest IP does not match VM 0 setup")) {
        return 1;
    }

    if (!check(net_udp_send(NET_IPV4(203, 0, 113, 9), 4000, 9999,
                            message, sizeof(message) - 1) == NET_WOULD_BLOCK,
               "first UDP send should wait for gateway ARP") ||
        !check(tx_count == 1 && read_be16(tx_frames[0] + 12) == 0x0806,
               "gateway ARP request was not emitted")) {
        return 1;
    }

    uint8_t arp_reply[42];
    make_arp_reply(arp_reply);
    if (!check(queue_rx_frame(arp_reply, sizeof(arp_reply)),
               "could not enqueue ARP reply") ||
        !check(net_udp_receive(NULL, NULL, reply, sizeof(reply)) ==
                   NET_WOULD_BLOCK,
               "ARP reply should be consumed without returning a datagram") ||
        !check(net_gateway_ip() == NET_IPV4(10, 200, 1, 1),
               "gateway address is incorrect")) {
        return 1;
    }

    if (!check(net_udp_send(NET_IPV4(203, 0, 113, 9), 4000, 9999,
                            message, sizeof(message) - 1) == NET_OK,
               "UDP send failed after ARP resolution") ||
        !check(tx_count == 2 && read_be16(tx_frames[1] + 12) == 0x0800,
               "IPv4 frame was not emitted") ||
        !check(checksum_finish(checksum_add(0, tx_frames[1] + 14, 20)) == 0,
               "outgoing IPv4 checksum is invalid") ||
        !check(tx_frames[1][0] == gateway_mac[0] &&
                   tx_frames[1][5] == gateway_mac[5],
               "outbound frame is not addressed to the gateway") ||
        !check(read_be16(tx_frames[1] + 34) == 4000 &&
                   read_be16(tx_frames[1] + 36) == 9999,
               "UDP ports are incorrect")) {
        return 1;
    }

    const size_t echo_length = make_udp_echo_reply(
        rx_frames[1], message, sizeof(message) - 1);
    rx_lengths[1] = echo_length;
    rx_count = 2;
    const int received = net_udp_receive(&source_ip, &source_port,
                                         reply, sizeof(reply));
    if (!check(received == (int)(sizeof(message) - 1),
               "UDP Echo payload was not received") ||
        !check(source_ip == NET_IPV4(203, 0, 113, 9) && source_port == 9999,
               "UDP Echo peer tuple is incorrect")) {
        return 1;
    }
    for (size_t i = 0; i < sizeof(message) - 1; ++i) {
        if (!check(reply[i] == message[i], "UDP Echo payload differs")) {
            return 1;
        }
    }

    puts("network_stack_unit: ARP, IPv4/UDP TX, and UDP RX passed");
    return 0;
}
