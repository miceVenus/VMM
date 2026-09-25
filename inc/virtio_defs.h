#pragma once

#include <stdint.h>

/*
 * Transport-independent Virtio 1.2 definitions shared by Guest and Host.
 * Values and semantics are defined by OASIS Virtio 1.2 sections 2.1, 2.2,
 * 2.7.5, and 5.1.
 */

/* Virtio 1.2 status bits; drivers accumulate these during initialization. */
/* Driver has noticed the device. */
#define VIRTIO_STATUS_ACKNOWLEDGE UINT8_C(0x01)
/* Driver is loaded and ready to configure the device. */
#define VIRTIO_STATUS_DRIVER      UINT8_C(0x02)
/* Device initialization completed successfully. */
#define VIRTIO_STATUS_DRIVER_OK   UINT8_C(0x04)
/* Device accepted the driver's negotiated feature set. */
#define VIRTIO_STATUS_FEATURES_OK UINT8_C(0x08)
/* Device or driver detected an unrecoverable initialization/operation error. */
#define VIRTIO_STATUS_FAILED      UINT8_C(0x80)

/* Feature numbers are bit positions, not masks. */
/* Modern Virtio 1.x interface, feature bit 32. */
#define VIRTIO_F_VERSION_1 UINT32_C(32)
/* Virtio device type assigned to a network device. */
#define VIRTIO_DEVICE_ID_NET UINT32_C(1)
/* Virtio-net device supplies a MAC address in its configuration space. */
#define VIRTIO_NET_F_MAC   UINT32_C(5)
/* Modern Virtio-net header size when VERSION_1 is negotiated (includes num_buffers). */
#define VIRTIO_NET_HEADER_SIZE 12U

/* Split Virtqueue descriptor flags. */
/* Descriptor chains to the descriptor selected by its `next` field. */
#define VIRTQ_DESC_F_NEXT  UINT16_C(1)
/* Device writes data into this descriptor's buffer. */
#define VIRTQ_DESC_F_WRITE UINT16_C(2)
