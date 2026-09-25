#include "guest/guest_interrupts.h"

#include "virtio_mmio.h"

#include <stdint.h>

#define PIC_MASTER_COMMAND UINT16_C(0x20)
#define PIC_MASTER_DATA    UINT16_C(0x21)
#define PIC_SLAVE_COMMAND  UINT16_C(0xa0)
#define PIC_SLAVE_DATA     UINT16_C(0xa1)

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_gate) == 16, "x86-64 IDT gate size");
_Static_assert(sizeof(struct idt_pointer) == 10, "x86-64 IDTR size");

struct interrupt_frame;

static struct idt_gate idt[VIRTIO_NET_INTERRUPT_VECTOR + 1];

static void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

static uint32_t virtio_interrupt_status(void) {
    return *(volatile uint32_t*)(uintptr_t)
        (VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_REG_INTERRUPT_STATUS);
}

static void virtio_interrupt_ack(uint32_t status) {
    *(volatile uint32_t*)(uintptr_t)
        (VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_REG_INTERRUPT_ACK) = status;
}

static void __attribute__((interrupt))
virtio_net_irq(struct interrupt_frame* frame) {
    (void)frame;
    const uint32_t pending = virtio_interrupt_status();

    /* PIC EOI makes KVM deassert GSI 5 and signal the resample eventfd.
     * The following MMIO ACK lets the Host check whether more used-ring
     * work arrived while the interrupt was being handled. */
    outb(PIC_MASTER_COMMAND, UINT8_C(0x20));
    if (pending & UINT32_C(1)) virtio_interrupt_ack(UINT32_C(1));
}

void guest_interrupts_init(void) {
    const uintptr_t handler = (uintptr_t)virtio_net_irq;
    struct idt_gate* gate = &idt[VIRTIO_NET_INTERRUPT_VECTOR];
    gate->offset_low = (uint16_t)handler;
    gate->selector = UINT16_C(0x08); /* Code selector installed by the Host. */
    gate->ist = 0;
    gate->attributes = UINT8_C(0x8e); /* Present, ring 0 interrupt gate. */
    gate->offset_middle = (uint16_t)(handler >> 16);
    gate->offset_high = (uint32_t)(handler >> 32);
    gate->reserved = 0;

    const struct idt_pointer pointer = {
        (uint16_t)(sizeof(idt) - 1),
        (uint64_t)(uintptr_t)idt,
    };
    asm volatile("lidt %0" :: "m"(pointer) : "memory");

    /* Remap the 8259 PIC: IRQ 5 becomes IDT vector 0x25. */
    outb(PIC_MASTER_DATA, UINT8_C(0xff));
    outb(PIC_SLAVE_DATA, UINT8_C(0xff));
    outb(PIC_MASTER_COMMAND, UINT8_C(0x11));
    outb(PIC_SLAVE_COMMAND, UINT8_C(0x11));
    outb(PIC_MASTER_DATA, (uint8_t)VIRTIO_PIC_MASTER_VECTOR_BASE);
    outb(PIC_SLAVE_DATA, UINT8_C(0x28));
    outb(PIC_MASTER_DATA, UINT8_C(0x04));
    outb(PIC_SLAVE_DATA, UINT8_C(0x02));
    outb(PIC_MASTER_DATA, UINT8_C(0x01));
    outb(PIC_SLAVE_DATA, UINT8_C(0x01));
    outb(PIC_MASTER_DATA, (uint8_t)~(UINT8_C(1) << VIRTIO_NET_PIC_IRQ));
    outb(PIC_SLAVE_DATA, UINT8_C(0xff));
}

void guest_interrupts_enable(void) {
    asm volatile("sti" ::: "memory");
}
