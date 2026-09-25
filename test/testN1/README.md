# Freestanding Virtio-net network demos

The Guest configures modern Virtio-MMIO split RX/TX queues, obtains its MAC,
and uses a compact Ethernet/ARP/IPv4/UDP implementation to send a fixed UDP
payload. It reports success only after receiving the same payload from the
configured Echo peer and matching the expected peer IP and UDP port.

The default peer is the TAP gateway on port 9999; an Echo service must be
running there for the default image to complete. To test the Host NAT path,
rebuild with `make -B testN1 UDP_ECHO_IP=0xAABBCCDD UDP_ECHO_PORT=9999`, replacing
the address with a reachable external UDP Echo server. Start the VMM with
`--net`, then configure the TAP gateway and NAT using `scripts/net-setup.sh`.

The Guest handles RX completion interrupts through PIC IRQ 5 and IDT vector
0x25. It sleeps with HLT while waiting for the UDP reply and drains the RX
used ring after waking. TX descriptor completion remains polled.

## Public DNS demo

`make testN1_dns` builds `bin/testN1_dns.img` alongside the original Echo
image. Run it on a Linux KVM Host with `--net`, then configure the TAP and NAT
with `scripts/net-setup.sh 0 <uplink-interface>`. The Guest sends an A-record
query for `example.com` to `1.1.1.1:53`, checks the response source, DNS
transaction ID, and answer format, then prints the returned IPv4 address.
Successful output starts with `Public DNS reply: example.com =`.

This exercises external IPv4/UDP communication; it does not add TCP, HTTP, or
a reusable DNS resolver to the Guest. The Host's uplink must allow UDP port 53.
To use another reachable IPv4 resolver, rebuild with
`make -B testN1_dns DNS_SERVER_IP=0xAABBCCDD`.
