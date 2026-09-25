#include "host/cli.hpp"
#include "host/kvm.hpp"
#include "host/log.hpp"
#include "host/vcpu.hpp"
#include "host/virtio_net.hpp"

#include <cstdio>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const char* error_message(int status) {
    switch (status) {
    case 0x10: return "Couldn't open /dev/kvm.";
    case 0x11: return "KVM API mismatch.";
    case 0x12: return "KVM_CREATE_VM";
    case 0x13: return "MMAP MEM";
    case 0x14: return "KVM_SET_USER_MEMORY_REGION";
    case 0x15: return "KVM_CREATE_VCPU";
    case 0x16: return "KVM_GET_VCPU_MMAP_SIZE";
    case 0x17: return "MMAP KVM_RUN";
    case 0x18: return "KVM_CREATE_IRQCHIP";
    case 0x19: return "KVM_SET_GSI_ROUTING";
    case 0x20: return "KVM_GET_SREGS";
    case 0x22: return "KVM_SET_SREGS";
    case 0x30: return "Failed to open guest image.";
    case 0x31: return "Failed to reach the EOF.";
    case 0x32: return "Failed to get size of guest image.";
    case 0x33: return "Guest image is too large.";
    case 0x34: return "Failed to read guest image.";
    case 0x40: return "KVM_SET_REGS";
    case 0x50: return "KVM_RUN";
    case 0x51: return "Hard exit - forceful shutdown.";
    case 0x52: return "Unexpected exit cause.";
    default: return "Unexpected error.";
    }
}

void log_result(const char* source, int status, const char* success_message) {
    LOG(source, status == 0 ? success_message : error_message(status),
        status == 0 ? GREEN_PREFIX : RED_PREFIX);
}

int child_main(const vm_args_t& config, bool network, int vm_id) {
    char source[32];
    std::snprintf(source, sizeof(source), "[VM-%d]", vm_id);

    vm v{};
    std::unique_ptr<virtio_net> network_device;

    int status = vm_init(v, config.memory_sz);
    log_result(source, status, "VM initialized successfully.");
    if (status != 0) goto cleanup;

    status = setup_long_mode(v);
    log_result(source, status, "Long mode setup successfully.");
    if (status != 0) goto cleanup;

    status = load_guest_image(v, config.image.c_str());
    log_result(source, status, "Guest image loaded successfully.");
    if (status != 0) goto cleanup;

    status = set_context(v);
    log_result(source, status, "Guest registers set successfully.");
    if (status != 0) goto cleanup;

    if (network) {
        network_device = std::make_unique<virtio_net>(v, vm_id);
        if (!network_device->initialize()) {
            status = 0x60;
            LOG(source, "Couldn't initialize the Virtio-net vhost/TAP backend.",
                RED_PREFIX);
            goto cleanup;
        }
        char message[80];
        std::snprintf(message, sizeof(message),
                      "Virtio-net vhost backend attached to %s.",
                      network_device->tap_name().c_str());
        LOG(source, message, GREEN_PREFIX);
    }

    status = run_vcpu(v, vm_id);
    log_result(source, status, "Graceful exit - HLT reached.");

cleanup:
    network_device.reset();
    vm_destroy(v);
    return status;
}

} // namespace

int main(int argc, char* argv[]) {
    char source[64];
    args_t args;
    const int status = read_args(argc, argv, args);

    switch (status) {
    case 0: LOG("[HOST]", "Arguments read successfully.", GREEN_PREFIX); break;
    case 1: LOG("[HOST]", "Missing VM configuration.", RED_PREFIX); print_help(); break;
    case 2: LOG("[HOST]", "Invalid argument format.", RED_PREFIX); print_help(); break;
    case 3: LOG("[HOST]", "Argument set twice.", RED_PREFIX); break;
    case 4: LOG("[HOST]", "Invalid argument value.", RED_PREFIX); break;
    case 5: return 0;
    default: LOG("[HOST]", "Unexpected argument parsing error.", RED_PREFIX); break;
    }

    if (status != 0) return status;

    for (size_t vm_index = 0; vm_index < args.vms.size(); ++vm_index) {
        const pid_t pid = fork();

        if (pid < 0) {
            std::snprintf(source, sizeof(source), "[HOST-%zu]", vm_index);
            LOG(source, "Process not started.", RED_PREFIX);
            return -1;
        }

        if (pid == 0) {
            const int vm_id = static_cast<int>(vm_index);
            std::snprintf(source, sizeof(source), "[VM-%d]", vm_id);
            LOG(source, "Starting process.", GREEN_PREFIX);
            return child_main(args.vms[vm_index], args.network, vm_id);
        }

        std::snprintf(source, sizeof(source), "VM-%zu started with PID %d",
                      vm_index, pid);
        LOG("[HOST]", source, GREEN_PREFIX);
    }

    int final_status = 0;
    int child_status = 0;
    for (size_t i = 0; i < args.vms.size(); ++i) {
        const pid_t pid = wait(&child_status);
        const bool success = WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;

        std::snprintf(source, sizeof(source), "Process %d returned with status: %d",
                      pid, WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1);
        LOG("[HOST]", source, success ? GREEN_PREFIX : RED_PREFIX);
        if (!success) final_status = 1;
    }

    return final_status;
}
