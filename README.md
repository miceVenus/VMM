# Mini Hypervisor

This x86-64 userspace VMM uses KVM for execution and Linux vhost-net as the
Virtio-net data-plane backend. Its freestanding Guest contains a small modern
Virtio-MMIO frontend and just enough Ethernet, ARP, IPv4, and UDP to exchange a
UDP Echo datagram or query a public DNS resolver through TAP. Linux on the Host
performs forwarding and NAT.

The VMM creates one vCPU per VM, enters 64-bit long mode with 2 MiB paging,
loads the flat Guest test image, and handles console output, Virtio-MMIO
control registers, and the Guest's final HLT. Queue data stays in shared Guest
RAM; KVM IOEVENTFD sends queue kicks directly to vhost-net, which exchanges
frames with TAP.

## Network path

```text
Freestanding Guest app: UDP Echo or public DNS A query
        ↓
UDP / IPv4 / ARP / Ethernet II
        ↓
Virtio-net frontend: split RX queue 0 + TX queue 1
        ↓  Virtio-MMIO control; KVM_IOEVENTFD queue kicks
Linux vhost-net ── TAP (vmtapN)
        ↓
Host IP forwarding + MASQUERADE NAT ── uplink
```

The Guest uses a static address derived from its VM number:

| VM | TAP gateway | Guest address |
|---|---|---|
| 0 | `10.200.1.1/24` | `10.200.1.2/24` |
| 1 | `10.200.2.1/24` | `10.200.2.2/24` |

Further VM addresses follow the same pattern. The Guest installs an IDT and
remaps PIC IRQ 5 to vector 0x25. KVM IRQFD delivers RX completions through
that vector; the handler acknowledges the interrupt, and the Guest drains the
RX used ring after waking. TX completions are polled. Protocol support is
small: static IPv4, gateway ARP, IPv4 header checksums, and UDP. DHCP, TCP,
IPv4 fragmentation, and general-purpose routing are not implemented.

## Build and run

Build the VMM and Guest image:

```bash
make
```

Start the VM with networking enabled:

```bash
./bin/mini_hypervisor.a \
  --vm=image=bin/testN1.img,mem=2 \
  --net
```

In another terminal, configure TAP, forwarding, and NAT. Replace `enpXsY`
with the Host's uplink interface:

```bash
sudo ./scripts/net-setup.sh 0 enpXsY
```

The Guest retries gateway ARP while the TAP setup runs. The matching cleanup
script removes the forwarding and NAT rules:

```bash
sudo ./scripts/net-cleanup.sh 0 enpXsY
```

To demonstrate external IPv4/UDP access without running an Echo server, use
the separate DNS Guest image. It asks Cloudflare's `1.1.1.1:53` for the A
record of `example.com` and prints the returned IPv4 address:

```bash
make testN1_dns
./bin/mini_hypervisor.a --vm=image=bin/testN1_dns.img,mem=2 --net
```

Run `scripts/net-setup.sh 0 enpXsY` in another terminal after `vmtap0`
appears. A successful Guest prints `Public DNS reply: example.com = ...`.
This checks the Guest UDP path through Host NAT to a public DNS service;
the Guest does not implement a general DNS resolver or TCP web access.
If `1.1.1.1:53` is unreachable from the Linux Host, rebuild with
`make -B testN1_dns DNS_SERVER_IP=0xAABBCCDD` using another reachable
IPv4 DNS resolver in dotted-octet order.

By default, the demo targets the TAP gateway at UDP port 9999. The VMM does
not run an Echo application; a UDP Echo service must be listening there for
this default test. To exercise Host NAT to a server outside the Host, rebuild
the Guest for that reachable server's IPv4 address and port:

```bash
make -B testN1 UDP_ECHO_IP=0xC000020A UDP_ECHO_PORT=9999
```

`UDP_ECHO_IP` is a numeric 32-bit IPv4 value written in dotted-octet order;
the example value represents the documentation-only address `192.0.2.10` and
must be replaced with the address of a real UDP Echo service. The Guest prints
success only after the peer address, peer port, and echoed payload all match.

`make hypervisor` builds only the VMM; `make testN1` builds the Echo image;
`make testN1_dns` builds the DNS image. `make tests` builds both and runs the
host-native protocol-stack unit test. The `mem` option currently accepts 2,
4, or 8 MiB. Each VM has one vCPU and its own TAP interface (`vmtap0`,
`vmtap1`, ...).

## Prerequisites

The VMM requires Linux with KVM, a C++17 compiler, Make, `/dev/kvm`, and
`/dev/vhost-net` when `--net` is used. Creating TAP and configuring NAT
requires the appropriate network-administration privileges and `iptables`.

```bash
ls /dev/kvm
ls /dev/vhost-net
```

## Project layout

```text
src/host/virtio_net.cpp    Virtio-MMIO control plane and vhost coordination
src/host/vhost_net.cpp     vhost-net setup and TAP backend
src/host/tap_device.cpp    TAP interface lifetime
src/guest/guest_interrupts.c  Guest IDT and PIC interrupt setup
src/guest/virtio_net.c     Freestanding Virtio-MMIO split-ring RX/TX driver
src/guest/network_stack.c  Ethernet, ARP, IPv4, and UDP processing
src/guest/guest_dma.c      Fixed identity-mapped Guest DMA arena
test/testN1/guest.c        Configurable UDP Echo verification app
test/testN1/dns_guest.c    Public DNS A-record query demo
test/network_stack_unit.c  Host-native protocol-stack unit test
scripts/net-setup.sh       TAP address, forwarding, and NAT setup
scripts/net-cleanup.sh     Forwarding and NAT cleanup
```
