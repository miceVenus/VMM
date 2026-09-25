#pragma once

/* Install the Virtio-net IRQ gate and unmask PIC IRQ 5. */
void guest_interrupts_init(void);

/* Called after the Virtio-net queues have been initialized. */
void guest_interrupts_enable(void);
