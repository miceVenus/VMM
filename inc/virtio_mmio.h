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
/* Project convention: Guest physical base address selected by the VMM for the Virtio-MMIO device. */
#define VIRTIO_MMIO_BASE_GPA UINT64_C(0x01000000)
/* Project convention: MMIO window size reserved by the VMM; the specification defines register offsets and the configuration-space start, not this window's size or base. */
#define VIRTIO_MMIO_SIZE     UINT32_C(0x1000)

/* Standard: OASIS Virtio 1.2 §4.2.2; identifies this as a Virtio-MMIO device. */
#define VIRTIO_MMIO_MAGIC_VALUE UINT32_C(0x74726976)
/* Standard: OASIS Virtio 1.2 §4.2.2; version 2 for the non-legacy MMIO transport. */
#define VIRTIO_MMIO_VERSION     UINT32_C(2)
/* Standard: OASIS Virtio 1.2 §5.1.1; device ID 1 denotes a virtual network card. */
#define VIRTIO_MMIO_DEVICE_NET  UINT32_C(1)
/* Project convention: custom value returned in the vendor field; Virtio does not assign this value. */
#define VIRTIO_MMIO_VENDOR_ID   UINT32_C(0x554d4554) /* "UMET" */

/* Project convention: Guest IDT vector used for Virtio-net interrupts; Virtio does not mandate this value. */
#define VIRTIO_NET_INTERRUPT_VECTOR UINT32_C(32)

/*
 * Standard: OASIS Virtio 1.2 §4.2.2; the following macros are fixed
 * register offsets relative to the MMIO base. They cover device discovery,
 * feature negotiation, virtqueue configuration, notifications, and interrupt acknowledgement.
 */
/* Standard: MagicValue, the read-only device magic-value register. */
#define VIRTIO_MMIO_REG_MAGIC_VALUE      UINT32_C(0x000)
/* Standard: Version, the read-only Virtio-MMIO version register. */
#define VIRTIO_MMIO_REG_VERSION          UINT32_C(0x004)
/* Standard: DeviceID, the read-only Virtio subsystem device-type register. */
#define VIRTIO_MMIO_REG_DEVICE_ID        UINT32_C(0x008)
/* Standard: VendorID, the read-only Virtio subsystem vendor-identifier register. */
#define VIRTIO_MMIO_REG_VENDOR_ID        UINT32_C(0x00c)
/* Standard: DeviceFeatures, reads 32 device-supported feature bits. */
#define VIRTIO_MMIO_REG_DEVICE_FEATURES  UINT32_C(0x010)
/* Standard: DeviceFeaturesSel, selects the 32-bit group exposed through DeviceFeatures. */
#define VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL UINT32_C(0x014)
/* Standard: DriverFeatures, writes 32 feature bits accepted by the driver. */
#define VIRTIO_MMIO_REG_DRIVER_FEATURES  UINT32_C(0x020)
/* Standard: DriverFeaturesSel, selects the 32-bit group exposed through DriverFeatures. */
#define VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL UINT32_C(0x024)
/* Standard: QueueSel, selects the virtqueue targeted by subsequent operations. */
#define VIRTIO_MMIO_REG_QUEUE_SEL        UINT32_C(0x030)
/* Standard: QueueNumMax, reads the maximum number of elements in the selected virtqueue. */
#define VIRTIO_MMIO_REG_QUEUE_NUM_MAX    UINT32_C(0x034)
/* Standard: QueueNum, writes the size used by the driver for the selected virtqueue. */
#define VIRTIO_MMIO_REG_QUEUE_NUM        UINT32_C(0x038)
/* Standard: QueueReady, enables or disables the selected virtqueue. */
#define VIRTIO_MMIO_REG_QUEUE_READY      UINT32_C(0x044)
/* Standard: QueueNotify, notifies the device about new buffers in the selected virtqueue. */
#define VIRTIO_MMIO_REG_QUEUE_NOTIFY     UINT32_C(0x050)
/* Standard: InterruptStatus, reads the triggered used-buffer/configuration event bits. */
#define VIRTIO_MMIO_REG_INTERRUPT_STATUS UINT32_C(0x060)
/* Standard: InterruptACK, acknowledges handled device-interrupt events with a bit mask. */
#define VIRTIO_MMIO_REG_INTERRUPT_ACK    UINT32_C(0x064)
/* Standard: Status, reads/writes device-initialization status; writing 0 resets the device. */
#define VIRTIO_MMIO_REG_STATUS            UINT32_C(0x070)
/* Standard: low 32 bits of the selected virtqueue Descriptor Area address. */
#define VIRTIO_MMIO_REG_QUEUE_DESC_LOW    UINT32_C(0x080)
/* Standard: high 32 bits of the selected virtqueue Descriptor Area address. */
#define VIRTIO_MMIO_REG_QUEUE_DESC_HIGH   UINT32_C(0x084)
/* Standard: low 32 bits of the selected virtqueue Driver Area (Available Ring) address. */
#define VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW  UINT32_C(0x090)
/* Standard: high 32 bits of the selected virtqueue Driver Area (Available Ring) address. */
#define VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH UINT32_C(0x094)
/* Standard: low 32 bits of the selected virtqueue Device Area (Used Ring) address. */
#define VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW  UINT32_C(0x0a0)
/* Standard: high 32 bits of the selected virtqueue Device Area (Used Ring) address. */
#define VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH UINT32_C(0x0a4)
/* Standard: configuration-space generation value used to detect changes during a read. */
#define VIRTIO_MMIO_REG_CONFIG_GENERATION UINT32_C(0x0fc)
/* Standard: start of device-specific configuration space; Virtio-net exposes its MAC address here. */
#define VIRTIO_MMIO_REG_CONFIG_SPACE      UINT32_C(0x100)

/*
 * Standard: OASIS Virtio 1.2 §2.1; status bits used during device
 * initialization. Each macro is a bit mask, not an independent enum value.
 */
/* Standard: ACKNOWLEDGE, the Guest has discovered and recognized the Virtio device. */
#define VIRTIO_STATUS_ACKNOWLEDGE UINT8_C(0x01)
/* Standard: DRIVER, the Guest knows how to drive the device. */
#define VIRTIO_STATUS_DRIVER     UINT8_C(0x02)
/* Standard: DRIVER_OK, driver initialization is complete and the device is ready. */
#define VIRTIO_STATUS_DRIVER_OK  UINT8_C(0x04)
/* Standard: FEATURES_OK, feature negotiation is complete and the selected features are accepted. */
#define VIRTIO_STATUS_FEATURES_OK UINT8_C(0x08)
/* Standard: FAILED, the Guest driver encountered an error and gave up on the device. */
#define VIRTIO_STATUS_FAILED     UINT8_C(0x80)

/* Standard: OASIS Virtio 1.2 §6; device-independent feature-bit number for version compatibility. */
#define VIRTIO_F_VERSION_1 UINT32_C(32)
/* Standard: OASIS Virtio 1.2 §5.1.3; network-device feature-bit number indicating a supplied MAC address. */
#define VIRTIO_NET_F_MAC   UINT32_C(5)

/* Standard: OASIS Virtio 1.2 §2.7.5; split-virtqueue descriptor flag bits. */
/* Standard: VIRTQ_DESC_F_NEXT, another descriptor follows through next. */
#define VIRTQ_DESC_F_NEXT  UINT16_C(1)
/* Standard: VIRTQ_DESC_F_WRITE, the device may write data to this buffer. */
#define VIRTQ_DESC_F_WRITE UINT16_C(2)
