.text
.globl virtio_net_irq_stub
.type virtio_net_irq_stub, @function
.extern virtio_net_interrupt_pending

virtio_net_irq_stub:
    movb $1, virtio_net_interrupt_pending(%rip)
    iretq

.size virtio_net_irq_stub, .-virtio_net_irq_stub
