#include <sys/wait.h>

#include "host/vcpu.hpp"
#include "host/cli.hpp"
#include "host/kvm.hpp"
#include "host/virtio_net.hpp"
#include "host/log.hpp"

#include <cstdio>
#include <memory>
#include <new>


#include <deque>


int child_main(args_t& myArgs) {

    int status = 0;
    char src[32];
    vm v{};
    std::unique_ptr<virtio_net> network_device;

    sprintf(src, "[VM-%d]", vm_id);

    status = vm_init(v, myArgs.vms[vm_id].memory_sz);
    switch (status) {
    case 0x00: LOG(src, "VM initialized successfully.", GREEN_PREFIX); break;
    case 0x10: LOG(src, "Couldn't open /dev/kvm.", RED_PREFIX); break;
    case 0x11: LOG(src, "KVM API mismatch.", RED_PREFIX); break;
    case 0x12: LOG(src, "KVM_CREATE_VM", RED_PREFIX); break;
    case 0x13: LOG(src, "MMAP MEM", RED_PREFIX); break;
    case 0x14: LOG(src, "KVM_SET_USER_MEMORY_REGION", RED_PREFIX); break;
    case 0x15: LOG(src, "KVM_CREATE_VCPU", RED_PREFIX); break;
    case 0x16: LOG(src, "KVM_GET_VCPU_MMAP_SIZE", RED_PREFIX); break;
    case 0x17: LOG(src, "MMAP KVM_RUN", RED_PREFIX); break;
    default:   LOG(src, "Unexpected error.", RED_PREFIX); break;
    }
    if (status != 0) goto cleanup;

    status = setup_long_mode(v);
    switch (status) {
    case 0x00: LOG(src, "Long mode setup successfully.", GREEN_PREFIX); break;
    case 0x20: LOG(src, "KVM_GET_SREGS", RED_PREFIX); break;
    case 0x22: LOG(src, "KVM_SET_SREGS", RED_PREFIX); break;
    default:   LOG(src, "Unexpected error.", RED_PREFIX); break;
    }
    if (status != 0) goto cleanup;

    status = load_guest_image(v, myArgs.vms[vm_id].image.c_str());
    switch (status) {
    case 0x00: LOG(src, "Guest image loaded successfully.", GREEN_PREFIX); break;
    case 0x30: LOG(src, "Failed to open guest image.", RED_PREFIX); break;
    case 0x31: LOG(src, "Failed to reach the EOF.", RED_PREFIX); break;
    case 0x32: LOG(src, "Failed to get size of guest image.", RED_PREFIX); break;
    case 0x33: LOG(src, "Guest image is too large.", RED_PREFIX); break;
    case 0x34: LOG(src, "Failed to read guest image.", RED_PREFIX); break;
    default:   LOG(src, "Unexpected error.", RED_PREFIX); break;
    }
    if (status != 0) goto cleanup;

    status = set_context(v);
    switch (status) {
    case 0x00: LOG(src, "Guest registers set successfully.", GREEN_PREFIX); break;
    case 0x40: LOG(src, "KVM_SET_REGS", RED_PREFIX); break;
    default:   LOG(src, "Unexpected error.", RED_PREFIX); break;
    }
    if (status != 0) goto cleanup;

    if (myArgs.network) {
        network_device.reset(new (std::nothrow) virtio_net(v, vm_id));
        if (network_device == nullptr || !network_device->initialize()) {
            LOG(src, "Couldn't create the Virtio-net TAP device.", RED_PREFIX);
            status = 0x60;
            goto cleanup;
        }
        char message[80];
        sprintf(message, "Virtio-net attached to %s.",
                network_device->tap_name().c_str());
        LOG(src, message, GREEN_PREFIX);
    }

    status = run_vcpu(v);
    switch (status) {
    case 0x00: LOG(src, "Graceful exit - HLT reached.", GREEN_PREFIX); break;
    case 0x50: LOG(src, "KVM_RUN", RED_PREFIX); break;
    case 0x51: LOG(src, "Hard exit - forceful shutdown.", RED_PREFIX); break;
    case 0x52: LOG(src, "Unexpected exit cause.", RED_PREFIX); break;
    default:   LOG(src, "Unexpected error.", RED_PREFIX); break;
    }

cleanup:
    network_device.reset();
    v.net = nullptr;
    vm_destroy(v);
    return status;
}

int main(int argc, char* argv[]) {
    char src[64];
    args_t myArgs;
    int status = read_args(argc, argv, myArgs);

    switch (status) {
    case 0: LOG("[HOST]", "Arguments read successfully.", GREEN_PREFIX); break;
    case 1: LOG("[HOST]", "Invalid argument format.", RED_PREFIX); print_help(); break;
    case 2: LOG("[HOST]", "Invalid number of arguments.", RED_PREFIX); print_help(); break;
    case 3: LOG("[HOST]", "Argument set twice.", RED_PREFIX); break;
    case 4: LOG("[HOST]", "Invalid argument value.", RED_PREFIX); break;
    case 5: return 0;
    default: LOG("[HOST]", "Unexpected argument parsing error.", RED_PREFIX); break;
    }

    if (status != 0) return status;

    for (size_t vm_index = 0; vm_index < myArgs.vms.size(); ++vm_index) {
        pid_t pid = fork();

        if (pid < 0) {
            sprintf(src, "[HOST-%zu]", vm_index);
            LOG(src, "Process not started.", RED_PREFIX);
            return -1;
        }

        if (pid == 0) {
            vm_id = static_cast<int>(vm_index);
            sprintf(src, "[VM-%d]", vm_id);
            LOG(src, "Starting process.", GREEN_PREFIX);
            return child_main(myArgs);
        }

        sprintf(src, "VM-%zu started with PID %d", vm_index, pid);
        LOG("[HOST]", src, GREEN_PREFIX);
    }

    int final_status = 0;
    int child_status = 0;
    for (size_t i = 0; i < myArgs.vms.size(); ++i) {
        const pid_t pid = wait(&child_status);
        const bool success = WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;

        sprintf(src, "Process %d returned with status: %d", pid,
                WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1);
        LOG("[HOST]", src, success ? GREEN_PREFIX : RED_PREFIX);
        if (!success) final_status = 1;
    }

    return final_status;
}
