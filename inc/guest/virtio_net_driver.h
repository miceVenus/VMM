#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef GUEST_BUILD

/*
 * Lower-level Guest Virtio-net transport. It handles Virtio-MMIO, split
 * Virtqueues, Guest interrupt setup, and Ethernet-frame buffers. It does not
 * know about ARP, IPv4, UDP, or any other network protocol.
 */
/*
 * Project-internal return codes for the Virtio-net Guest driver API. These
 * are not values defined by the OASIS Virtio specification or POSIX errno.
 */
/* Project convention: the driver operation succeeded. */
#define VIRTIO_NET_DRIVER_OK              0
/* Project convention: the queue or transmit buffer is temporarily unavailable; retry later. */
#define VIRTIO_NET_DRIVER_WOULD_BLOCK    (-1)
/* Project convention: the driver has not finished initialization. */
#define VIRTIO_NET_DRIVER_NOT_INITIALIZED (-2)
/* Project convention: a driver argument is invalid or device negotiation failed. */
#define VIRTIO_NET_DRIVER_BAD_ARGUMENT   (-3)
/* Project convention: the received frame exceeds the caller's destination-buffer capacity. */
#define VIRTIO_NET_DRIVER_FRAME_TOO_LARGE (-4)

/* Project convention: total capacity, in bytes, of one Guest RX/TX buffer. */
#define VIRTIO_NET_DRIVER_BUFFER_SIZE       2048
/*
 * Specification reference: OASIS Virtio 1.2 §5.1.6 defines the virtio-net
 * header. This project implements only a fixed 10-byte prefix, which both
 * sides skip before processing the Ethernet frame. This is the project's
 * current transport-layout convention, not the total size of every
 * virtio-net extended header.
 */
#define VIRTIO_NET_DRIVER_HEADER_SIZE       10
/* Project convention: maximum Ethernet-frame length that fits in one Guest buffer. */
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
