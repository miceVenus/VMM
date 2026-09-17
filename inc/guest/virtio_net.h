#pragma once

#include <stdint.h>

#ifdef GUEST_BUILD

/*
 * Guest-side network API. The implementation is split between a small
 * Virtio-net transport and a minimal Ethernet/ARP/IPv4/UDP protocol stack.
 * IP addresses are represented in the human-readable order used by
 * NET_IPV4(a, b, c, d).
 */

/* Project network-stack status: the requested operation succeeded. */
#define NET_OK             0
/* Project status: a non-blocking operation cannot complete yet; try again. */
#define NET_WOULD_BLOCK   (-1)
/* Project status: net_init() has not completed successfully. */
#define NET_NOT_INITIALIZED (-2)
/* Project status: a function argument is invalid. */
#define NET_BAD_ARGUMENT  (-3)

/* Project helper that packs four IPv4 octets into the documented 32-bit
 * human-readable-order representation, e.g. NET_IPV4(10, 0, 0, 1). */
#define NET_IPV4(a, b, c, d) \
    ((((uint32_t)(a) & 0xffU) << 24) | (((uint32_t)(b) & 0xffU) << 16) | \
     (((uint32_t)(c) & 0xffU) << 8) | ((uint32_t)(d) & 0xffU))

/* Initializes the Virtio-net device and returns 0 on success. */
int net_init(void);

/* The addresses are assigned from the VM-specific MAC address. */
uint32_t net_local_ip(void);
uint32_t net_gateway_ip(void);

/*
 * Sends one UDP datagram.  If ARP resolution is still pending, the function
 * returns NET_WOULD_BLOCK; call net_poll() and retry the send.
 */
int net_udp_send(uint32_t destination_ip,
                uint16_t source_port,
                uint16_t destination_port,
                const void* payload,
                uint16_t payload_length);

/*
 * Polls RX, answers ARP, and returns one UDP payload addressed to this VM.
 * The return value is the payload length, or NET_WOULD_BLOCK when no matching
 * datagram is currently available.
 */
int net_udp_receive(uint32_t* source_ip,
                    uint16_t* source_port,
                    void* payload,
                    uint16_t payload_capacity);

/* Useful for demos that only need to service ARP/TAP traffic. */
void net_poll(void);

/* Sleeps until the Virtio device raises the Guest interrupt. */
void net_wait(void);

#endif
