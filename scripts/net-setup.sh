#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <vm-id> <uplink-interface>" >&2
    echo "Example: $0 0 wlp2s0" >&2
    exit 1
fi

vm_id="$1"
uplink="$2"

if [[ ! "$vm_id" =~ ^[0-9]+$ ]] || (( vm_id > 253 )); then
    echo "vm-id must be an integer between 0 and 253" >&2
    exit 1
fi

tap="vmtap${vm_id}"
network_octet=$((vm_id + 1))
network="10.200.${network_octet}.0/24"
gateway="10.200.${network_octet}.1/24"

for _ in $(seq 1 100); do
    if ip link show "$tap" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

if ! ip link show "$tap" >/dev/null 2>&1; then
    echo "${tap} did not appear. Start the VMM with --net." >&2
    exit 1
fi

sudo sysctl -w net.ipv4.ip_forward=1
sudo ip link set "$tap" up
sudo ip address replace "$gateway" dev "$tap"

if ! sudo iptables -C FORWARD -i "$tap" -o "$uplink" -j ACCEPT 2>/dev/null; then
    sudo iptables -A FORWARD -i "$tap" -o "$uplink" -j ACCEPT
fi
if ! sudo iptables -C FORWARD -i "$uplink" -o "$tap" \
        -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null; then
    sudo iptables -A FORWARD -i "$uplink" -o "$tap" \
        -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
fi
if ! sudo iptables -t nat -C POSTROUTING -s "$network" -o "$uplink" \
        -j MASQUERADE 2>/dev/null; then
    sudo iptables -t nat -A POSTROUTING -s "$network" -o "$uplink" -j MASQUERADE
fi

echo "${tap}: ${gateway}; guest address: 10.200.${network_octet}.2"
echo "NAT enabled through ${uplink}"
