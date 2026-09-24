# Mini Hypervisor

This x86-64 userspace VMM uses KVM for execution and Linux vhost-net as the
Virtio-net data-plane backend. Its freestanding Guest contains a small modern
Virtio-MMIO frontend and just enough Ethernet, ARP, IPv4, and UDP to exchange a
UDP Echo datagram through TAP. Linux on the Host performs forwarding and NAT.

The VMM creates one vCPU per VM, enters 64-bit long mode with 2 MiB paging,
loads the flat Guest test image, and handles console output, Virtio-MMIO
control registers, and the Guest's final HLT. Queue data stays in shared Guest
RAM; KVM IOEVENTFD sends queue kicks directly to vhost-net, which exchanges
frames with TAP.

## Network path

```text
Freestanding Guest app: UDP Echo request/reply
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

Further VM addresses follow the same pattern. The Guest frontend polls both
split rings and suppresses completion interrupts; it does not need an IDT or
interrupt handler. Its protocol support is deliberately small: static IPv4,
gateway ARP, IPv4 header checksums, and UDP. DHCP, TCP, IPv4 fragmentation,
and general-purpose routing are not implemented.

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

By default, the demo targets the TAP gateway at UDP port 9999. The VMM does
not run an Echo application; a UDP Echo service must be listening there for
this default test. To exercise Host NAT to a server outside the Host, rebuild
the Guest for that reachable server's IPv4 address and port:

```bash
make -B tests UDP_ECHO_IP=0xC000020A UDP_ECHO_PORT=9999
```

`UDP_ECHO_IP` is a numeric 32-bit IPv4 value written in dotted-octet order;
the example value represents the documentation-only address `192.0.2.10` and
must be replaced with the address of a real UDP Echo service. The Guest prints
success only after the peer address, peer port, and echoed payload all match.

`make hypervisor` builds only the VMM; `make testN1` builds the Guest image;
`make tests` builds the image and runs the host-native protocol-stack unit
test. The `mem` option currently accepts 2, 4, or 8 MiB. Each VM has one vCPU
and its own TAP interface (`vmtap0`, `vmtap1`, ...).

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
src/host/vhost.cpp         Generic Linux vhost endpoint
src/host/vhost_net.cpp     vhost-net backend bound to TAP
src/host/tap_device.cpp    TAP interface lifetime
src/guest/virtio_net.c     Freestanding Virtio-MMIO split-ring RX/TX driver
src/guest/network_stack.c  Ethernet, ARP, IPv4, and UDP processing
src/guest/guest_dma.c      Fixed identity-mapped Guest DMA arena
test/testN1/guest.c        Configurable UDP Echo verification app
test/network_stack_unit.c  Host-native protocol-stack unit test
scripts/net-setup.sh       TAP address, forwarding, and NAT setup
scripts/net-cleanup.sh     Forwarding and NAT cleanup
```
