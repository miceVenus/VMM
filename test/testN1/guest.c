#include "guest/guest_console.h"
#include "guest/virtio_net.h"
#include <stddef.h>

static void __attribute__((noreturn)) halt_forever(void) {
    asm volatile("cli" ::: "memory");
    for (;;) asm volatile("hlt");
}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void) {
    static const char message[] = "hello from the guest";
    char reply[64];

    puts("Virtio-net demo started\n");
    if (net_init() != NET_OK) {
        puts("Virtio-net initialization failed\n");
        halt_forever();
    }
    puts("Virtio-net device is ready\n");

    /* The first call emits an ARP request; retry after polling RX. */
    int send_status = NET_WOULD_BLOCK;
    for (int attempt = 0; attempt < 20000 && send_status != NET_OK; ++attempt) {
        send_status = net_udp_send(net_gateway_ip(),
                                   4000,
                                   9999,
                                   message,
                                   sizeof(message) - 1);
        if (send_status == NET_WOULD_BLOCK) net_poll();
    }

    if (send_status == NET_OK) {
        puts("UDP datagram sent; waiting for an optional echo\n");
    } else {
        puts("ARP/TAP network is not ready\n");
        halt_forever();
    }

    for (;;) {
        const int received = net_udp_receive(NULL, NULL, reply, sizeof(reply) - 1);
        if (received >= 0) {
            reply[received] = '\0';
            puts("UDP reply: ");
            puts(reply);
            puts("\n");
            halt_forever();
        }

        net_wait();
    }
}
