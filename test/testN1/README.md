# Virtio-net + TAP demo

This guest initializes the Virtio-MMIO network device, resolves the host TAP
gateway with ARP, and sends a small IPv4/UDP datagram to `10.200.<vm-id+1>.1:9999`.
It then polls briefly for an optional UDP echo before halting.

The guest network library intentionally implements only Ethernet, ARP, IPv4,
and UDP. It does not implement TCP, DNS, or TLS.

