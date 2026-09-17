# Virtio-net + TAP demo

This guest initializes the Virtio-MMIO network device, resolves the host TAP
gateway with ARP, and sends a small IPv4/UDP datagram to `10.200.<vm-id+1>.1:9999`.
It then enables the Guest IDT and waits for an interrupt-driven UDP echo before
halting. The final halt clears IF so the VMM can distinguish it from the
interruptible wait state.

The Guest network stack intentionally implements only Ethernet, ARP, IPv4,
and UDP. The Virtio-net transport is kept in a separate Guest driver module.
It does not implement TCP, DNS, or TLS. Virtio notifications use interrupt
vector 32 through the minimal KVM interrupt path.
