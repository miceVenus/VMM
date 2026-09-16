#include "vcpu.hpp"

#include "log.hpp"
#include "virtio_net.hpp"

#include <linux/kvm.h>
#include <sys/ioctl.h>

#include <cstdio>

int vm_id;

int run_vcpu(struct vm &v) {
    char vm_src[32];
    char guest_output[101];
    size_t output_length = 0;
    int status = 0;
    bool stop = false;

    sprintf(vm_src, "[VM-%d]", vm_id);

    auto flush_output = [&]() {
        if (output_length == 0) return;

        guest_output[output_length] = '\0';
        LOG(vm_src, guest_output, NORMAL_PREFIX);
        output_length = 0;
    };

    while (!stop) {
        if (ioctl(v.vcpu_fd, KVM_RUN, 0) != 0) {
            perror("KVM_RUN failed");
            status = 0x50;
            break;
        }

        struct kvm_run *run = v.run;

        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            if (run->io.direction != KVM_EXIT_IO_OUT ||
                run->io.port != CIO_PORT || run->io.size != 1) {
                fprintf(stderr, "%s Unexpected I/O exit: port=0x%x direction=%u size=%u\n",
                        vm_src,
                        run->io.port,
                        run->io.direction,
                        run->io.size);
                status = 0x52;
                stop = true;
                break;
            }

            {
                const char *data = reinterpret_cast<const char *>(run) +
                                   run->io.data_offset;
                for (uint32_t i = 0; i < run->io.count; ++i) {
                    const char c = data[i];
                    if (output_length < sizeof(guest_output) - 1) {
                        guest_output[output_length++] = c;
                    }
                    if (c == '\n' || output_length == sizeof(guest_output) - 1) {
                        flush_output();
                    }
                }
            }
            break;

        case KVM_EXIT_MMIO:
            if (v.net && virtio_net_handle_mmio(v, *run)) {
                break;
            }

            fprintf(stderr, "%s Unexpected MMIO exit: address=0x%llx len=%u\n",
                    vm_src,
                    (unsigned long long)run->mmio.phys_addr,
                    run->mmio.len);
            status = 0x52;
            stop = true;
            break;

        case KVM_EXIT_HLT:
            stop = true;
            break;

        case KVM_EXIT_SHUTDOWN: {
            struct kvm_regs regs;
            if (ioctl(v.vcpu_fd, KVM_GET_REGS, &regs) == 0) {
                printf("RIP=0x%.16llx RSP=0x%.16llx RAX=0x%.16llx\n",
                       (unsigned long long)regs.rip,
                       (unsigned long long)regs.rsp,
                       (unsigned long long)regs.rax);
            }
            status = 0x51;
            stop = true;
            break;
        }

        default:
            fprintf(stderr, "%s Unexpected exit cause: %u\n",
                    vm_src, run->exit_reason);
            status = 0x52;
            stop = true;
            break;
        }
    }

    flush_output();
    return status;
}
