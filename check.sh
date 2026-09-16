#!/bin/bash

# is virtualisation enabled on the CPU?
#   > 0 - it is
#   = 0 - it is not
test_1=$(egrep -c '(vmx|svm)' /proc/cpuinfo)
if (( $test_1 > 0 )); then
    echo -e "\033[32m[PASSED]\033[0m Virtualisation enabled."
else
    echo -e "\033[31m[ERROR]\033[0m Virtualisation disabled."
    exit 1;
fi

# are kernel modules loaded; should see
#   kvm_intel - if Intel CPU
#   kvm_amd   - if AMD CPU
test_2=$(lsmod | grep "^kvm_")
if (( ! $? )); then
    echo -e "\033[32m[PASSED]\033[0m Found $(echo $test_2 | cut -d' ' -f1) module."
else
    echo -e "\033[31m[ERROR]\033[0m No KVM module loaded"
    exit 2;
fi              

# make sure /dev/kvm exists
ls -l /dev/kvm >/dev/null 2>/dev/null
if (( ! $? )); then
    echo -e "\033[32m[PASSED]\033[0m /dev/kvm exists."
else
    echo -e "\033[31m[ERROR]\033[0m /dev/kvm doesn't exist."
fi

