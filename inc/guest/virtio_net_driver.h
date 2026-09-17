#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef GUEST_BUILD

/*
 * Lower-level Guest Virtio-net transport. It handles Virtio-MMIO, split
 * Virtqueues, Guest interrupt setup, and Ethernet-frame buffers. It does not
 * know about ARP, IPv4, UDP, or any other network protocol.
 */
/* Project Virtio-net driver status: the requested operation succeeded. */
#define VIRTIO_NET_DRIVER_OK              0
/* Project status: no queue completion is available or TX is still busy. */
#define VIRTIO_NET_DRIVER_WOULD_BLOCK    (-1)
/* Project status: the Virtio-net driver has not been initialized. */
#define VIRTIO_NET_DRIVER_NOT_INITIALIZED (-2)
/* Project status: a driver function argument or configuration is invalid. */
#define VIRTIO_NET_DRIVER_BAD_ARGUMENT   (-3)
/* Project status: the received frame does not fit the caller's buffer. */
#define VIRTIO_NET_DRIVER_FRAME_TOO_LARGE (-4)

/* Project transport buffer size in Guest RAM, including the Virtio-net header. */
#define VIRTIO_NET_DRIVER_BUFFER_SIZE       2048
/* Virtio 1.2 `struct virtio_net_hdr` size used when offloads are disabled. */
#define VIRTIO_NET_DRIVER_HEADER_SIZE       10
/* Largest Ethernet frame exposed to network_stack.c after removing that header. */
#define VIRTIO_NET_DRIVER_MAX_FRAME_SIZE \
    (VIRTIO_NET_DRIVER_BUFFER_SIZE - VIRTIO_NET_DRIVER_HEADER_SIZE)

int virtio_net_driver_init(uint8_t mac[6]);

int virtio_net_driver_send_frame(const uint8_t* frame,
                                 uint16_t frame_length);

/*
 * Copies one completed Ethernet frame into the caller's buffer and recycles
 * the corresponding RX descriptor. Returns the frame length, or a negative
 * driver status.
 */
int virtio_net_driver_receive_frame(uint8_t* frame, size_t capacity);

/* Services completed TX buffers and prepares the RX queue for polling. */
void virtio_net_driver_poll(void);

/* Waits for a device interrupt when no interrupt is already pending. */
void virtio_net_driver_wait(void);

#endif
