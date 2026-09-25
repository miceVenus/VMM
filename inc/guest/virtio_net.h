#pragma once

#include <stddef.h>
#include <stdint.h>
#include "virtio_defs.h"

#ifdef GUEST_BUILD

/* Project status: the requested operation completed successfully. */
#define VIRTIO_NET_OK 0
/* Project status: no completed RX frame or free TX descriptor is available. */
#define VIRTIO_NET_WOULD_BLOCK (-1)
/* Project status: the driver has not been initialized. */
#define VIRTIO_NET_NOT_INITIALIZED (-2)
/* Project status: an argument or device configuration is invalid. */
#define VIRTIO_NET_BAD_ARGUMENT (-3)
/* Project status: a received Ethernet frame does not fit the caller's buffer. */
#define VIRTIO_NET_FRAME_TOO_LARGE (-4)

/* Project buffer capacity; includes an Ethernet frame but not the Virtio header. */
#define VIRTIO_NET_FRAME_CAPACITY 1514U

int virtio_net_init(uint8_t mac[6]);
int virtio_net_send_frame(const uint8_t* frame, uint16_t length);
int virtio_net_receive_frame(uint8_t* frame, size_t capacity);
void virtio_net_poll(void);
void virtio_net_wait_for_receive(void);

#endif
