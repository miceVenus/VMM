#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <vm-id> <uplink-interface>" >&2
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

sudo iptables -D FORWARD -i "$tap" -o "$uplink" -j ACCEPT 2>/dev/null || true
sudo iptables -D FORWARD -i "$uplink" -o "$tap" \
    -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null || true
sudo iptables -t nat -D POSTROUTING -s "$network" -o "$uplink" \
    -j MASQUERADE 2>/dev/null || true

echo "Removed forwarding/NAT rules for ${tap}."
