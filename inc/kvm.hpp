
#pragma once

#include <cstddef>
#include <stdint.h>
#include <linux/kvm.h>

#include "virtio_mmio.h"


#define PML4_OFF 0x1000
#define PDP_OFF  0x2000
#define PD_OFF   0x3000
#define GUEST_START_ADDR    0x0000
#define STACK_START_OFF     0x10000
#define GUEST_PAGE_SIZE     0x200000

struct virtio_net_device;

// PDE bitovi
#define PDE64_PRESENT (1ULL << 0)
#define PDE64_RW (1ULL << 1)
#define PDE64_USER (1ULL << 2)
#define PDE64_PS (1ULL << 7)

// CR4 i CR0
#define CR0_PE (1ULL << 0)
#define CR0_PG (1ULL << 31)
#define CR4_PAE (1ULL << 5)

#define EFER_LME (1ULL << 8)
#define EFER_LMA (1ULL << 10)


struct vm {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    uint8_t *mem_start;
    size_t mem_size;
    struct kvm_run *run;
    int run_mmap_size;
    struct kvm_sregs sregs;
    virtio_net_device* net;
};


int vm_init(struct vm &v, size_t mem_size);

void vm_destroy(struct vm &v);

int setup_long_mode(struct vm &v);

int load_guest_image(struct vm &v, const char* path);

int set_context(struct vm &v);
