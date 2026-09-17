# Mini Hypervisor

This project is a small x86 userspace hypervisor built on Linux KVM. Its
focused feature is a Virtio-net device backed by a Linux TAP interface, which
allows a freestanding 64-bit Guest to exchange UDP packets with the Host and,
with NAT configured, the outside network.

The VMM creates one vCPU per VM, enters 64-bit long mode with 2 MiB paging,
loads a flat Guest image, and handles the exits used by the demo:

* `KVM_EXIT_IO` for Guest console output;
* `KVM_EXIT_MMIO` for Virtio-MMIO register accesses;
* `KVM_EXIT_HLT` for interruptible Guest waits and final shutdown.

The project intentionally does not include a Guest filesystem, file-sharing
syscalls, interactive console input, or multi-vCPU execution. These mechanisms
are outside the network-device demonstration.

## Architecture

```text
Guest application
        ↓
Guest network_stack.c
        ↓
Guest Virtio-net driver
        ↓  Virtio-MMIO + KVM_EXIT_MMIO
VMM Virtqueue device model
        ↓
Linux TAP interface (vmtap0, vmtap1, ...)
        ↓
Host routing/NAT
        ↓
Outside network
```

The Guest network stack implements Ethernet, ARP, IPv4, and UDP. The separate
Virtio-net Guest driver owns Virtio-MMIO, Virtqueues, the Guest GDT/IDT, and
frame buffers. It waits for network activity with `sti; hlt`.
After the VMM completes a Virtio RX/TX buffer, it injects interrupt vector 32
with `KVM_INTERRUPT`; the Guest ISR records the event and the main loop drains
the virtqueue. A full PIC/APIC/IOAPIC model is outside this demo. TCP, DHCP,
DNS, TLS, offloads, and multiqueue are also outside its scope.

## Usage

Build the hypervisor and the network Guest image with:

```bash
make
```

The individual build targets are:

```bash
make hypervisor  # build the VMM
make testN1      # build the Virtio-net Guest image
make tests       # build all Guest tests (currently testN1)
make clean       # remove build/ and bin/
```

Each `--vm` option describes one VM. Every VM uses one vCPU and 2 MiB paging.
Multiple `--vm` options create independent VMs and independent TAP interfaces.

```bash
./bin/mini_hypervisor.a \
  --vm=image=bin/testN1.img,mem=2 \
  --net
```

The accepted memory sizes are 2, 4, and 8 MiB. VM 0 receives TAP interface
`vmtap0`, gateway `10.200.1.1`, and Guest address `10.200.1.2`. The `testN1`
Guest sends a UDP datagram to port 9999 and briefly waits for an optional
reply on port 4000.

## Connecting TAP to the outside network

Creating and configuring TAP interfaces normally requires `CAP_NET_ADMIN`.
The setup script waits for the VMM to create the interface, assigns the
gateway address, enables forwarding, and installs a MASQUERADE rule:

```bash
sudo -v
sudo ./scripts/net-setup.sh 0 <uplink-interface> &
./bin/mini_hypervisor.a \
  --vm=image=bin/testN1.img,mem=2 \
  --net
```

For example, `<uplink-interface>` may be `wlp2s0` or `enp1s0`. A simple
host-side UDP echo listener can be used for a round-trip check:

```bash
socat -v UDP-LISTEN:9999,bind=10.200.1.1,reuseaddr,fork \
  UDP:10.200.1.2:4000
```

Remove the forwarding and NAT rules after the test:

```bash
sudo ./scripts/net-cleanup.sh 0 <uplink-interface>
```

The VMM removes its TAP interface when it exits. The MMIO register layout
follows the [Virtio 1.2 specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
and TAP provides userspace Ethernet frames as described in the
[Linux TUN/TAP documentation](https://docs.kernel.org/networking/tuntap.html).
The interrupt injection interface is documented in the
[KVM API](https://docs.kernel.org/virt/kvm/api.html).

## Prerequisites

The hypervisor requires Linux with KVM support, a C++17 compiler, Make, and
Linux KVM headers. Network mode additionally requires `/dev/net/tun`,
`ip`, `iptables`, and permission to create TAP interfaces.

```bash
ls /dev/kvm
```

If necessary, load the KVM modules:

```bash
sudo modprobe kvm
sudo modprobe kvm_intel   # or kvm_amd
```

## Project layout

```text
src/host/                Host VMM and Virtio-net device model
src/host/kvm.cpp         KVM VM, vCPU, long-mode, and Guest image setup
src/host/vcpu.cpp        KVM_RUN loop and exit dispatch
src/host/virtio_net.cpp  Virtio-net device model and interrupts
src/host/virtqueue.cpp   Host split Virtqueue implementation
src/host/tap_device.cpp  Linux TAP interface wrapper
src/guest/               Freestanding Guest code
src/guest/network_stack.c  Guest Ethernet/ARP/IPv4/UDP stack
src/guest/virtio_net_driver.c  Guest Virtio-MMIO/frame transport
src/guest/virtio_net_guest_irq.S  Guest interrupt entry point
src/guest/guest_console.c  Minimal Guest-to-Host console output
inc/host/                Host-only interfaces
inc/guest/               Guest-only interfaces
inc/virtio_mmio.h        Shared Virtio-MMIO definitions
test/testN1/            Virtio-net demonstration Guest
scripts/                TAP routing and NAT helpers
```
