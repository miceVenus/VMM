#include "guest/guest_console.h"
#include "guest/guest_interrupts.h"
#include "guest/network_stack.h"
#include "guest/virtio_net.h"

#include <stddef.h>
#include <stdint.h>

/* Build-time peer IPv4 address; zero selects the Guest's TAP gateway. */
#ifndef TEST_UDP_ECHO_IP
#define TEST_UDP_ECHO_IP UINT32_C(0)
#endif

/* Build-time UDP port on the selected Echo peer. */
#ifndef TEST_UDP_ECHO_PORT
#define TEST_UDP_ECHO_PORT UINT16_C(9999)
#endif

static const uint8_t echo_request[] = "freestanding VirtIO-net UDP echo";

static void __attribute__((noreturn)) halt_forever(void) {
    asm volatile("cli" ::: "memory");
    for (;;) asm volatile("hlt");
}

static int bytes_equal(const uint8_t* left,
                       const uint8_t* right,
                       size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void) {
    uint8_t reply[sizeof(echo_request)];
    const uint16_t echo_server_port = (uint16_t)TEST_UDP_ECHO_PORT;

    puts("Freestanding VirtIO-net UDP echo test started\n");
    guest_interrupts_init();
    if (net_init() != NET_OK) {
        puts("Virtio-net initialization failed\n");
        halt_forever();
    }
    const uint32_t echo_server_ip = (uint32_t)TEST_UDP_ECHO_IP != 0
        ? (uint32_t)TEST_UDP_ECHO_IP : net_gateway_ip();
    guest_interrupts_enable();
    puts("Virtio-net IRQ receive and Ethernet/ARP/IPv4/UDP are ready\n");

    uint32_t progress = 0;
    for (;;) {
        const int status = net_udp_send(echo_server_ip,
                                        UINT16_C(4000),
                                        echo_server_port,
                                        echo_request,
                                        sizeof(echo_request) - 1);
        if (status == NET_OK) break;
        if (status != NET_WOULD_BLOCK) {
            puts("UDP request could not be queued\n");
            halt_forever();
        }
        (void)net_udp_receive(NULL, NULL, reply, sizeof(reply));
        asm volatile("pause" ::: "memory");
        if (++progress == UINT32_C(100000000)) {
            progress = 0;
            puts("Waiting for TAP gateway ARP / a free TX descriptor\n");
        }
    }
    puts("UDP Echo request sent; waiting for the matching response\n");

    for (;;) {
        uint32_t source_ip = 0;
        uint16_t source_port = 0;
        const int length = net_udp_receive(&source_ip,
                                           &source_port,
                                           reply,
                                           sizeof(reply));
        if (length >= 0 && source_ip == echo_server_ip &&
            source_port == echo_server_port &&
            (size_t)length == sizeof(echo_request) - 1 &&
            bytes_equal(reply, echo_request, sizeof(echo_request) - 1)) {
            puts("UDP Echo payload and peer verified successfully\n");
            halt_forever();
        }
        virtio_net_wait_for_receive();
    }
}
