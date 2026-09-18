#pragma once

#include <stdint.h>

/*
 * MMIO layout shared by the host device model and the guest driver.
 *
 * Comment convention: macros marked "standard" derive their values or
 * semantics from a specification; macros marked "project convention" serve
 * only this VMM's address-space layout or implementation details and are not
 * generic Virtio constants. The primary references are OASIS Virtio 1.2
 * §4.2.2 (MMIO register layout), §2.1 (status bits), §5.1 (network device),
 * and §2.7.5 (split-virtqueue descriptor flags).
 *
 * The address is intentionally outside the current guest RAM memslot, so a
 * guest access becomes KVM_EXIT_MMIO and is handled by the VMM.
 */
/*
 * Project-defined Guest Physical Address (GPA) at which the VMM exposes the
 * Virtio-MMIO device. It is outside Guest RAM so KVM reports accesses here
 * as MMIO exits to the VMM.
 */
#define VIRTIO_MMIO_BASE_GPA UINT64_C(0x01000000)

/* Project-defined size of the Virtio-MMIO address window: 4 KiB. */
#define VIRTIO_MMIO_SIZE     UINT32_C(0x1000)

/* Virtio 1.2, section "MMIO Device Configuration": the fixed "virt" magic
 * value, encoded as the little-endian 32-bit register value. */
#define VIRTIO_MMIO_MAGIC_VALUE UINT32_C(0x74726976)

/* Virtio MMIO specification version 2. */
#define VIRTIO_MMIO_VERSION     UINT32_C(2)

/* Virtio device ID 1, assigned to a network device by the Virtio spec. */
#define VIRTIO_MMIO_DEVICE_NET  UINT32_C(1)

/* Project-defined vendor identifier; the source historically labels it
 * "UMET". The numeric value is unchanged and is not a standard device ID. */
#define VIRTIO_MMIO_VENDOR_ID   UINT32_C(0x554d4554) /* historical label: "UMET" */

/* Project convention: Guest IDT vector used for Virtio-net interrupts; Virtio does not mandate this value. */
#define VIRTIO_NET_INTERRUPT_VECTOR UINT32_C(32)

/*
 * Virtio 1.2, section "MMIO Device Configuration": offsets of the common
 * Virtio-MMIO registers from VIRTIO_MMIO_BASE_GPA.
 */
/* Read-only device identification magic. */
#define VIRTIO_MMIO_REG_MAGIC_VALUE      UINT32_C(0x000)
/* Read-only Virtio-MMIO interface version. */
#define VIRTIO_MMIO_REG_VERSION          UINT32_C(0x004)
/* Read-only Virtio device type, for example network device = 1. */
#define VIRTIO_MMIO_REG_DEVICE_ID        UINT32_C(0x008)
/* Read-only vendor identifier. */
#define VIRTIO_MMIO_REG_VENDOR_ID        UINT32_C(0x00c)
/* Read-only 32-bit half of the device feature bitmap selected by SEL. */
#define VIRTIO_MMIO_REG_DEVICE_FEATURES  UINT32_C(0x010)
/* Selects the low or high 32 bits read from DEVICE_FEATURES. */
#define VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL UINT32_C(0x014)
/* Writable 32-bit half of the driver feature bitmap selected by SEL. */
#define VIRTIO_MMIO_REG_DRIVER_FEATURES  UINT32_C(0x020)
/* Selects the low or high 32 bits written to DRIVER_FEATURES. */
#define VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL UINT32_C(0x024)
/* Selects which Virtqueue subsequent queue-register accesses address. */
#define VIRTIO_MMIO_REG_QUEUE_SEL        UINT32_C(0x030)
/* Read-only maximum number of descriptors supported by the selected queue. */
#define VIRTIO_MMIO_REG_QUEUE_NUM_MAX    UINT32_C(0x034)
/* Sets the number of descriptors used by the selected queue. */
#define VIRTIO_MMIO_REG_QUEUE_NUM        UINT32_C(0x038)
/* Enables or disables the selected queue after its addresses are configured. */
#define VIRTIO_MMIO_REG_QUEUE_READY      UINT32_C(0x044)
/* Notifies the device that the driver placed work in a queue. */
#define VIRTIO_MMIO_REG_QUEUE_NOTIFY     UINT32_C(0x050)
/* Read-only pending device-interrupt bitmap. */
#define VIRTIO_MMIO_REG_INTERRUPT_STATUS UINT32_C(0x060)
/* Writing interrupt bits here acknowledges/clears those device interrupts. */
#define VIRTIO_MMIO_REG_INTERRUPT_ACK    UINT32_C(0x064)
/* Device status register used during Virtio device initialization. */
#define VIRTIO_MMIO_REG_STATUS            UINT32_C(0x070)
/* Low 32 bits of the selected Virtqueue descriptor-table address. */
#define VIRTIO_MMIO_REG_QUEUE_DESC_LOW    UINT32_C(0x080)
/* High 32 bits of the selected Virtqueue descriptor-table address. */
#define VIRTIO_MMIO_REG_QUEUE_DESC_HIGH   UINT32_C(0x084)
/* Low 32 bits of the driver/available-ring address. */
#define VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW  UINT32_C(0x090)
/* High 32 bits of the driver/available-ring address. */
#define VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH UINT32_C(0x094)
/* Low 32 bits of the device/used-ring address. */
#define VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW  UINT32_C(0x0a0)
/* High 32 bits of the device/used-ring address. */
#define VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH UINT32_C(0x0a4)
/* Configuration-generation counter; this project keeps it at zero. */
#define VIRTIO_MMIO_REG_CONFIG_GENERATION UINT32_C(0x0fc)
/* Start of the device-specific configuration space, containing the MAC. */
#define VIRTIO_MMIO_REG_CONFIG_SPACE      UINT32_C(0x100)

/*
 * Virtio 1.2 device-status bits. The driver sets these during the standard
 * ACKNOWLEDGE -> DRIVER -> FEATURES_OK -> DRIVER_OK initialization sequence.
 */
/* Driver has noticed the device. */
#define VIRTIO_STATUS_ACKNOWLEDGE UINT8_C(0x01)
/* Driver is loaded and ready to use the device. */
#define VIRTIO_STATUS_DRIVER     UINT8_C(0x02)
/* Device initialization completed successfully. */
#define VIRTIO_STATUS_DRIVER_OK  UINT8_C(0x04)
/* Driver accepted the negotiated feature set. */
#define VIRTIO_STATUS_FEATURES_OK UINT8_C(0x08)
/* Device detected an unrecoverable configuration/operation error. */
#define VIRTIO_STATUS_FAILED     UINT8_C(0x80)

/*
 * Virtio 1.2 feature-bit numbers. These are bit positions, not bit masks;
 * callers create masks with (1ULL << feature_number).
 */
/* VIRTIO_F_VERSION_1: modern Virtio 1.x interface. */
#define VIRTIO_F_VERSION_1 UINT32_C(32)
/* VIRTIO_NET_F_MAC: the device supplies a MAC in configuration space. */
#define VIRTIO_NET_F_MAC   UINT32_C(5)

/* Virtio 1.2 split Virtqueue descriptor flags. */
/* Descriptor points to another descriptor through its `next` field. */
#define VIRTQ_DESC_F_NEXT  UINT16_C(1)
/* Device writes data into the descriptor's buffer (used by RX buffers). */
#define VIRTQ_DESC_F_WRITE UINT16_C(2)
