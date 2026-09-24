#pragma once

#include <stdint.h>

#ifdef GUEST_BUILD

/* Project status: the requested operation completed successfully. */
#define NET_OK 0
/* Project status: poll again; ARP, RX, or TX work is not ready yet. */
#define NET_WOULD_BLOCK (-1)
/* Project status: net_init() has not completed successfully. */
#define NET_NOT_INITIALIZED (-2)
/* Project status: a function argument is invalid. */
#define NET_BAD_ARGUMENT (-3)

/* Project helper: pack dotted IPv4 octets into a host-order 32-bit value. */
#define NET_IPV4(a, b, c, d) \
    ((((uint32_t)(a) & 0xffU) << 24) | (((uint32_t)(b) & 0xffU) << 16) | \
     (((uint32_t)(c) & 0xffU) << 8) | ((uint32_t)(d) & 0xffU))

int net_init(void);
uint32_t net_local_ip(void);
uint32_t net_gateway_ip(void);

int net_udp_send(uint32_t destination_ip,
                 uint16_t source_port,
                 uint16_t destination_port,
                 const void* payload,
                 uint16_t payload_length);

int net_udp_receive(uint32_t* source_ip,
                    uint16_t* source_port,
                    void* payload,
                    uint16_t payload_capacity);

/* Processes queued Ethernet frames and reclaims completed TX buffers. */
void net_poll(void);

#endif
