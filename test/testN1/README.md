# Freestanding Virtio-net UDP Echo test

The Guest configures modern Virtio-MMIO split RX/TX queues, obtains its MAC,
and uses a compact Ethernet/ARP/IPv4/UDP implementation to send a fixed UDP
payload. It reports success only after receiving the same payload from the
configured Echo peer and matching the expected peer IP and UDP port.

The default peer is the TAP gateway on port 9999; an Echo service must be
running there for the default image to complete. To test the Host NAT path,
rebuild with `make -B tests UDP_ECHO_IP=0xAABBCCDD UDP_ECHO_PORT=9999`, replacing
the address with a reachable external UDP Echo server. Start the VMM with
`--net`, then configure the TAP gateway and NAT using `scripts/net-setup.sh`.

The RX and TX queues are polled and completion interrupts are suppressed, so
the Guest does not install an IDT or interrupt handler.
